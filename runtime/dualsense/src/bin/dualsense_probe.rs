#[cfg(windows)]
fn main() {
    use std::time::{Duration, Instant};
    let args: Vec<String> = std::env::args().collect();
    let seconds = args
        .windows(2)
        .find(|a| a[0] == "--seconds")
        .and_then(|a| a[1].parse::<u64>().ok())
        .unwrap_or(10)
        .clamp(1, 120);
    let rumble = args.iter().any(|v| v == "--rumble");
    if args.iter().any(|v| v == "--diagnose-output") {
        let api = hidapi::HidApi::new().expect("HID enumeration");
        for d in api.device_list().filter(|d| {
            d.vendor_id() == 0x054c
                && matches!(d.product_id(), 0x0ce6 | 0x0df2)
                && d.usage_page() == 1
                && d.usage() == 5
        }) {
            let handle = d.open_device(&api).expect("Open controller");
            let transport = if matches!(d.bus_type(), hidapi::BusType::Bluetooth) {
                dark_dualsense::protocol::Transport::Bluetooth
            } else {
                dark_dualsense::protocol::Transport::Usb
            };
            let stop = dark_dualsense::protocol::rumble(transport, 0, 0, false);
            println!(
                "Zero-rumble diagnostic: requested={} result={:?}",
                stop.len(),
                handle.write(&stop)
            );
            let mut exact =
                dark_dualsense::output::Output::open(d.path()).expect("Open exact output");
            println!(
                "Exact-size zero-rumble diagnostic: requested={} result={:?}",
                stop.len(),
                exact.write(&stop)
            );
        }
        return;
    }
    if args.iter().any(|v| v == "--enumerate") {
        let api = hidapi::HidApi::new().expect("HID enumeration");
        for d in api.device_list().filter(|d| d.vendor_id() == 0x054c) {
            println!(
                "Sony HID product={:04x} usage={:04x}:{:04x} bus={:?}",
                d.product_id(),
                d.usage_page(),
                d.usage(),
                d.bus_type()
            );
        }
    }
    let mut input = dark_dualsense::Context::new().expect("HID initialization");
    let mut previous = [None; 4];
    let until = Instant::now() + Duration::from_secs(seconds);
    let mut tested = false;
    let mut seen = false;
    while Instant::now() < until {
        for (slot, prev) in previous.iter_mut().enumerate() {
            let pad = input.state(slot);
            seen |= pad.is_some();
            if pad != *prev {
                println!("slot={slot} state={pad:?}");
                *prev = pad;
            }
            if rumble && !tested && pad.is_some() {
                let start = input.rumble(slot, 24000, 12000);
                std::thread::sleep(Duration::from_millis(250));
                let stop = input.rumble(slot, 0, 0);
                println!("rumble start={start} stop={stop}");
                tested = true;
            }
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    println!("connected_seen={seen}");
    if !seen {
        std::process::exit(2);
    }
}
#[cfg(not(windows))]
fn main() {
    eprintln!("The native HID probe requires Windows.");
}
