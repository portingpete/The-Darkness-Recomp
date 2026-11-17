//! Keep simultaneous absolute-axis updates from filling one retail input tick.

#[cfg(test)]
fn coalesce(packet: &mut [u8], incoming: &[u8]) -> bool {
    coalesce_timed(packet, incoming, false)
}

fn coalesce_timed(packet: &mut [u8], incoming: &[u8], relative_time: bool) -> bool {
    if packet.len() < 12
        || packet.len() > 256
        || usize::from(packet[1]) + 2 != packet.len()
        || incoming.len() != 10
        || incoming[0] != 3
        || incoming[1] != 6
    {
        return false;
    }
    let mut offset = 2;
    let mut last = 2;
    while offset < packet.len() {
        if offset + 4 > packet.len() {
            return false;
        }
        let end = offset + 4 + usize::from(packet[offset + 1]);
        if end > packet.len() {
            return false;
        }
        last = offset;
        offset = end;
    }
    if packet.len() - last != 10
        || packet[last] != 3
        || packet[last + 1] != 6
        || if relative_time {
            incoming[2] != 0
        } else {
            packet[last + 2] != incoming[2]
        }
    {
        return false;
    }
    // Type 3 assigns an absolute movement vector. No simulation time elapses
    // between these two records. Preserve the final vector AND its sequence
    // for the retail acknowledgement search. Never cross another event.
    let elapsed = packet[last + 2];
    packet[last..].copy_from_slice(incoming);
    if relative_time {
        // Transport records carry elapsed milliseconds, rather than absolute
        // tick fractions. Retain the elapsed time leading into this group.
        packet[last + 2] = elapsed;
    }
    true
}

/// Caller supplies a readable/writable retail packet span and ten readable
/// incoming command bytes. The incoming command may overlap the packet.
#[no_mangle]
pub unsafe extern "C" fn dark_movement_coalesce_command(
    packet: *mut u8,
    packet_bytes: u32,
    command: *const u8,
) -> u32 {
    coalesce_ffi(packet, packet_bytes, command, false)
}

/// Same pointer contract as dark_movement_coalesce_command; transport records
/// use elapsed milliseconds and only a zero-duration successor may replace one.
#[no_mangle]
pub unsafe extern "C" fn dark_movement_coalesce_relative_command(
    packet: *mut u8,
    packet_bytes: u32,
    command: *const u8,
) -> u32 {
    coalesce_ffi(packet, packet_bytes, command, true)
}

unsafe fn coalesce_ffi(
    packet: *mut u8,
    packet_bytes: u32,
    command: *const u8,
    relative_time: bool,
) -> u32 {
    if packet.is_null() || command.is_null() || !(12..=256).contains(&packet_bytes) {
        return 0;
    }
    let mut incoming = [0; 10];
    std::ptr::copy(command, incoming.as_mut_ptr(), incoming.len());
    u32::from(coalesce_timed(
        std::slice::from_raw_parts_mut(packet, packet_bytes as usize),
        &incoming,
        relative_time,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn packet(records: &[u8]) -> Vec<u8> {
        let mut output = vec![17, records.len() as u8];
        output.extend_from_slice(records);
        output
    }

    #[test]
    fn final_absolute_vector_and_wrapped_sequence_replace_intermediate_axes() {
        let mut output = packet(&[3, 6, 15, 255, 0, 1, 0, 0, 0, 0]);
        let incoming = [3, 6, 15, 0, 0, 1, 128, 255, 12, 0];
        assert!(coalesce(&mut output, &incoming));
        assert_eq!(&output[..2], &[17, 10]);
        assert_eq!(&output[2..], &incoming);
    }

    #[test]
    fn transport_replacements_preserve_elapsed_time_and_final_sequence() {
        let mut output = packet(&[3, 6, 3, 255, 0, 1, 0, 0, 0, 0]);
        let incoming = [3, 6, 0, 0, 0, 1, 128, 255, 12, 0];
        assert!(coalesce_timed(&mut output, &incoming, true));
        assert_eq!(output[4], 3);
        assert_eq!(output[5], 0);
        assert_eq!(&output[6..], &incoming[4..]);
        let before = output.clone();
        let mut later = incoming;
        later[2] = 1;
        assert!(!coalesce_timed(&mut output, &later, true));
        assert_eq!(output, before);
        let mut look = incoming;
        look[0] = 2;
        assert!(!coalesce_timed(&mut output, &look, true));
        assert_eq!(output, before);
    }

    #[test]
    fn different_times_relative_look_and_button_edges_are_barriers() {
        let movement = [3, 6, 15, 3, 0, 1, 128, 0, 0, 0];
        for records in [
            vec![3, 6, 14, 2, 0, 1, 0, 0, 0, 0],
            vec![2, 6, 15, 2, 0, 1, 0, 0, 0, 0],
            vec![3, 6, 15, 1, 0, 1, 0, 0, 0, 0, 1, 4, 15, 2, 1, 0, 0, 0],
            vec![3, 6, 15, 1, 0, 1, 0, 0, 0, 0, 0, 1, 15, 2, 127],
        ] {
            let mut output = packet(&records);
            let before = output.clone();
            assert!(!coalesce(&mut output, &movement));
            assert_eq!(output, before);
        }
        let mut output = packet(&movement);
        let before = output.clone();
        for tag in [0, 1, 2] {
            let mut other = movement;
            other[0] = tag;
            assert!(!coalesce(&mut output, &other));
            assert_eq!(output, before);
        }
    }

    #[test]
    fn malformed_lengths_never_mutate_the_packet() {
        let incoming = [3, 6, 15, 2, 0, 1, 0, 0, 0, 0];
        for mut output in [
            vec![],
            vec![0, 0],
            vec![0, 10, 3, 20, 15, 1, 0, 0, 0, 0, 0, 0],
            vec![0, 9, 3, 6, 15, 1, 0, 0, 0, 0, 0, 0],
        ] {
            let before = output.clone();
            assert!(!coalesce(&mut output, &incoming));
            assert_eq!(output, before);
        }
    }

    #[test]
    fn all_tick_fractions_retain_partial_and_negative_axes() {
        for fraction in 0..=128 {
            for axis in [-256_i16, -180, -1, 0, 1, 180, 256] {
                let mut output = packet(&[3, 6, fraction, 19, 0, 0, 0, 0, 0, 0]);
                let mut incoming = [3, 6, fraction, 20, 0, 0, 0, 0, 0, 0];
                incoming[4..6].copy_from_slice(&axis.to_le_bytes());
                incoming[6..8].copy_from_slice(&(-axis).to_le_bytes());
                assert!(coalesce(&mut output, &incoming));
                assert_eq!(&output[2..], &incoming);
            }
        }
    }
}
