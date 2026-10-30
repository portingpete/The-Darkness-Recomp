//! Per-thread wake channels; guest predicates and memory stay in C++.
//! Capture before the predicate, arm before waiting, finish before rechecking.
#![cfg_attr(not(test), no_std)]
use core::{ffi::c_void, sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering::SeqCst}};
const SLOTS: usize = 64;
const INVALID: u32 = u32::MAX;
#[repr(C)]
#[derive(Default)]
pub struct Token { generation: u32, slot: u32 }
static GENERATION: AtomicU32 = AtomicU32::new(0);
static ARMED: AtomicU64 = AtomicU64::new(0);
static NEXT_SLOT: AtomicU32 = AtomicU32::new(0);
static EVENTS: [AtomicUsize; SLOTS] = [const { AtomicUsize::new(0) }; SLOTS];
#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateEventW(attributes: *const c_void, manual: i32, initial: i32, name: *const u16) -> *mut c_void;
    fn SetEvent(handle: *mut c_void) -> i32;
}
/// Called once per native thread. Published handles are bounded, process-lifetime
/// objects: never close/reuse a handle while a notifier might still hold it.
#[no_mangle]
pub extern "C" fn dark_resource_wait_register() -> u32 {
    let Ok(slot) = NEXT_SLOT.fetch_update(SeqCst, SeqCst,
        |n| if n < SLOTS as u32 { Some(n + 1) } else { None }) else { return INVALID; };
    let event = unsafe { CreateEventW(core::ptr::null(), 0, 0, core::ptr::null()) } as usize;
    // A failed reservation stays unused. Each native thread caches failure,
    // and original timed waits remain available even after capacity is full.
    if event == 0 { return INVALID; }
    EVENTS[slot as usize].store(event, SeqCst);
    slot
}
/// Valid exclusively owned token, and this native thread's registration slot.
#[no_mangle]
pub unsafe extern "C" fn dark_resource_wait_begin(token: *mut Token, slot: u32) -> usize {
    let Some(token) = (unsafe { token.as_mut() }) else { return 0; };
    token.slot = slot;
    token.generation = GENERATION.load(SeqCst);
    EVENTS.get(slot as usize).map_or(0, |event| event.load(SeqCst))
}
/// Each thread sets only its own bit. SC store/load pairs prevent missed wakes.
#[no_mangle]
pub unsafe extern "C" fn dark_resource_wait_arm(token: *const Token) -> u32 {
    let Some(token) = (unsafe { token.as_ref() }) else { return 0; };
    if token.slot >= SLOTS as u32 { return 0; }
    ARMED.fetch_or(1u64 << token.slot, SeqCst);
    u32::from(GENERATION.load(SeqCst) == token.generation)
}
#[no_mangle]
pub unsafe extern "C" fn dark_resource_wait_finish(token: *mut Token) {
    if let Some(token) = unsafe { token.as_mut() } {
        if token.slot < SLOTS as u32 { ARMED.fetch_and(!(1u64 << token.slot), SeqCst); }
        token.generation = GENERATION.load(SeqCst);
    }
}
/// Call after the original job's normal return, after busy-byte clear and lock
/// release. Notifications are progress hints; only the guest decides completion.
#[no_mangle]
pub extern "C" fn dark_resource_wait_notify() {
    GENERATION.fetch_add(1, SeqCst);
    let mut mask = ARMED.load(SeqCst);
    while mask != 0 {
        let slot = mask.trailing_zeros() as usize;
        let event = EVENTS[slot].load(SeqCst);
        if event != 0 { unsafe { SetEvent(event as *mut c_void); } }
        mask &= mask - 1;
    }
}
#[cfg(not(test))]
mod panic_runtime {
    // Keep this in its own codegen unit so another Rust static library can
    // supply the same abort handler without pulling a duplicate definition.
    #[panic_handler]
    fn panic(_: &core::panic::PanicInfo) -> ! {
        unsafe extern "C" { fn abort() -> !; }
        unsafe { abort() }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
    #[test]
    fn notification_before_arm_is_not_lost() {
        let _guard = LOCK.lock().unwrap();
        let mut t = Token::default();
        assert_ne!(unsafe { dark_resource_wait_begin(&mut t, dark_resource_wait_register()) }, 0);
        dark_resource_wait_notify();
        assert_eq!(unsafe { dark_resource_wait_arm(&t) }, 0);
        unsafe { dark_resource_wait_finish(&mut t); }
        assert_eq!(unsafe { dark_resource_wait_arm(&t) }, 1);
        unsafe { dark_resource_wait_finish(&mut t); }
    }
    #[test]
    fn finishing_one_waiter_preserves_another() {
        let _guard = LOCK.lock().unwrap();
        let (mut a, mut b) = (Token::default(), Token::default());
        unsafe {
            dark_resource_wait_begin(&mut a, dark_resource_wait_register());
            dark_resource_wait_begin(&mut b, dark_resource_wait_register());
            assert_ne!(a.slot, b.slot);
            assert_eq!(dark_resource_wait_arm(&a), 1);
            assert_eq!(dark_resource_wait_arm(&b), 1);
            dark_resource_wait_finish(&mut a);
            assert_eq!(ARMED.load(SeqCst) & (1 << b.slot), 1 << b.slot);
            dark_resource_wait_notify();
            dark_resource_wait_finish(&mut b);
        }
    }
    #[test]
    fn invalid_registration_falls_back() {
        let _guard = LOCK.lock().unwrap();
        let mut t = Token::default();
        assert_eq!(unsafe { dark_resource_wait_begin(&mut t, INVALID) }, 0);
        assert_eq!(unsafe { dark_resource_wait_arm(&t) }, 0);
        unsafe { dark_resource_wait_finish(&mut t); }
        assert_eq!(unsafe { dark_resource_wait_begin(core::ptr::null_mut(), INVALID) }, 0);
    }
    #[test]
    fn generation_wrap_observes_progress() {
        let _guard = LOCK.lock().unwrap();
        GENERATION.store(u32::MAX, SeqCst);
        let mut t = Token::default();
        unsafe { dark_resource_wait_begin(&mut t, dark_resource_wait_register()); }
        dark_resource_wait_notify();
        assert_eq!(unsafe { dark_resource_wait_arm(&t) }, 0);
        unsafe { dark_resource_wait_finish(&mut t); }
    }
}
