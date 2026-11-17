//! Recover the extra horizontal camera lag allowed by retail wall collision.

/// The gameplay look caller passes a frame-sized minimum. Comparing two
/// separately sampled clocks against that full interval skips otherwise valid
/// input updates. Retain a small scaled minimum to bound command traffic while
/// accepting normal 30–360 Hz updates, including ordinary clock jitter.
#[no_mangle]
pub extern "C" fn dark_controller_look_minimum(requested: f64, clock_scale: f32) -> f64 {
    if !requested.is_finite()
        || requested <= 0.0
        || !clock_scale.is_finite()
        || clock_scale <= 0.0
    {
        return requested;
    }
    requested.min(f64::from(clock_scale) / 480.0)
}

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
    fn look_updates_survive_frame_clock_jitter_without_removing_the_rate_bound() {
        for fps in [30, 60, 90, 120, 144, 240, 360] {
            for scale in [0.25_f32, 1.0, 4.0] {
                let frame = 1.0 / f64::from(fps);
                let minimum = dark_controller_look_minimum(frame * f64::from(scale), scale);
                assert!(frame * 0.9 * f64::from(scale) > minimum);
                assert!((1.0 / 1000.0) * f64::from(scale) <= minimum);
            }
        }
    }

    #[test]
    fn smaller_authored_look_minima_and_invalid_clocks_are_preserved() {
        assert_eq!(dark_controller_look_minimum(0.0001, 1.0), 0.0001);
        for scale in [f32::NAN, f32::INFINITY, -1.0, 0.0] {
            assert_eq!(dark_controller_look_minimum(0.05, scale), 0.05);
        }
        for requested in [f64::INFINITY, -0.01, 0.0] {
            assert_eq!(dark_controller_look_minimum(requested, 1.0), requested);
        }
        assert!(dark_controller_look_minimum(f64::NAN, 1.0).is_nan());
    }

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
