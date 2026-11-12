//! Horizontal movement in reference units per second.
//! The C++ bridge converts The Darkness's displacement-per-tick representation
//! and solves at each motor interval while cancelling the downstream control gain.

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Velocity {
    pub x: f32,
    pub y: f32,
}

#[repr(C)]
pub struct Vertical {
    pub jump: f32,
    pub gravity: f32,
}

#[no_mangle]
pub extern "C" fn dark_movement_vertical(style: u32) -> Vertical {
    match style {
        // Softer gravity with nearly the same apex as the former 300/1200 arc.
        1 => Vertical {
            jump: 245.0,
            gravity: 800.0,
        },
        _ => Vertical {
            jump: 0.0,
            gravity: 0.0,
        },
    }
}

#[no_mangle]
pub extern "C" fn dark_movement_gravity(style: u32, vertical_velocity: f32) -> f32 {
    if style != 1 || !vertical_velocity.is_finite() {
        return dark_movement_vertical(style).gravity;
    }
    // Ease both sides of the apex without a sign-triggered gravity jump.
    // Velocity is in world units/second; ordinary rise/fall remains unchanged
    // outside the last 40 UPS approaching or leaving the top.
    let phase = if vertical_velocity < 0.0 {
        600.0
    } else {
        800.0
    };
    let t = (vertical_velocity.abs() / 40.0).min(1.0);
    let blend = t * t * (3.0 - 2.0 * t);
    240.0 + (phase - 240.0) * blend
}

#[no_mangle]
pub extern "C" fn dark_movement_limit_fall(style: u32, next_velocity: f32) -> f32 {
    if style == 1 && next_velocity.is_finite() {
        next_velocity.max(-160.0)
    } else {
        next_velocity
    }
}

impl Velocity {
    fn length(self) -> f32 {
        self.x.hypot(self.y)
    }
    fn scale(self, value: f32) -> Self {
        Self {
            x: self.x * value,
            y: self.y * value,
        }
    }
}

/// Responsive alternate movement; the host retains collision and support motion.
#[no_mangle]
pub extern "C" fn dark_movement_step(
    style: u32,
    grounded: u32,
    vx: f32,
    vy: f32,
    wx: f32,
    wy: f32,
    max_speed: f32,
    dt: f32,
) -> Velocity {
    let original = Velocity { x: vx, y: vy };
    if style != 1
        || ![vx, vy, wx, wy, max_speed, dt]
            .iter()
            .all(|v| v.is_finite())
        || max_speed <= 0.0
        || dt <= 0.0
        || dt > 0.1
    {
        return original;
    }
    let mut wish = Velocity { x: wx, y: wy };
    let length = wish.length();
    if length > max_speed {
        wish = wish.scale(max_speed / length);
    }
    alternate(original, wish, max_speed, dt, grounded != 0)
}

