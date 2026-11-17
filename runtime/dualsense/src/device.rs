use crate::protocol::{self, Gamepad, Transport};
use hidapi::{HidApi, HidDevice};
use std::{
    ffi::CString,
    time::{Duration, Instant},
};

struct Device {
    path: CString,
    handle: HidDevice,
    pad: Gamepad,
    valid: bool,
    transport: Transport,
    enhanced: bool,
    last: Instant,
    motors: (u16, u16),
    effects_active: bool,
    output: Option<crate::output::Output>,
    write_errors: u8,
    write_logs: u8,
}
impl Device {
    fn poll(&mut self) -> bool {
        let mut bytes = [0; 128];
        // Bound work, while draining the complete normal Windows HID queue.
        for _ in 0..256 {
            match self.handle.read_timeout(&mut bytes, 0) {
                Ok(0) => break,
                Ok(n) => {
                    if let Some(r) = protocol::parse(&bytes[..n]) {
                        if !self.valid {
                            eprintln!("[DualSense] connected transport={:?}", r.transport);
                        }
                        self.pad = r.pad;
                        self.transport = r.transport;
                        self.enhanced |= r.enhanced;
                        self.valid = true;
                        self.last = Instant::now();
                    }
                }
                Err(_) => return false,
            }
        }
        self.last.elapsed() < Duration::from_millis(500)
    }
    fn send(&mut self, bytes: &[u8]) -> bool {
        let result = (|| {
            if self.output.is_none() {
                self.output = Some(crate::output::Output::open(&self.path)?);
            }
            self.output.as_mut().unwrap().write(bytes)
        })();
        match result {
            // Windows HID completion can name the collection's maximum output
            // size (547 over Bluetooth), even for a submitted 78-byte report.
            Ok(n) if n >= bytes.len() => {
                if self.write_logs < 3 {
                    eprintln!(
                        "[DualSense] output report={:#04x} submitted={} completion={n}",
                        bytes[0],
                        bytes.len()
                    );
                    self.write_logs += 1;
                }
                true
            }
            other => {
                if self.write_errors < 4 {
                    eprintln!(
                        "[DualSense] output failed expected={} result={other:?}",
                        bytes.len()
                    );
                    self.write_errors += 1;
                }
                false
            }
        }
    }
    fn rumble(&mut self, left: u16, right: u16) -> bool {
        if !self.valid {
            return false;
        }
        if self.motors == (left, right) && (!self.effects_active || left != 0 || right != 0) {
            return true;
        }
        if self.motors == (0, 0) && (left != 0 || right != 0) {
            if !self.send(&protocol::rumble(self.transport, 0, 0, true)) {
                return false;
            }
            self.effects_active = true;
            if !self.enhanced {
                std::thread::sleep(Duration::from_millis(10));
            }
            self.enhanced = true;
        }
        if !self.send(&protocol::rumble(self.transport, left, right, false)) {
            return false;
        }
        self.motors = (left, right);
        self.effects_active = left != 0 || right != 0;
        true
    }
}
impl Drop for Device {
    fn drop(&mut self) {
        if self.effects_active {
            let _ = self.rumble(0, 0);
        }
    }
}
pub struct Context {
    devices: [Option<Device>; 4],
    api: HidApi,
    discovery: crate::discovery::Discovery,
    initial_paths: Option<Vec<CString>>,
}
fn controller_paths(api: &HidApi) -> Vec<CString> {
    api.device_list()
        .filter(|d| {
            d.vendor_id() == 0x054c
                && matches!(d.product_id(), 0x0ce6 | 0x0df2)
                && d.usage_page() == 1
                && d.usage() == 5
        })
        .map(|d| d.path().to_owned())
        .collect()
}
impl Context {
    pub fn new() -> hidapi::HidResult<Self> {
        // One startup enumeration preserves immediate controller detection.
        // Subsequent scans own their separate HidApi on the discovery thread.
        let api = HidApi::new()?;
        let initial_paths = Some(controller_paths(&api));
        let mut scanner: Option<HidApi> = None;
        let discovery = crate::discovery::Discovery::spawn(
            move || {
                if let Some(api) = scanner.as_mut() {
                    api.refresh_devices().ok()?;
                    Some(controller_paths(api))
                } else {
                    let api = HidApi::new().ok()?;
                    let paths = controller_paths(&api);
                    scanner = Some(api);
                    Some(paths)
                }
            },
            Duration::from_secs(1),
        )
        .map_err(|error| hidapi::HidError::IoError { error })?;
        Ok(Self {
            api,
            devices: std::array::from_fn(|_| None),
            discovery,
            initial_paths,
        })
    }
    fn discover(&mut self) {
        let Some(paths) = self
            .initial_paths
            .take()
            .or_else(|| self.discovery.latest())
        else {
            return;
        };
        // Vacant slots are selected before removals so a replacement cannot
        // inherit an old pad's connection, held input or queued vibration.
        let mut vacant: Vec<usize> = self
            .devices
            .iter()
            .enumerate()
            .filter_map(|(i, d)| d.is_none().then_some(i))
            .collect();
        for d in &mut self.devices {
            if d.as_ref().is_some_and(|v| !paths.contains(&v.path)) {
                *d = None;
            }
        }
        for path in paths {
            if self.devices.iter().flatten().any(|d| d.path == path) {
                continue;
            }
            if vacant.is_empty() {
                break;
            }
            if let Ok(handle) = self.api.open_path(&path) {
                let slot = vacant.remove(0);
                self.devices[slot] = Some(Device {
                    path,
                    handle,
                    pad: Gamepad::default(),
                    valid: false,
                    transport: Transport::Usb,
                    enhanced: false,
                    last: Instant::now(),
                    motors: (0, 0),
                    effects_active: false,
                    output: None,
                    write_errors: 0,
                    write_logs: 0,
                });
            }
        }
    }
    pub fn state(&mut self, slot: usize) -> Option<Gamepad> {
        self.discover();
        let device = self.devices.get_mut(slot)?.as_mut()?;
        if !device.poll() {
            self.devices[slot] = None;
            eprintln!("[DualSense] disconnected slot={slot}");
            return None;
        }
        device.valid.then_some(device.pad)
    }
    pub fn rumble(&mut self, slot: usize, left: u16, right: u16) -> bool {
        self.devices
            .get_mut(slot)
            .and_then(Option::as_mut)
            .is_some_and(|d| d.rumble(left, right))
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn context_coexists_with_an_initialized_hidapi() {
        let _existing = hidapi::HidApi::new().unwrap();
        let context = super::Context::new().unwrap();
        drop(context);
    }
}
