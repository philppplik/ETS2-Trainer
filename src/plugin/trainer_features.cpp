// Trainer commands and per-frame features (fuel, no damage, boost/nitro/speed cap, teleport).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "cloud_storage.h"
#include "log.h"
#include "trainer.h"

namespace e2t {
namespace {

constexpr float kRefillMargin = 0.1f;          // litres below capacity before refilling
constexpr float kMinPowerFactor = 1.0f;
constexpr float kMaxPowerFactor = 20.0f;
constexpr float kBoostMinThrottle = 0.05f;
constexpr float kMaxBoostBaseAccel = 4.0f;     // m/s^2: caps feedback if the game's accel
                                               // channel ever includes our own injection
constexpr float kMaxNitroAccel = 50.0f;        // m/s^2
constexpr float kMaxDeltaVPerFrame = 2.0f;     // m/s
constexpr float kMinSpeedChange = 1.0e-4f;     // m/s, below: no write
constexpr float kKmhPerMs = 3.6f;
constexpr float kMaxSpeedCapKmh = 1000.0f;
constexpr double kMaxRecalibrateMask = 31.0;
constexpr double kMaxTeleportCoordinate = 1.0e7;  // m
constexpr double kTeleportLift = 0.4;             // m above the target, the truck settles
constexpr std::uint32_t kTeleportHoldFrames = 20;
constexpr std::uint32_t kMaxTeleportFailures = 10;  // frames
constexpr std::size_t kTextBytes = 160;

float sanitized(float value, float low, float high, float fallback) noexcept {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::min(high, std::max(low, value));
}

// Extra speed from the power boost: positive longitudinal acceleration along the direction of
// travel times (factor - 1). Vehicle forward is -Z in SCS vehicle space.
float boostDeltaV(const TelemetryState& tel, float powerFactor, float dt) noexcept {
    const float factor = sanitized(powerFactor, kMinPowerFactor, kMaxPowerFactor,
                                   kMinPowerFactor);
    if (tel.effThrottle <= kBoostMinThrottle) {
        return 0.0f;
    }
    const float direction = tel.speed < 0.0f ? -1.0f : 1.0f;
    const float acceleration = direction * -tel.accLocal[2];
    if (!(acceleration > 0.0f)) {
        return 0.0f;
    }
    return std::min(acceleration, kMaxBoostBaseAccel) * (factor - 1.0f) * dt;
}

}  // namespace

void Trainer::executeCommand(const TelemetryState& tel, const Control& control,
                             std::uint64_t now) {
    switch (static_cast<CommandType>(control.commandType)) {
        case CommandType::None: break;
        case CommandType::Refuel:
            pendingRefuel_ = true;
            if (!fuel_.isActive()) {
                setCommandMessage(u8"Tanken: startet, sobald der Sprit kalibriert ist – fahr "
                                  u8"kurz los",
                                  now);
            }
            break;
        case CommandType::Repair: pendingRepair_ = true; break;
        case CommandType::StopTruck: pendingStop_ = true; break;
        case CommandType::Recalibrate: {
            const double arg = control.commandArgs[0];
            if (!std::isfinite(arg) || arg < 1.0 || arg > kMaxRecalibrateMask) {
                setCommandMessage(u8"Neu kalibrieren: ungültige Auswahl", now);
                break;
            }
            const auto mask = static_cast<std::uint32_t>(arg);
            resetCalibrations(mask);
            if ((mask & kRecalibratePosition) != 0) {
                positionRequested_ = true;
            }
            setCommandMessage(u8"Neu-Kalibrierung gestartet", now);
            break;
        }
        case CommandType::Teleport: startTeleport(control, now); break;
        case CommandType::ResetAll:
            resetCalibrations(kRecalibrateAll);
            positionRequested_ = false;
            pendingRefuel_ = pendingRepair_ = pendingStop_ = pendingTeleport_ = false;
            pendingUnflip_ = pendingReturn_ = false;
            setCommandMessage(u8"Alle Kalibrierungen zurückgesetzt", now);
            break;
        case CommandType::Unflip:
            pendingUnflip_ = true;
            recoveryRequestedMs_ = now;
            break;
        case CommandType::ReturnToRoad:
            pendingReturn_ = true;
            recoveryRequestedMs_ = now;
            break;
        case CommandType::Jump: {
            const double arg = control.commandArgs[0];
            const double up = std::isfinite(arg) && arg > 0.0 && arg <= 40.0 ? arg : 9.0;
            kick(tel, 0.0, up, u8"Sprung", now);
            break;
        }
        case CommandType::Rocket: kick(tel, 25.0, 7.0, u8"Rakete", now); break;
        case CommandType::BarrelRoll: startBarrelRoll(tel, now); break;
        case CommandType::WriteSaveFiles: handleSaveWrite(control, now); break;
        default: setCommandMessage(u8"Unbekannter Befehl", now); break;
    }
}

void Trainer::handleSaveWrite(const Control& control, std::uint64_t now) {
    const cloud::WriteResult result = cloud::writeRequestedFiles();
    saveWriteAck_ = control.commandSeq;
    saveWriteResult_ = result.code;
    setCommandMessage(result.message, now);
}

void Trainer::startTeleport(const Control& control, std::uint64_t now) {
    for (int i = 0; i < 3; ++i) {
        const double coordinate = control.commandArgs[i];
        if (!std::isfinite(coordinate) || std::fabs(coordinate) > kMaxTeleportCoordinate) {
            setCommandMessage(u8"Teleport abgelehnt: ungültige Zielkoordinaten", now);
            return;
        }
        teleportTarget_[i] = coordinate;
    }
    const double heading = control.commandArgs[3];
    teleportHasHeading_ = control.commandArgs[4] > 0.5 && std::isfinite(heading);
    teleportHeading_ = teleportHasHeading_ ? static_cast<float>(heading - std::floor(heading)) : 0.0f;
    pendingTeleport_ = true;
    positionRequested_ = true;
    log::info("teleport requested to %.1f %.1f %.1f", teleportTarget_[0], teleportTarget_[1],
              teleportTarget_[2]);
    if (!position_.isActive()) {
        setCommandMessage(u8"Teleport: Positions-Kalibrierung gestartet (experimentell)", now);
    }
}

void Trainer::applyPending(const TelemetryState& tel, const CalibrationContext& ctx,
                           std::uint64_t now) {
    if (pendingRefuel_) {
        if (fuel_.isActive()) {
            pendingRefuel_ = false;
            const bool refilled = tel.fuelCapacity > 0.0f && fuel_.write(tel.fuelCapacity);
            setCommandMessage(refilled ? u8"Tank aufgefüllt"
                                       : u8"Tanken fehlgeschlagen – Adresse ungültig",
                              now);
        } else if (fuel_.state() == FeatureState::Failed) {
            pendingRefuel_ = false;
            setCommandMessage(u8"Tanken fehlgeschlagen – Sprit nicht kalibrierbar", now);
        }
    }
    if (pendingRepair_) {
        applyPendingRepair(now);
    }
    if (pendingStop_) {
        applyPendingStop(ctx, now);
    }
    if (pendingTeleport_) {
        applyPendingTeleport(tel, ctx, now);
    }
    if (positionRequested_ && !pendingTeleport_ && position_.isActive()) {
        // Keep the confirmed addresses (re-validated before the next teleport) but stop
        // driving the position calibrator, so e.g. a truck change does not trigger a scan.
        positionRequested_ = false;
    }
}

void Trainer::applyPendingRepair(std::uint64_t now) {
    pendingRepair_ = false;
    unsigned repaired = 0;
    for (std::size_t i = 0; i < kWearChannelCount; ++i) {
        if (wear_[i].isActive() && wear_[i].write(0.0f)) {
            ceilings_[i] = 0.0f;
            ceilingSet_[i] = true;
            ++repaired;
        }
    }
    char text[kTextBytes];
    if (repaired > 0) {
        std::snprintf(text, sizeof(text), u8"Reparatur: %u Schadenswerte auf 0 gesetzt",
                      repaired);
    } else {
        std::snprintf(text, sizeof(text), "%s",
                      u8"Reparatur: keine kalibrierten Schadenswerte – „Kein Schaden“ "
                      u8"aktivieren und etwas fahren");
    }
    setCommandMessage(text, now);
}

void Trainer::applyPendingStop(const CalibrationContext& ctx, std::uint64_t now) {
    pendingStop_ = false;
    if (!velocity_.isActive()) {
        setCommandMessage(u8"Stopp: Geschwindigkeit ist noch nicht kalibriert (Boost aktivieren)",
                          now);
    } else if (!ctx.velocityWritesAllowed) {
        setCommandMessage(u8"Stopp: während der Pause nicht möglich", now);
    } else {
        setCommandMessage(velocity_.setMagnitude(0.0f) ? u8"LKW gestoppt"
                                                       : u8"Stopp fehlgeschlagen",
                          now);
    }
}

void Trainer::applyPendingTeleport(const TelemetryState& tel, const CalibrationContext& ctx,
                                   std::uint64_t now) {
    if (position_.state() == FeatureState::Failed) {
        pendingTeleport_ = false;
        setCommandMessage(u8"Teleport fehlgeschlagen: Position nicht kalibrierbar", now);
        return;
    }
    if (!position_.isActive() || !ctx.testWritesAllowed) {
        return;  // keep waiting for the calibration / the end of the pause
    }
    if (teleportHasHeading_ && orientation_.isActive()) {
        const double lifted[3] = {teleportTarget_[0], teleportTarget_[1] + kTeleportLift,
                                  teleportTarget_[2]};
        startHold(lifted, teleportHeading_, kTeleportHoldFrames);
        pendingTeleport_ = false;
        teleportFailures_ = 0;
        setCommandMessage(tel.trailerConnected ? u8"Teleport ausgeführt – der Anhänger bleibt zurück"
                                               : u8"Teleport ausgeführt",
                          now);
        return;
    }
    if (!position_.teleport(teleportTarget_)) {
        // Stored addresses went stale: validation drops them within a few frames and a new
        // calibration starts; only give up if writing keeps failing while still "active".
        if (++teleportFailures_ < kMaxTeleportFailures) {
            return;
        }
        pendingTeleport_ = false;
        teleportFailures_ = 0;
        setCommandMessage(u8"Teleport fehlgeschlagen: Positionsadresse ungültig", now);
        return;
    }
    pendingTeleport_ = false;
    teleportFailures_ = 0;
    if (velocity_.isActive() && ctx.velocityWritesAllowed) {
        velocity_.setMagnitude(0.0f);
    }
    setCommandMessage(u8"Teleport ausgeführt", now);
}

void Trainer::applyFuel(const TelemetryState& tel, const Control& control) {
    if (control.infiniteFuel == 0 || !fuel_.isActive()) {
        return;
    }
    activeFlags_ |= kActiveFuel;
    if (tel.fuelCapacity > 0.0f && fuel_.value() < tel.fuelCapacity - kRefillMargin) {
        fuel_.write(tel.fuelCapacity);
    }
}

void Trainer::applyNoDamage(const TelemetryState& tel, const Control& control) {
    if (control.noDamage == 0) {
        ceilingSet_.fill(false);
        return;
    }
    bool engaged = false;
    for (std::size_t i = 0; i < kWearChannelCount; ++i) {
        ScalarCalibrator& wear = wear_[i];
        if (!wear.isActive()) {
            ceilingSet_[i] = false;
            continue;
        }
        if (wearChannelInfo(i).needsTrailer && !tel.trailerConnected) {
            continue;
        }
        engaged = true;
        const float value = wear.value();
        if (!ceilingSet_[i]) {
            ceilings_[i] = value;  // damage may not grow beyond the value at activation
            ceilingSet_[i] = true;
            log::info("no damage: %s ceiling %.6f", wearChannelInfo(i).label,
                      static_cast<double>(value));
        }
        if (value > ceilings_[i]) {
            wear.write(ceilings_[i]);
        }
    }
    if (engaged) {
        activeFlags_ |= kActiveNoDamage;
    }
}

void Trainer::applyVelocity(const TelemetryState& tel, const Control& control,
                            const CalibrationContext& ctx) {
    const bool boost = control.powerBoost != 0;
    const bool nitro = control.nitroEnabled != 0;
    const bool speedCap = control.speedCapEnabled != 0;
    if (!(boost || nitro || speedCap) || !velocity_.isActive() || !ctx.velocityWritesAllowed) {
        return;
    }
    const float speed = bodySpeed(tel);
    float deltaV = 0.0f;
    if (boost) {
        activeFlags_ |= kActiveBoost;
        deltaV += boostDeltaV(tel, control.powerFactor, tel.dt);
    }
    if (nitro && platform_.gameHasFocus() && platform_.isKeyDown(control.nitroVk)) {
        activeFlags_ |= kActiveNitro;
        deltaV += sanitized(control.nitroAccel, 0.0f, kMaxNitroAccel, 0.0f) * tel.dt;
    }
    float target = speed + std::min(deltaV, kMaxDeltaVPerFrame);
    if (speedCap) {
        const float capMs = sanitized(control.speedCapKmh, 0.0f, kMaxSpeedCapKmh, 0.0f) /
                            kKmhPerMs;
        if (capMs > 0.0f) {
            activeFlags_ |= kActiveSpeedCap;
            target = std::min(target, capMs);
        }
    }
    if (std::fabs(target - speed) > kMinSpeedChange) {
        velocity_.setMagnitude(target);
    }
}

}  // namespace e2t
