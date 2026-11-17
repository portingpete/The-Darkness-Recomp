//! Basic DualSense reports. Layout/compatibility reference: SDL's PS5 HIDAPI
//! driver, pinned and credited in THIRD_PARTY_NOTICES.md. No advanced effects.

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Gamepad {
    pub buttons: u16,
    pub left_trigger: u8,
    pub right_trigger: u8,
    pub left_x: i16,
    pub left_y: i16,
    pub right_x: i16,
    pub right_y: i16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Transport {
    Usb,
    Bluetooth,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Report {
    pub pad: Gamepad,
    pub transport: Transport,
    pub enhanced: bool,
}

pub fn crc32(prefix: u8, bytes: &[u8]) -> u32 {
    crc_bytes(std::iter::once(prefix).chain(bytes.iter().copied()))
}
fn crc_bytes(bytes: impl IntoIterator<Item = u8>) -> u32 {
    let mut crc = !0u32;
    for byte in bytes {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}
fn axis(v: u8, invert: bool) -> i16 {
    let d = i32::from(v) - 128;
    let value = if invert {
        if d >= 0 {
            -d * 32768 / 127
        } else {
            -d * 32767 / 128
        }
    } else if d >= 0 {
        d * 32767 / 127
    } else {
        d * 32768 / 128
    };
    value as i16
}
fn buttons(face: u8, extra: u8, system: u8) -> u16 {
    const HAT: [u16; 8] = [1, 1 | 8, 8, 8 | 2, 2, 2 | 4, 4, 4 | 1];
    let mut b = HAT.get(usize::from(face & 15)).copied().unwrap_or(0);
    for (mask, xbox) in [
        (0x10, 0x4000),
        (0x20, 0x1000),
        (0x40, 0x2000),
        (0x80, 0x8000),
    ] {
        if face & mask != 0 {
            b |= xbox;
        }
    }
    for (mask, xbox) in [
        (1, 0x100),
        (2, 0x200),
        (0x10, 0x20),
        (0x20, 0x10),
        (0x40, 0x40),
        (0x80, 0x80),
    ] {
        if extra & mask != 0 {
            b |= xbox;
        }
    }
    // Create and touchpad click both provide the original Back/Select action.
    // PS and microphone buttons keep their system meaning.
    if system & 2 != 0 {
        b |= 0x20;
    }
    b
}
pub fn parse(data: &[u8]) -> Option<Report> {
    let (p, simple, transport) = match data.first().copied()? {
        1 if data.len() == 10 || data.len() == 78 => (&data[1..], true, Transport::Bluetooth),
        1 if data.len() == 64 => (&data[1..], false, Transport::Usb),
        0x31 if data.len() == 78 => {
            if crc32(0xa1, &data[..74]) != u32::from_le_bytes(data[74..78].try_into().ok()?) {
                return None;
            }
            (&data[2..], false, Transport::Bluetooth)
        }
        _ => return None,
    };
    let (face, extra, system, lt, rt) = if simple {
        (p[4], p[5], p[6], p[7], p[8])
    } else {
        (p[7], p[8], p[9], p[4], p[5])
    };
    Some(Report {
        pad: Gamepad {
            buttons: buttons(face, extra, system),
            left_trigger: lt,
            right_trigger: rt,
            left_x: axis(p[0], false),
            left_y: axis(p[1], true),
            right_x: axis(p[2], false),
            right_y: axis(p[3], true),
        },
        transport,
        enhanced: !simple,
    })
}
pub fn rumble(transport: Transport, left: u16, right: u16, start: bool) -> Vec<u8> {
    let (len, offset) = match transport {
        Transport::Usb => (48, 1),
        Transport::Bluetooth => (78, 3),
    };
    let mut data = vec![0; len];
    if transport == Transport::Usb {
        data[0] = 2;
    } else {
        data[0] = 0x31;
        data[2] = 0x10;
    }
    // Legacy rumble emulation works on older firmware too. Match SDL's reduced
    // Xbox-strength compatibility path; all audio/LED/trigger validity bits stay off.
    data[offset] = if start {
        2
    } else if left != 0 || right != 0 {
        3
    } else {
        0
    };
    if !start {
        data[offset + 2] = (right >> 9) as u8;
        data[offset + 3] = (left >> 9) as u8;
    }
    if transport == Transport::Bluetooth {
        let crc = crc32(0xa2, &data[..74]);
        data[74..78].copy_from_slice(&crc.to_le_bytes());
    }
    data
}

#[cfg(test)]
mod tests {
    use super::*;
    fn usb() -> [u8; 64] {
        let mut b = [0; 64];
        b[0] = 1;
        b[1..5].fill(128);
        b[8] = 8;
        b
    }
    #[test]
    fn crc_standard_vector() {
        assert_eq!(crc_bytes(b"123456789".iter().copied()), 0xcbf4_3926);
    }
    #[test]
    fn neutral_and_axis_extremes() {
        let mut b = usb();
        assert_eq!(parse(&b).unwrap().pad, Gamepad::default());
        b[1] = 0;
        b[2] = 0;
        b[3] = 255;
        b[4] = 255;
        b[5] = 255;
        b[6] = 123;
        let p = parse(&b).unwrap().pad;
        assert_eq!(
            (p.left_x, p.left_y, p.right_x, p.right_y),
            (-32768, 32767, 32767, -32768)
        );
        assert_eq!((p.left_trigger, p.right_trigger), (255, 123));
    }
    #[test]
    fn faces_shoulders_and_menu() {
        for (sony, xbox) in [
            (0x10, 0x4000),
            (0x20, 0x1000),
            (0x40, 0x2000),
            (0x80, 0x8000),
        ] {
            let mut b = usb();
            b[8] = sony | 8;
            assert_eq!(parse(&b).unwrap().pad.buttons, xbox);
        }
        for (sony, xbox) in [
            (1, 0x100),
            (2, 0x200),
            (0x10, 0x20),
            (0x20, 0x10),
            (0x40, 0x40),
            (0x80, 0x80),
        ] {
            let mut b = usb();
            b[9] = sony;
            assert_eq!(parse(&b).unwrap().pad.buttons, xbox);
        }
        let mut b = usb();
        b[10] = 2;
        assert_eq!(parse(&b).unwrap().pad.buttons, 0x20);
        b[10] = 1 | 4;
        assert_eq!(parse(&b).unwrap().pad.buttons, 0);
    }
    #[test]
    fn dpad_all_directions() {
        for (hat, expected) in [1, 9, 8, 10, 2, 6, 4, 5, 0, 0, 0, 0, 0, 0, 0, 0]
            .into_iter()
            .enumerate()
        {
            let mut b = usb();
            b[8] = hat as u8;
            assert_eq!(parse(&b).unwrap().pad.buttons, expected);
        }
    }
    #[test]
    fn bluetooth_simple_and_extended() {
        let simple = [1, 128, 128, 128, 128, 0x28, 0x20, 0, 40, 90];
        let p = parse(&simple).unwrap();
        assert_eq!(p.pad.buttons, 0x1010);
        assert_eq!(p.pad.left_trigger, 40);
        assert!(!p.enhanced);
        let mut padded = [0; 78];
        padded[..10].copy_from_slice(&simple);
        assert_eq!(parse(&padded).unwrap(), p);
        let mut bt = [0; 78];
        bt[0] = 0x31;
        bt[2..6].fill(128);
        bt[6] = 40;
        bt[7] = 90;
        bt[9] = 0x28;
        bt[10] = 0x20;
        let crc = crc32(0xa1, &bt[..74]);
        bt[74..].copy_from_slice(&crc.to_le_bytes());
        let full = parse(&bt).unwrap();
        assert_eq!(full.pad, p.pad);
        assert!(full.enhanced);
        bt[20] ^= 1;
        assert!(parse(&bt).is_none());
    }
    #[test]
    fn malformed_reports_never_index_outside_packet() {
        for n in 0..100 {
            let b = vec![0x31; n];
            assert!(parse(&b).is_none());
        }
        let mut b = usb();
        b[0] = 7;
        assert!(parse(&b).is_none());
    }
    #[test]
    fn rumble_packets_only_claim_rumble() {
        for t in [Transport::Usb, Transport::Bluetooth] {
            let (len, o) = if t == Transport::Usb {
                (48, 1)
            } else {
                (78, 3)
            };
            let b = rumble(t, 65535, 32768, false);
            assert_eq!(b.len(), len);
            assert_eq!(&b[o..o + 4], &[3, 0, 64, 127]);
            assert!(b[o + 4..o + 47].iter().all(|v| *v == 0));
            let stop = rumble(t, 0, 0, false);
            assert!(stop[o..o + 47].iter().all(|v| *v == 0));
            let start = rumble(t, 0, 0, true);
            assert_eq!(start[o], 2);
            if t == Transport::Bluetooth {
                assert_eq!(
                    u32::from_le_bytes(b[74..].try_into().unwrap()),
                    crc32(0xa2, &b[..74])
                );
            }
        }
    }
}
