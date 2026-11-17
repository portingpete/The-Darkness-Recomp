//! Recover the extra horizontal camera lag allowed by retail wall collision.

/// The retail camera removes its collision grace after 0.5 seconds. Contract
/// the previous offset continuously during that grace, before the cap returns;
/// the caller still runs the original camera ray and vertical constraints.
#[no_mangle]
pub extern "C" fn dark_movement_camera_lag_limit(previous: f32, ordinary: f32, dt: f32) -> f32 {
    if !previous.is_finite()
        || !ordinary.is_finite()
        || !dt.is_finite()
        || ordinary < 0.0
        || dt <= 0.0
        || dt > 0.1
        || previous <= ordinary
    {
        return ordinary;
    }
    let excess = (previous - ordinary) * (-12.0 * dt).exp();
    ordinary + if excess < 0.05 { 0.0 } else { excess }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn collision_grace_does_not_end_with_a_full_offset_snap() {
        // The recorded eye jumped ~27 units with <1 unit of body movement.
        let limit = dark_movement_camera_lag_limit(31.0, 4.0, 1.0 / 360.0);
        assert!(limit > 30.0 && limit < 31.0);
        assert_eq!(dark_movement_camera_lag_limit(3.0, 4.0, 1.0 / 360.0), 4.0);
    }

    #[test]
    fn recovery_has_the_same_duration_at_different_frame_rates() {
        for fps in [30, 60, 120, 144, 240, 360] {
            let mut distance = 31.0;
            for _ in 0..fps / 2 {
                let next = dark_movement_camera_lag_limit(distance, 4.0, 1.0 / fps as f32);
                assert!(next >= 4.0 && next <= distance);
                distance = next;
            }
            assert!((distance - (4.0 + 27.0 * (-6.0_f32).exp())).abs() < 0.001);
            for _ in 0..fps {
                distance = dark_movement_camera_lag_limit(distance, 4.0, 1.0 / fps as f32);
            }
            assert_eq!(distance, 4.0);
        }
    }

    #[test]
    fn invalid_history_and_clock_values_keep_the_retail_limit() {
        for previous in [f32::NAN, f32::INFINITY, -1.0, 0.0, 4.0] {
            assert_eq!(dark_movement_camera_lag_limit(previous, 4.0, 0.01), 4.0);
        }
        for dt in [f32::NAN, f32::INFINITY, -0.01, 0.0, 0.101] {
            assert_eq!(dark_movement_camera_lag_limit(31.0, 4.0, dt), 4.0);
        }
    }
}
