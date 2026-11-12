# Alternate Movement

Alternate Movement is experimental; intermittent contact/camera snags are not
fully resolved. Options > Controls > Movement Style has two choices: Original (the default)
and Alternate Movement. The latter replaces the former DOOM option. Its current
speeds are run 198 units/second and walk 99, increased 10% from 180/90. Forward,
backward and strafe share the same cap, and diagonal input is normalized.
Crouching halves speed. Alt activates Walk; Shift retains Zoom. The unused
HL2-only Sprint binding and the Half-Life 2 controller have been removed.

F5 > Player Speed is a session-only developer slider from 0.25x to 4x, with
the 1x default at the slider's center line. It is applied once to local
prediction as well as the server. Jump and gravity are unchanged. Speed stays
in developer options; Controls contains only the movement-style choice.
The Video Settings FOV slider places Original
at the left minimum and 120 degrees at the right maximum. Explicit existing FOV
settings remain intact until edited; the slider uses the retail 70-degree guest
baseline (converted to horizontal 16:9) for its unzoomed Original endpoint.

## Response and integration

The Rust solver moves velocity toward the requested vector with ground
acceleration of 30 times maximum speed per second, neutral braking of 40 times
maximum speed per second, and air steering of 8 times maximum speed per second.
Neutral airborne input preserves horizontal momentum. These are adaptation
values rather than an exact port of another game's controller.

The C++/PPC bridge reads collision-resolved actor velocity on each query and
converts its per-tick displacement to units/second. The original motor can
split one server tick into several intervals. The solver uses dt times the
actual interval fraction; the returned correction cancels both the retail
40*dt control gain and the subsequent fractional integration. Support motion
and collision remain in the original controller. No independent host velocity
accumulator is retained. Sustained airborne movement reaches the common
completion hook at 82135BF8 even when retail skips ground support work.

The local camera renders prediction copies of the player. During their retail
player-state callback at 821C9100, they now use the same movement, jump, gravity
and fall limit as the authoritative body. Eligibility is limited to the active
client's local-player ID, exact callback actor/state, matching state class and
valid owner/world links. Prediction uses its client's timestep; authoritative
movement uses the server's timestep. Nested callbacks restore the previous
scope, and changing the movement option invalidates it. Unscoped copies and
other actors retain retail handling.

The upward jump retains its 245-unit/second impulse and 800-unit/second-squared
gravity away from the top. Within 40 UPS of the apex, a smoothstep curve eases
gravity toward 240 on both sides of zero vertical speed; beyond that band the
descending value remains 600. Downward speed stays limited to 160 units/second.
This rounds the top while keeping the established rise and fall outside that
small transition. The cap is applied to the complete force result
for ordinary airborne player movement, using the motor's actual interval.
Ground support, Original, special movement and NPCs do not receive the cap.
The original jump gate and collision integration remain active.

The normal first-person camera retains its original head/eye position and
collision checks. After the head-camera helper, the alternate option levels
the horizon while preserving the forward axis, pitch/yaw, crouch pose and
later weapon-camera effects. Animated eye-height motion remains. Normal
posture/modes 0, 1 and crouch 2 are eligible. NPCs, scripted/special postures,
flying and developer noclip retain their original movement handling.

## Saved preferences

`[Game] MovementStyle=original|alternate` is separate from savegame state.
The legacy `doom` spelling still selects Alternate Movement, without rewriting
the file on load. The removed `hl2` spelling falls back to Original. New saves
write the canonical `alternate` spelling. Saving preserves other INI sections.

Controls exposes only these two choices and reserves eleven cells for the
complete Alternate Movement label. Walk remains remappable in its own binding
context, so existing gameplay bindings survive migration by setting name.

## Verification

Rust checks cover reduced-speed starts, braking, reversal, fractional motor
intervals, diagonals, neutral air momentum and the softer jump profile.
Native fixtures cover the translated wish path, crouch speed, platform-relative
conversion, the sustained-air branch, jump/gravity, NPC isolation, Original
restoration, camera transforms, two-choice menu cycling and full label width.
Settings checks cover legacy-name compatibility and canonical persistence.
The live check uses a fresh isolated profile in Fulton and measures actual
actor speed rather than treating input counters as proof of movement.

The earlier full-height camera shortcut was discarded after crouch clipping;
the current camera keeps the original collision-resolved eye. Extreme pitch,
scoped/recoil behavior and continuous live reversal are not covered by that
prior camera validation. This is an alternate controller, not a complete
character-controller replacement.

The earlier 240-unit/second build passed all 15 targeted checks. One bounded
Fulton run measured 240 UPS forward and strafe, braking in 32/63 ms from the
first neutral probe. Its retained jump measured 300 UPS and about 38.43 units
of rise. The standing camera was clear. The live Controls menu cycled only
Alternate Movement -> Original -> Alternate Movement, and the full name fit.
Walk, crouch and vertical look were covered by existing fixtures/prior camera
validation rather than repeated in this run. The isolated game exited normally;
the executable and real user-settings hashes remained unchanged during testing.

