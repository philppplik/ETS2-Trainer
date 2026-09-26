// Trainer frame orchestration, safety gates and batched scanning.
#include "trainer.h"

#include <array>
#include <cstring>

#include "log.h"
#include "text_util.h"

namespace e2t {
namespace {

constexpr std::uint64_t kTruckersMpCheckIntervalMs = 5'000;
constexpr std::uint64_t kAppHeartbeatTimeoutMs = 3'000;
constexpr std::uint64_t kMinScanIntervalMs = 2'000;  // full scans freeze the game briefly
constexpr std::uint64_t kCommandMessageMs = 4'000;
constexpr float kMaxVelocityDt = 0.1f;               // s
constexpr float kFuelMinDistinctive = 1.0f;          // litres
constexpr float kWearMinDistinctive = 1.0e-5f;
constexpr float kFuelTestStep = 0.5f;                // litres
constexpr float kWearTestFactor = 0.5f;
constexpr float kWearLimit = 1.0f;
constexpr std::uint32_t kMaxVirtualKey = 0xFE;
constexpr double kBytesPerMiB = 1024.0 * 1024.0;

float fuelTestValue(float current, float capacity) noexcept {
    if (capacity > 0.0f && current + kFuelTestStep > capacity) {
        return current - kFuelTestStep;
    }
    return current + kFuelTestStep;
}

float wearTestValue(float current, float /*limit*/) noexcept { return current * kWearTestFactor; }

constexpr ScalarConfig kFuelConfig{"fuel", kFuelMinDistinctive, &fuelTestValue};
constexpr ScalarConfig kWearConfigs[kWearChannelCount] = {
    {"wear.engine", kWearMinDistinctive, &wearTestValue},
    {"wear.transmission", kWearMinDistinctive, &wearTestValue},
    {"wear.cabin", kWearMinDistinctive, &wearTestValue},
    {"wear.chassis", kWearMinDistinctive, &wearTestValue},
    {"wear.wheels", kWearMinDistinctive, &wearTestValue},
    {"wear.trailer_chassis", kWearMinDistinctive, &wearTestValue},
    {"cargo.damage", kWearMinDistinctive, &wearTestValue},
};

const WearChannelInfo kWearChannels[kWearChannelCount] = {
    {"Motor", [](const TelemetryState& t) { return t.wearEngine; }, false},
    {"Getriebe", [](const TelemetryState& t) { return t.wearTransmission; }, false},
    {"Kabine", [](const TelemetryState& t) { return t.wearCabin; }, false},
    {"Fahrgestell", [](const TelemetryState& t) { return t.wearChassis; }, false},
    {"Räder", [](const TelemetryState& t) { return t.wearWheels; }, false},
    {"Anhänger", [](const TelemetryState& t) { return t.trailerWearChassis; }, true},
    {"Fracht", [](const TelemetryState& t) { return cargoDamage(t); }, true},
};

const char* gateName(int gate) noexcept {
    constexpr const char* kNames[] = {"open", "TruckersMP detected", "app heartbeat stale",
                                      "no truck"};
    return gate >= 0 && gate < 4 ? kNames[gate] : "?";
}

}  // namespace

const WearChannelInfo& wearChannelInfo(std::size_t index) noexcept {
    return kWearChannels[index < kWearChannelCount ? index : 0];
}

Trainer::Trainer(Platform& platform) noexcept
    : platform_(platform),
      fuel_(kFuelConfig),
      wear_{ScalarCalibrator(kWearConfigs[0]), ScalarCalibrator(kWearConfigs[1]),
            ScalarCalibrator(kWearConfigs[2]), ScalarCalibrator(kWearConfigs[3]),
            ScalarCalibrator(kWearConfigs[4]), ScalarCalibrator(kWearConfigs[5]),
            ScalarCalibrator(kWearConfigs[6])} {}

void Trainer::onFrameEnd(const TelemetryState& tel, const Control& control, Status& status) {
    ++frame_;
    ++heartbeat_;
    const std::uint64_t now = platform_.nowMs();
    refreshSafetyInputs(control, now);
    detectTruckChange(tel);
    handleMenuHotkey(control);
    const Gate gate = evaluateGate(tel, now);
    logGateChange(gate);

    if (commandResyncPending_) {
        lastCommandSeq_ = control.commandSeq;  // never replay a command from a previous session
        commandResyncPending_ = false;
    }
    const bool newCommand = control.commandSeq != lastCommandSeq_;
    lastCommandSeq_ = control.commandSeq;

    activeFlags_ = 0;
    if (gate == Gate::Open) {
        runFrame(tel, control, newCommand, now);
    } else if (newCommand) {
        setCommandMessage("Befehl abgelehnt: Trainer ist blockiert", now);
    }
    fillStatus(control, gate, now, status);
}

void Trainer::checkTruckersMpNow() noexcept {
    lastTruckersMpCheckMs_ = platform_.nowMs();
    truckersMpChecked_ = true;
    if (truckersMp_ || !platform_.truckersMpLoaded()) {
        return;
    }
    truckersMp_ = true;  // latched until the plugin is reloaded
    log::warn("TruckersMP client detected - all trainer features are disabled");
    try {
        resetCalibrations(kRecalibrateAll);
    } catch (...) {
        log::error("reset after TruckersMP detection failed");
    }
    positionRequested_ = false;
    pendingRefuel_ = pendingRepair_ = pendingStop_ = pendingTeleport_ = false;
}

void Trainer::refreshSafetyInputs(const Control& control, std::uint64_t now) noexcept {
    if (!truckersMpChecked_ || now - lastTruckersMpCheckMs_ >= kTruckersMpCheckIntervalMs) {
        checkTruckersMpNow();
    }
    const bool wasStale = !appHeartbeatSeen_ ||
                          now - lastAppHeartbeatChangeMs_ > kAppHeartbeatTimeoutMs;
    if (control.appHeartbeat != lastAppHeartbeat_) {
        lastAppHeartbeat_ = control.appHeartbeat;
        lastAppHeartbeatChangeMs_ = now;
        appHeartbeatSeen_ = true;
        commandResyncPending_ = commandResyncPending_ || wasStale;
    }
}

// A new truck (or the same one after a reload) means new game objects: stored addresses are
// meaningless, so start over instead of waiting for validation to notice.
void Trainer::detectTruckChange(const TelemetryState& tel) {
    const bool arrived = tel.hasTruck && !lastHadTruck_;
    const bool swapped = tel.hasTruck && lastHadTruck_ &&
                         std::strcmp(tel.truckId, lastTruckId_) != 0;
    lastHadTruck_ = tel.hasTruck;
    if (!arrived && !swapped) {
        return;
    }
    copyUtf8(lastTruckId_, tel.truckId);
    log::info("truck %s (%s) - calibrations reset", arrived ? "loaded" : "changed", tel.truckId);
    resetCalibrations(kRecalibrateAll);
}

Trainer::Gate Trainer::evaluateGate(const TelemetryState& tel, std::uint64_t now) const noexcept {
    if (truckersMp_) {
        return Gate::TruckersMp;
    }
    if (!appHeartbeatSeen_ || now - lastAppHeartbeatChangeMs_ > kAppHeartbeatTimeoutMs) {
        return Gate::AppStale;
    }
    if (!tel.hasTruck) {
        return Gate::NoTruck;
    }
    return Gate::Open;
}

void Trainer::logGateChange(Gate gate) noexcept {
    if (gate == lastGate_) {
        return;
    }
    log::info("safety gate: %s -> %s", gateName(static_cast<int>(lastGate_)),
              gateName(static_cast<int>(gate)));
    lastGate_ = gate;
}

void Trainer::handleMenuHotkey(const Control& control) noexcept {
    const std::uint32_t vk = control.menuHotkeyVk;
    const bool down = vk != 0 && vk <= kMaxVirtualKey && platform_.gameHasFocus() &&
                      platform_.isKeyDown(vk);
    if (down && !menuKeyWasDown_) {
        ++menuToggleCount_;
    }
    menuKeyWasDown_ = down;
}

void Trainer::runFrame(const TelemetryState& tel, const Control& control, bool newCommand,
                       std::uint64_t now) {
    const CalibrationContext ctx = makeContext(tel, now);
    if (newCommand) {
        executeCommand(tel, control, now);
    }
    updateRequests(control);
    updateCalibrators(tel, ctx);
    runBatchedScan(now);
    applyPending(tel, ctx, now);
    applyFuel(tel, control);
    applyNoDamage(tel, control);
    applyVelocity(tel, control, ctx);
}

CalibrationContext Trainer::makeContext(const TelemetryState& tel,
                                        std::uint64_t now) const noexcept {
    CalibrationContext ctx;
    ctx.frame = frame_;
    ctx.nowMs = now;
    const bool running = !tel.paused && tel.dt > 0.0f;
    ctx.testWritesAllowed = running;
    ctx.velocityWritesAllowed = running && tel.dt <= kMaxVelocityDt;
    return ctx;
}

void Trainer::updateRequests(const Control& control) {
    fuel_.setRequested(control.infiniteFuel != 0 || pendingRefuel_);
    for (ScalarCalibrator& wear : wear_) {
        wear.setRequested(control.noDamage != 0);
    }
    velocity_.setRequested(control.powerBoost != 0 || control.nitroEnabled != 0 ||
                           control.speedCapEnabled != 0);
    position_.setRequested(positionRequested_);
}

void Trainer::updateCalibrators(const TelemetryState& tel, const CalibrationContext& ctx) {
    fuel_.update(tel.fuel, tel.fuelCapacity, true, ctx);
    for (std::size_t i = 0; i < kWearChannelCount; ++i) {
        const WearChannelInfo& info = kWearChannels[i];
        wear_[i].update(info.read(tel), kWearLimit, !info.needsTrailer || tel.trailerConnected,
                        ctx);
    }
    velocity_.update({bodySpeed(tel), angularSpeedRad(tel), true}, ctx);
    PositionInput position;
    for (int i = 0; i < 3; ++i) {
        position.pos[i] = tel.pos[i];
    }
    position.available = true;
    position_.update(position, ctx);
}

void Trainer::runBatchedScan(std::uint64_t now) {
    if (hasScanned_ && now - lastScanMs_ < kMinScanIntervalMs) {
        return;
    }
    std::array<ScanClient*, kWearChannelCount + 3> clients{};
    std::size_t clientCount = 0;
    clients[clientCount++] = &fuel_;
    for (ScalarCalibrator& wear : wear_) {
        clients[clientCount++] = &wear;
    }
    clients[clientCount++] = &velocity_;
    clients[clientCount++] = &position_;

    struct Slice {
        ScanClient* client;
        std::size_t first;
        std::size_t count;
    };
    std::vector<mem::ScanTarget> targets;
    std::vector<Slice> slices;
    for (ScanClient* client : clients) {
        if (client->wantsScan()) {
            const std::size_t first = targets.size();
            client->appendScanTargets(targets);
            slices.push_back({client, first, targets.size() - first});
        }
    }
    if (slices.empty()) {
        return;
    }
    lastScanMs_ = now;  // rate-limit even if the scan throws
    hasScanned_ = true;
    mem::ScanStats stats;
    const std::vector<mem::ScanResult> results = mem::scanProcess(targets, &stats);
    log::info("scan: %zu targets / %zu calibrators, %zu regions, %.1f MiB, %u threads, "
              "%zu faults, %.1f ms",
              targets.size(), slices.size(), stats.regions,
              static_cast<double>(stats.bytes) / kBytesPerMiB, stats.threads, stats.faults,
              stats.milliseconds);
    if (results.size() != targets.size()) {
        return;
    }
    for (const Slice& slice : slices) {
        slice.client->onScanResults(results.data() + slice.first, slice.count);
    }
}

void Trainer::resetCalibrations(std::uint32_t mask) {
    if ((mask & kRecalibrateFuel) != 0) {
        fuel_.reset();
    }
    if ((mask & kRecalibrateWear) != 0) {
        for (ScalarCalibrator& wear : wear_) {
            wear.reset();
        }
        ceilingSet_.fill(false);
    }
    if ((mask & kRecalibrateVelocity) != 0) {
        velocity_.reset();
    }
    if ((mask & kRecalibratePosition) != 0) {
        position_.reset();
    }
    log::info("calibrations reset (mask %u)", mask);
}

void Trainer::setCommandMessage(const char* message, std::uint64_t now) noexcept {
    copyUtf8(commandMessage_, message);
    commandMessageUntilMs_ = now + kCommandMessageMs;
    log::info("command: %s", message);
}

}  // namespace e2t
