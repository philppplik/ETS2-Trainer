// Teleport, recovery (unflip / return to road), physics tricks and the input light/horn show.
//
// Everything here only writes through the calibrators (validated addresses) and runs on the game
// thread in frame_end. Physics writes are skipped while the game is paused.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "input_device.h"
#include "log.h"
#include "trainer.h"

namespace e2t {
namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr float kCrumbMinSpeed = 3.0f;       // m/s
constexpr float kCrumbMaxTilt = 0.03f;       // turns (about 11 degrees)
constexpr double kCrumbSpacing = 25.0;       // m
constexpr double kReturnMinDistance = 20.0;  // m behind the stuck truck
constexpr double kReturnLift = 0.5;          // m
constexpr double kUnflipLift = 1.2;          // m
constexpr std::uint32_t kUnflipHoldFrames = 30;
constexpr std::uint32_t kReturnHoldFrames = 25;
constexpr std::uint64_t kRecoveryTimeoutMs = 90'000;
constexpr float kTippedTilt = 0.14f;  // turns (about 50 degrees)
constexpr std::uint64_t kTippedDelayMs = 1'500;
constexpr std::uint64_t kAutoUprightCooldownMs = 5'000;
constexpr float kAutoUprightMaxSpeed = 3.0f;  // m/s
constexpr double kGravity = 9.81;
constexpr double kHoverClimb = 4.0;   // m/s
constexpr double kHoverAccel = 20.0;  // m/s^2
constexpr float kMaxSpinTurns = 3.0f;
constexpr float kBarrelRollSeconds = 1.1f;
constexpr double kBarrelRollJump = 7.0;  // m/s
constexpr std::uint32_t kUprightFramesAfterRoll = 10;
constexpr float kSpaceVoteSpeed = 5.0f;
constexpr float kSpaceVoteMaxAngular = 0.05f;  // rad/s
constexpr double kSpaceVoteMinSin = 0.3;       // heading must not be near north/south
constexpr int kSpaceVoteLimit = 60;
constexpr std::uint64_t kDiscoStepMs = 250;
constexpr std::uint32_t kHazardEverySteps = 4;
constexpr std::uint64_t kLowriderStepMs = 400;
constexpr std::uint32_t kHornPattern[] = {140, 90, 140, 90, 140, 90, 420, 260,
                                          140, 90, 140, 90, 140, 90, 420, 900};
constexpr std::size_t kTextBytes = 160;

double distanceSq(const double (&a)[3], const double (&b)[3]) noexcept {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

bool isTilted(float pitch, float roll, float limit) noexcept {
    return std::fabs(pitch) > limit || std::fabs(roll) > limit;
}

bool hornHeldAt(std::uint64_t elapsedMs) noexcept {
    std::uint64_t total = 0;
    for (const std::uint32_t step : kHornPattern) {
        total += step;
    }
    std::uint64_t t = elapsedMs % total;
    for (std::size_t i = 0; i < std::size(kHornPattern); ++i) {
        if (t < kHornPattern[i]) {
            return i % 2 == 0;
        }
        t -= kHornPattern[i];
    }
    return false;
}

}  // namespace

void Trainer::updateMotion(const TelemetryState& tel, const Control& control,
                           const CalibrationContext& ctx, std::uint64_t now) {
    recordBreadcrumb(tel);
    detectVelocitySpace(tel);
    runPendingRecovery(tel, now);
    handleTrickKeys(tel, control, now);
    if (ctx.velocityWritesAllowed) {
        runScript(tel, ctx);
        applyFun(tel, control, ctx);
    }
    applyAutoUpright(tel, control, now);
    applyInputShow(control, now);
}

void Trainer::recordBreadcrumb(const TelemetryState& tel) {
    if (std::fabs(tel.speed) < kCrumbMinSpeed || isTilted(tel.pitch, tel.roll, kCrumbMaxTilt) ||
        script_.kind != MotionScript::Kind::None || anchorActive_) {
        return;
    }
    if (crumbCount_ > 0) {
        const Breadcrumb& newest = crumbs_[(crumbHead_ + kBreadcrumbCapacity - 1) % kBreadcrumbCapacity];
        if (distanceSq(newest.pos, tel.pos) < kCrumbSpacing * kCrumbSpacing) {
            return;
        }
    }
    Breadcrumb& crumb = crumbs_[crumbHead_];
    std::copy(std::begin(tel.pos), std::end(tel.pos), std::begin(crumb.pos));
    crumb.heading = tel.speed < 0.0f ? tel.heading + 0.5f : tel.heading;  // face travel direction
    crumb.heading -= std::floor(crumb.heading);
    crumbHead_ = (crumbHead_ + 1) % kBreadcrumbCapacity;
    crumbCount_ = std::min(crumbCount_ + 1, kBreadcrumbCapacity);
}

// The velocity vectors are usually world space; if they turn out to be vehicle space the tricks
// use (0, 0, -1) as "forward". Decided by votes while driving straight away from north/south.
void Trainer::detectVelocitySpace(const TelemetryState& tel) {
    double v[3];
    if (std::fabs(tel.speed) < kSpaceVoteSpeed || angularSpeedRad(tel) > kSpaceVoteMaxAngular ||
        !velocity_.readFirst(v)) {
        return;
    }
    const double heading = static_cast<double>(tel.heading) * kTwoPi;
    if (std::fabs(std::sin(heading)) < kSpaceVoteMinSin) {
        return;
    }
    const double length = lengthOf(v);
    const double sign = tel.speed < 0.0f ? -1.0 : 1.0;
    const double world = sign * (-std::sin(heading) * v[0] - std::cos(heading) * v[2]) / length;
    const double local = sign * -v[2] / length;
    if (world > 0.9 && local < 0.7) {
        velocitySpaceVotes_ = std::min(velocitySpaceVotes_ + 1, kSpaceVoteLimit);
    } else if (local > 0.9 && world < 0.7) {
        velocitySpaceVotes_ = std::max(velocitySpaceVotes_ - 1, -kSpaceVoteLimit);
    }
}

void Trainer::forwardDirection(const TelemetryState& tel, double (&out)[3]) const noexcept {
    if (velocitySpaceVotes_ < 0) {
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = -1.0;
        return;
    }
    const double heading = static_cast<double>(tel.heading) * kTwoPi;
    out[0] = -std::sin(heading);
    out[1] = 0.0;
    out[2] = -std::cos(heading);
}

void Trainer::runPendingRecovery(const TelemetryState& tel, std::uint64_t now) {
    if (!pendingUnflip_ && !pendingReturn_) {
        return;
    }
    if (now - recoveryRequestedMs_ > kRecoveryTimeoutMs) {
        pendingUnflip_ = pendingReturn_ = false;
        setCommandMessage(u8"Aufstellen abgebrochen: Position/Rotation nicht kalibriert – "
                          u8"„Teleport & Tricks vorbereiten“ an und kurz fahren",
                          now);
        return;
    }
    if (!position_.isActive() || !orientation_.isActive()) {
        return;  // status line explains what is still calibrating
    }
    if (pendingUnflip_) {
        pendingUnflip_ = false;
        startUnflip(tel, now);
    }
    if (pendingReturn_) {
        pendingReturn_ = false;
        startReturnToRoad(tel, now);
    }
}

bool Trainer::startUnflip(const TelemetryState& tel, std::uint64_t now) {
    const double target[3] = {tel.pos[0], tel.pos[1] + kUnflipLift, tel.pos[2]};
    startHold(target, tel.heading, kUnflipHoldFrames);
    setCommandMessage(u8"LKW wird aufgestellt", now);
    return true;
}

bool Trainer::startReturnToRoad(const TelemetryState& tel, std::uint64_t now) {
    if (crumbCount_ == 0) {
        setCommandMessage(u8"Zurück auf die Straße: noch keine sichere Position aufgezeichnet", now);
        return false;
    }
    const Breadcrumb* chosen = nullptr;
    for (std::size_t i = 1; i <= crumbCount_; ++i) {
        const Breadcrumb& crumb = crumbs_[(crumbHead_ + kBreadcrumbCapacity - i) % kBreadcrumbCapacity];
        chosen = chosen == nullptr ? &crumb : chosen;  // newest as fallback
        if (distanceSq(crumb.pos, tel.pos) >= kReturnMinDistance * kReturnMinDistance) {
            chosen = &crumb;
            break;
        }
    }
    const double target[3] = {chosen->pos[0], chosen->pos[1] + kReturnLift, chosen->pos[2]};
    startHold(target, chosen->heading, kReturnHoldFrames);
    setCommandMessage(u8"Zurück auf die Straße", now);
    return true;
}

void Trainer::startHold(const double (&target)[3], float heading, std::uint32_t frames) {
    script_ = MotionScript{};
    script_.kind = MotionScript::Kind::Hold;
    std::copy(std::begin(target), std::end(target), std::begin(script_.target));
    script_.hasTarget = true;
    script_.heading = heading;
    script_.framesLeft = frames;
}

void Trainer::runScript(const TelemetryState& tel, const CalibrationContext& /*ctx*/) {
    switch (script_.kind) {
        case MotionScript::Kind::None: return;
        case MotionScript::Kind::Hold:
            if (script_.hasTarget) {
                position_.teleport(script_.target);
                velocity_.setMagnitude(0.0f);
            }
            orientation_.write(script_.heading, 0.0f, 0.0f);
            if (script_.framesLeft == 0 || --script_.framesLeft == 0) {
                script_ = MotionScript{};
            }
            return;
        case MotionScript::Kind::BarrelRoll: {
            script_.elapsed += tel.dt;
            const float t = std::min(1.0f, script_.elapsed / kBarrelRollSeconds);
            orientation_.write(script_.heading, 0.0f, t);
            if (t >= 1.0f) {
                const float heading = script_.heading;
                script_ = MotionScript{};
                script_.kind = MotionScript::Kind::Hold;  // settle upright, keep moving
                script_.heading = heading;
                script_.framesLeft = kUprightFramesAfterRoll;
            }
            return;
        }
    }
}

void Trainer::applyFun(const TelemetryState& tel, const Control& control,
                       const CalibrationContext& /*ctx*/) {
    const bool scripted = script_.kind != MotionScript::Kind::None;
    const bool anchor = (control.funFlags & kFunAnchor) != 0 && position_.isActive() && !scripted;
    if (anchor && !anchorActive_) {
        std::copy(std::begin(tel.pos), std::end(tel.pos), std::begin(anchorPos_));
        anchorHeading_ = tel.heading;
    }
    anchorActive_ = anchor;
    if (anchor) {
        position_.teleport(anchorPos_);
        velocity_.setMagnitude(0.0f);
        orientation_.write(anchorHeading_, 0.0f, 0.0f);
        activeFlags_ |= kActiveAnchor;
        return;  // frozen: no other physics trick applies
    }

    const bool spin = (control.funFlags & kFunSpin) != 0 && orientation_.isActive() && !scripted;
    if (spin) {
        spinHeading_ = spinning_ ? spinHeading_ : tel.heading;
        const float turns = std::clamp(control.spinTurnsPerSecond, -kMaxSpinTurns, kMaxSpinTurns);
        spinHeading_ += turns * tel.dt;
        spinHeading_ -= std::floor(spinHeading_);
        orientation_.write(spinHeading_, tel.pitch, tel.roll);
        activeFlags_ |= kActiveSpin;
    }
    spinning_ = spin;

    if ((control.funFlags & kFunMoonGravity) != 0 && velocity_.isActive()) {
        const double fraction = std::clamp(static_cast<double>(control.moonGravity), 0.0, 1.0);
        const double lift[3] = {0.0, kGravity * fraction * tel.dt, 0.0};
        velocity_.addDelta(lift);
        activeFlags_ |= kActiveMoonGravity;
    }

    double v[3];
    const bool hover = control.hoverVk != 0 && platform_.gameHasFocus() &&
                       platform_.isKeyDown(control.hoverVk) && velocity_.readFirst(v);
    if (hover && v[1] < kHoverClimb) {
        const double climb[3] = {0.0, std::min(kHoverClimb - v[1], kHoverAccel * tel.dt), 0.0};
        velocity_.addDelta(climb);
        activeFlags_ |= kActiveHover;
    }
}

void Trainer::handleTrickKeys(const TelemetryState& tel, const Control& control, std::uint64_t now) {
    const std::uint32_t keys[4] = {control.jumpVk, control.rocketVk, control.rollVk, control.unflipVk};
    const bool focus = platform_.gameHasFocus();
    for (std::size_t i = 0; i < 4; ++i) {
        const bool down = focus && keys[i] != 0 && keys[i] <= 0xFE && platform_.isKeyDown(keys[i]);
        const bool pressed = down && !trickKeyDown_[i];
        trickKeyDown_[i] = down;
        if (!pressed) {
            continue;
        }
        switch (i) {
            case 0: kick(tel, 0.0, 9.0, u8"Sprung", now); break;
            case 1: kick(tel, 25.0, 7.0, u8"Rakete", now); break;
            case 2: startBarrelRoll(tel, now); break;
            default:
                pendingUnflip_ = true;
                recoveryRequestedMs_ = now;
                break;
        }
    }
}

bool Trainer::kick(const TelemetryState& tel, double forward, double up, const char* label,
                   std::uint64_t now) {
    char text[kTextBytes];
    if (!velocity_.isActive() || tel.paused) {
        std::snprintf(text, sizeof(text), u8"%s: erst „Teleport & Tricks vorbereiten“ an und "
                                          u8"kurz fahren (Geschwindigkeit kalibrieren)", label);
        setCommandMessage(text, now);
        return false;
    }
    double direction[3];
    forwardDirection(tel, direction);
    const double delta[3] = {direction[0] * forward, up + direction[1] * forward,
                             direction[2] * forward};
    const bool done = velocity_.addDelta(delta);
    std::snprintf(text, sizeof(text), done ? u8"%s!" : u8"%s fehlgeschlagen", label);
    setCommandMessage(text, now);
    return done;
}

bool Trainer::startBarrelRoll(const TelemetryState& tel, std::uint64_t now) {
    if (!orientation_.isActive() || !velocity_.isActive() || tel.paused) {
        setCommandMessage(u8"Fassrolle: Rotation/Geschwindigkeit noch nicht kalibriert – "
                          u8"„Teleport & Tricks vorbereiten“ an und etwas fahren",
                          now);
        return false;
    }
    const double jump[3] = {0.0, kBarrelRollJump, 0.0};
    velocity_.addDelta(jump);
    script_ = MotionScript{};
    script_.kind = MotionScript::Kind::BarrelRoll;
    script_.heading = tel.heading;
    setCommandMessage(u8"Fassrolle!", now);
    return true;
}

void Trainer::applyAutoUpright(const TelemetryState& tel, const Control& control, std::uint64_t now) {
    const bool enabled = (control.funFlags & kFunAutoUpright) != 0 && orientation_.isActive() &&
                         position_.isActive() && script_.kind == MotionScript::Kind::None;
    if (!enabled) {
        tippedSinceMs_ = 0;
        return;
    }
    activeFlags_ |= kActiveAutoUpright;
    const bool tipped = isTilted(tel.pitch, tel.roll, kTippedTilt) &&
                        std::fabs(tel.speed) < kAutoUprightMaxSpeed;
    if (!tipped) {
        tippedSinceMs_ = 0;
        return;
    }
    if (tippedSinceMs_ == 0) {
        tippedSinceMs_ = now;
    } else if (now - tippedSinceMs_ >= kTippedDelayMs && now - lastAutoUprightMs_ >= kAutoUprightCooldownMs) {
        lastAutoUprightMs_ = now;
        tippedSinceMs_ = 0;
        startUnflip(tel, now);
        log::info("auto upright triggered");
    }
}

void Trainer::applyInputShow(const Control& control, std::uint64_t now) {
    const bool registered = input::registered();
    const bool disco = registered && (control.funFlags & kFunDisco) != 0;
    const bool horn = registered && (control.funFlags & kFunHorn) != 0;
    const bool lowrider = registered && (control.funFlags & kFunLowrider) != 0;
    if (!discoOn_ && !hornOn_ && !lowriderOn_ && (disco || horn || lowrider)) {
        showStartMs_ = now;
        showStep_ = 0;
    }
    const std::uint64_t elapsed = now - showStartMs_;

    if (disco && !discoOn_) {
        input::pulse(input::Button::Beacon);
        ++beaconPresses_;
    }
    if (disco) {
        const auto step = static_cast<std::uint32_t>(elapsed / kDiscoStepMs);
        if (step != showStep_) {
            showStep_ = step;
            input::pulse(input::Button::HighBeam);
            ++highBeamPresses_;
            if (step % kHazardEverySteps == 0) {
                input::pulse(input::Button::Hazard);
                ++hazardPresses_;
            }
        }
        activeFlags_ |= kActiveDisco;
    } else if (discoOn_) {
        // Restore the light switches to how they were (toggle buttons: even number of presses).
        if (beaconPresses_ % 2 != 0) input::pulse(input::Button::Beacon);
        if (hazardPresses_ % 2 != 0) input::pulse(input::Button::Hazard);
        if (highBeamPresses_ % 2 != 0) input::pulse(input::Button::HighBeam);
        beaconPresses_ = hazardPresses_ = highBeamPresses_ = 0;
    }
    discoOn_ = disco;

    input::setHeld(input::Button::Horn, horn && hornHeldAt(elapsed));
    if (horn) {
        activeFlags_ |= kActiveHorn;
    }
    hornOn_ = horn;

    const bool phaseA = (elapsed / kLowriderStepMs) % 2 == 0;
    input::setHeld(input::Button::FrontSuspUp, lowrider && phaseA);
    input::setHeld(input::Button::RearSuspDown, lowrider && phaseA);
    input::setHeld(input::Button::FrontSuspDown, lowrider && !phaseA);
    input::setHeld(input::Button::RearSuspUp, lowrider && !phaseA);
    if (lowrider) {
        activeFlags_ |= kActiveLowrider;
    } else if (lowriderOn_) {
        input::pulse(input::Button::SuspReset);
    }
    lowriderOn_ = lowrider;
}

void Trainer::stopInputShow() {
    Control off{};
    applyInputShow(off, 0);
}

}  // namespace e2t