The subsequent softer-gravity build passed the three targeted movement,
settings and developer-tool checks. One isolated Fulton run measured 180 UPS
forward/strafe, a 245 UPS jump impulse, about 38.02 units of rise and roughly
641 ms of flight. It landed normally with a clear camera. This preserves
roughly the previous height while extending the arc from about 500 ms.
Walk90 and menu behavior were not repeated in this scalar-only live test.

The descending-phase update passed the three targeted checks. Its isolated
Fulton run reached 198 UPS forward/strafe and retained the 245 UPS takeoff.
Measured rise was about 38.09 units, with roughly 328 ms up and 375 ms down.
Downward velocity reached the 160 UPS limit and stayed bounded; descending
velocity changes of about 20 UPS per server tick match gravity 600. Landing
remained stable. These are quantized probe timings from one jump, not precise
input-latency measurements. No menu, live Walk99 or special-mode retest was
performed; those scope guards remain covered by fixtures.

Movement commands are scoped to individual motor callbacks. Queries for other
actors no longer clear a prepared player command, and nested callbacks restore
the caller's command when they return. A regression fixture interleaves an
unrelated actor query between player preparation and application. This prevents
that specific interruption; it does not by itself establish that every reported
camera jerk has the same cause.

The apex/isolation candidate passed all three targeted checks. A matched pair
of isolated 60-FPS Fulton runs measured sampled time within +/-40 UPS of the
apex increasing from 94 to 172 ms, while rise changed from 38.278 to 38.476
units. Takeoff remained 245 UPS and maximum descent 160 UPS. First ground
contact occurred at about 687 versus 735 ms; stable landing at 687 versus
766 ms. These are quantized query observations, not exact phase durations.
All twelve held-input legs in each run reached 198 UPS, with no subsequent
speed dips, root reversals or movement-command counter gaps.

Neither run reproduced the reported player/camera jerk. The command-isolation
fix is established by the regression fixture, not a witnessed live interruption.
Accepted-present intervals peaked at 17.48 versus 25.79 ms on the route; source
frame IDs remained consecutive. This is not evidence of a frame-pacing gain.
Camera review was sampled, observer/cache effects were uncontrolled, and an
external-input guard fired after the candidate recording with unknown input
onset. Real preferences remained unchanged and both game processes exited
normally. That candidate did not establish resolution of the intermittent jerk.

A subsequent dense trace found that prediction copies could descend at about
197-208 UPS while the authoritative body obeyed its 160 UPS limit. The previous
authoritative-only eligibility check excluded these copies from the tuned
controller. Native regression fixtures now cover local prediction with a client
timestep different from the server, its jump/fall limit, nested NPC isolation,
scope restoration and movement-option invalidation.

`DARK_MOVEMENT_CAMERA_PROBE=1` records each completed local camera query (up to
12,000 per thread) with QPC timestamps, prediction/authoritative positions and
velocities, eye position and view axes. These are camera queries, not displayed
frames; reads are sequential snapshots and logging can affect timing. It is
disabled during ordinary play. The older general movement counters now include
eligible prediction work as well as authoritative work.

The prediction candidate passed all three targeted checks. In the matched
60-FPS live stress run, all three jumps kept both the predicted and authoritative
body at or below 160 UPS downward, with the scope active on the actual local
prediction callbacks. The earlier trace had exceeded that limit on prediction
copies. These results establish the controller mismatch and its correction;
they do not identify every possible source of visible camera motion.

The deliberate contact phase contained two brief backward eye movements. One
coincided with the predicted body reaching the known front wall; the other
occurred with prediction nearly stopped while authoritative motion continued.
Its exact collider/camera cause remains unresolved. Neither event coincided
with an accepted-present/source-frame gap, and neither establishes a jerk in
unobstructed motion. The stress phase had no backward eye steps above 0.25 units
among its declared pairs with steady authoritative horizontal velocity.

A further run copied current preferences into an isolated profile and used a
360 FPS cap, VSync and 2560x1440 internal rendering, with movement/camera/developer
probes disabled. Output was windowed 1280x720. The scripted route and recording
finished before the 180-second diagnostic deadline; the process subsequently
hit that deadline (exit 5) before a late quit request. This was a diagnostic
timeout, not normal shutdown or evidence of a gameplay crash. Available route
timing rows cover the declared window, but the CSV has no clean closure footer.
Timeline GPU timing and recording still add overhead. All owned processes were
closed and real preferences stayed unchanged. No universal smoothness or frame
rate improvement is established by these bounded runs.
