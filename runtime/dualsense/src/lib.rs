#[cfg(windows)]
mod device;
#[cfg(windows)]
mod discovery;
#[cfg(windows)]
#[doc(hidden)]
pub mod output;
pub mod protocol;
#[cfg(windows)]
pub use device::Context;
use protocol::Gamepad;
use std::ffi::c_void;

// Device I/O handles stay on the native input worker; the discovery thread
// shares only owned paths. Serialize FFI calls and destroy on that I/O thread.
#[no_mangle]
pub extern "C" fn dark_dualsense_create() -> *mut c_void {
    #[cfg(windows)]
    {
        return Context::new()
            .map(|v| Box::into_raw(Box::new(v)).cast())
            .unwrap_or(std::ptr::null_mut());
    }
    #[cfg(not(windows))]
    {
        std::ptr::null_mut()
    }
}
#[no_mangle]
pub unsafe extern "C" fn dark_dualsense_destroy(context: *mut c_void) {
    #[cfg(windows)]
    if !context.is_null() {
        drop(Box::from_raw(context.cast::<Context>()));
    }
    #[cfg(not(windows))]
    let _ = context;
}
#[no_mangle]
pub unsafe extern "C" fn dark_dualsense_state(
    context: *mut c_void,
    slot: u32,
    output: *mut Gamepad,
) -> u32 {
    if context.is_null() || output.is_null() || slot >= 4 {
        return 1167;
    }
    #[cfg(windows)]
    if let Some(pad) = (&mut *context.cast::<Context>()).state(slot as usize) {
        output.write(pad);
        return 0;
    }
    1167
}
#[no_mangle]
pub unsafe extern "C" fn dark_dualsense_rumble(
    context: *mut c_void,
    slot: u32,
    left: u16,
    right: u16,
) -> u32 {
    if context.is_null() || slot >= 4 {
        return 1167;
    }
    #[cfg(windows)]
    {
        return if (&mut *context.cast::<Context>()).rumble(slot as usize, left, right) {
            0
        } else {
            1167
        };
    }
    #[cfg(not(windows))]
    {
        let _ = (left, right);
        1167
    }
}