fn alternate(current: Velocity, wish: Velocity, maximum: f32, dt: f32, ground: bool) -> Velocity {
    let wish_length = wish.length();
    if !ground && wish_length == 0.0 {
        return current;
    }
    let difference = Velocity {
        x: wish.x - current.x,
        y: wish.y - current.y,
    };
    let distance = difference.length();
    // Quick starts, decisive braking, and softer steering in the air.
    let response = if !ground {
        8.0
    } else if wish_length == 0.0 {
        40.0
    } else {
        30.0
    };
    let change = response * maximum * dt;
    if distance <= change {
        return wish;
    }
    let fraction = change / distance;
    Velocity {
        x: current.x + difference.x * fraction,
        y: current.y + difference.y * fraction,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn near(actual: f32, expected: f32) {
        assert!((actual - expected).abs() < 0.0001, "{actual} != {expected}");
    }
    #[test]
    fn starts_reverses_and_brakes_at_reduced_speed() {
        let v = dark_movement_step(1, 1, 0.0, 0.0, 198.0, 0.0, 198.0, 1.0 / 30.0);
        near(v.x, 198.0);
        let stopped = dark_movement_step(1, 1, v.x, v.y, 0.0, 0.0, 198.0, 1.0 / 30.0);
        near(stopped.x, 0.0);
        let reversed = dark_movement_step(1, 1, v.x, v.y, -198.0, 0.0, 198.0, 1.0 / 30.0);
        near(reversed.x, 0.0);
        let reversed =
            dark_movement_step(1, 1, reversed.x, reversed.y, -198.0, 0.0, 198.0, 1.0 / 30.0);
        near(reversed.x, -198.0);
    }
    #[test]
    fn braking_uses_actual_fractional_motor_time() {
        let dt = 1.0 / 30.0;
        let mut velocity = Velocity { x: 198.0, y: 0.0 };
        for fraction in [0.1171875, 0.4453125, 0.4375] {
            let next =
                dark_movement_step(1, 1, velocity.x, velocity.y, 0.0, 0.0, 198.0, dt * fraction);
            let control = (next.x - velocity.x) / (40.0 * fraction);
            velocity.x += control * (40.0 * dt) * fraction / dt;
        }
        near(velocity.x, 0.0);
    }
    #[test]
    fn air_steering_and_neutral_momentum_survive() {
        let v = dark_movement_step(1, 0, 0.0, 0.0, 198.0, 0.0, 198.0, 1.0 / 30.0);
        near(v.x, 52.8);
        let neutral = dark_movement_step(1, 0, v.x, v.y, 0.0, 0.0, 198.0, 1.0 / 30.0);
        near(neutral.x, v.x);
    }
    #[test]
    fn softer_gravity_keeps_height_with_longer_airtime() {
        let profile = dark_movement_vertical(1);
        near(profile.jump, 245.0);
        near(profile.gravity, 800.0);
        near(
            profile.jump * profile.jump / (2.0 * profile.gravity),
            37.515625,
        );
        near(2.0 * profile.jump / profile.gravity, 0.6125);
        near(dark_movement_vertical(0).jump, 0.0);
        near(dark_movement_vertical(2).jump, 0.0);
    }
    #[test]
    fn descent_is_gentler_without_changing_ascent() {
        near(dark_movement_gravity(1, 245.0), 800.0);
        near(dark_movement_gravity(1, 0.0), 240.0);
        near(dark_movement_gravity(1, -100.0), 600.0);
        near(dark_movement_limit_fall(1, 245.0), 245.0);
        near(dark_movement_limit_fall(1, -120.0), -120.0);
        near(dark_movement_limit_fall(1, -220.0), -160.0);
        near(dark_movement_limit_fall(0, -220.0), -220.0);
        // Same ascent height/time; a 600-gravity descent accelerates to 160,
        // then traverses the remaining height at that limit.
        let height = 245.0 * 245.0 / 1600.0;
        let accelerate_distance = 160.0 * 160.0 / 1200.0;
        let descent = 160.0 / 600.0 + (height - accelerate_distance) / 160.0;
        assert!(descent > 0.36 && descent < 0.38);
    }
    #[test]
    fn apex_gravity_is_continuous_and_eases_to_both_phases() {
        near(dark_movement_gravity(1, 60.0), 800.0);
        near(dark_movement_gravity(1, 20.0), 520.0);
        near(dark_movement_gravity(1, 0.0), 240.0);
        near(dark_movement_gravity(1, -20.0), 420.0);
        near(dark_movement_gravity(1, -60.0), 600.0);
        near(
            dark_movement_gravity(1, 0.001),
            dark_movement_gravity(1, -0.001),
        );
        assert!(dark_movement_gravity(1, 39.0) < 800.0);
        near(dark_movement_gravity(0, -30.0), 0.0);
    }
    #[test]
    fn diagonal_commands_are_normalized() {
        let v = dark_movement_step(1, 1, 0.0, 0.0, 198.0, 198.0, 198.0, 1.0 / 30.0);
        near(v.length(), 198.0);
        near(v.x, v.y);
    }
    #[test]
    fn original_removed_and_invalid_inputs_are_inert() {
        for (style, dt) in [
            (0, 0.03),
            (2, 0.03),
            (3, 0.03),
            (1, 0.0),
            (1, -1.0),
            (1, 0.2),
            (1, f32::NAN),
        ] {
            let v = dark_movement_step(style, 1, 3.0, -2.0, 198.0, 0.0, 198.0, dt);
            assert_eq!((v.x, v.y), (3.0, -2.0));
        }
    }
}
