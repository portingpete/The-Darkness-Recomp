#pragma once

namespace DarkRecomp::Native {

inline constexpr float kOriginalFov = 0.0f;
inline constexpr float kMinimumConfiguredFov = 60.0f;
inline constexpr float kMaximumConfiguredFov = 120.0f;
inline constexpr double kFovReferenceAspect = 16.0 / 9.0;

// Horizontal degrees at 16:9, independently of the current render dimensions.
// Zero preserves the guest camera. Safe to read/write while guest threads run.
float fieldOfViewSetting() noexcept;
// Accepts zero or finite values in [60, 120]. Invalid input leaves the setting
// unchanged; the UI/config loader can report failure rather than silently clamp.
bool setFieldOfViewSetting(float degrees) noexcept;
bool isValidConfiguredHorizontalFovDegrees(float degrees) noexcept;

// Preserve vertical FOV while converting horizontal degrees between aspects.
// Inputs must be finite, 0 < degrees < 180, and aspects > 0. Invalid input
// returns NaN. This helper does not consult the atomic runtime setting.
double horizontalFovAtAspect(double degrees, double fromAspect,
                             double toAspect) noexcept;

// 8275EEF8's perspective path (viewport+0x100 byte == 0), with unit viewport
// scales and square pixels: tan(horizontal/2) = (aspect/referenceAspect) *
// tan(guestDegrees/2). Guest degrees are at +0x104, reference aspect at +0x108.
double guestFovToHorizontal16By9(double guestDegrees,
                                double referenceAspect) noexcept;
double horizontal16By9ToGuestFov(double horizontalDegrees,
                                double referenceAspect) noexcept;

// Scale each original frame's tan(FOV/2) relative to an explicitly verified,
// unzoomed gameplay baseline expressed as horizontal degrees at 16:9.
// The current guest angle may be a zoom/cutscene value outside [60, 120].
// This preserves its tangent ratio to the baseline; it does not overwrite it
// with a fixed angle. Never derive the baseline from the first sampled frame,
// and never feed a previously adjusted result back as originalGuestDegrees.
// Zero, invalid inputs, or a result outside the guest's [0.1, 179] range return
// originalGuestDegrees unchanged. No camera identity or baseline is inferred.
float relativeGuestFovDegrees(float originalGuestDegrees,
                              float originalHorizontal16By9Baseline,
                              float configuredHorizontal16By9) noexcept;

// fov_camera.cpp applies this math at the scoped player camera query, using a
// baseline captured during player initialization/property setup. It never
// samples the first live frame or replaces the shared projection matrix.

} // namespace DarkRecomp::Native
