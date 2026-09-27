// Trainer frame orchestration, safety gates and batched scanning.
#include "trainer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>

#include "input_device.h"
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

Trainer::~Trainer() { shutdown(); }

void Trainer::shutdown() noexcept {
    discardScan();
    input::releaseAll();
}

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
    bool newCommand = control.commandSeq != lastCommandSeq_;
    lastCommandSeq_ = control.commandSeq;

    // Saves can be written from the main menu (no truck yet); everything else needs the gate.
    const bool saveCommand = newCommand && static_cast<CommandType>(control.commandType) ==
                                               CommandType::WriteSaveFiles;
    if (saveCommand && gate != Gate::TruckersMp && gate != Gate::AppStale) {
        handleSaveWrite(control, now);
        newCommand = false;
    }

    activeFlags_ = 0;
    if (gate == Gate::Open) {
        runFrame(tel, control, newCommand, now);
    } else {
        if (newCommand) {
            setCommandMessage("Befehl abgelehnt: Trainer ist blockiert", now);
        }
        stopInputShow();
    }
    fillStatus(control, gate, now, status);
    fillExtendedStatus(tel, control, gate, status);
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
    updateMotion(tel, control, ctx, now);
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
    constexpr std::uint32_t kPhysicsFun = kFunMoonGravity | kFunAnchor | kFunSpin | kFunAutoUpright;
    const bool motion = control.prepareMotion != 0 || (control.funFlags & kPhysicsFun) != 0 ||
                        pendingUnflip_ || pendingReturn_;
    velocity_.setRequested(control.powerBoost != 0 || control.nitroEnabled != 0 ||
                           control.speedCapEnabled != 0 || motion);
    position_.setRequested(positionRequested_ || motion);
    orientation_.setRequested(motion);
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
    position.speed = bodySpeed(tel);
    position.available = true;
    position_.update(position, ctx);
    OrientationInput orientation;
    orientation.heading = tel.heading;
    orientation.pitch = tel.pitch;
    orientation.roll = tel.roll;
    orientation.anchors = &position_.confirmed();
    orientation.available = position_.isActive() && script_.kind == MotionScript::Kind::None;
    orientation_.update(orientation, ctx);
}

// One batched scan at a time. It runs on a background thread (the game keeps running) while
// every participating calibrator widens its live bounds each frame; results are delivered on
// the game thread, where the calibrators immediately filter them against the current values.
void Trainer::runBatchedScan(std::uint64_t now) {
    if (asyncScan_.running()) {
        pollScan();
        return;
    }
    if (hasScanned_ && now - lastScanMs_ < kMinScanIntervalMs) {
        return;
    }
    startScan(now);
}

void Trainer::startScan(std::uint64_t now) {
    std::array<ScanClient*, kScanClientCount> clients{};
    std::size_t clientCount = 0;
    clients[clientCount++] = &fuel_;
    for (ScalarCalibrator& wear : wear_) {
        clients[clientCount++] = &wear;
    }
    clients[clientCount++] = &velocity_;
    clients[clientCount++] = &position_;

    std::vector<mem::ScanTarget> targets;
    scanSlices_.clear();
    for (std::size_t i = 0; i < clientCount; ++i) {
        ScanClient* client = clients[i];
        if (!client->scanning() && client->wantsScan()) {
            const std::size_t first = targets.size();
            client->appendScanTargets(targets);
            scanSlices_.push_back({client, first, targets.size() - first});
        }
    }
    if (scanSlices_.empty()) {
        return;
    }
    lastScanMs_ = now;  // rate-limit even if the scan throws
    hasScanned_ = true;
    auto bounds = std::make_unique<mem::LiveBounds[]>(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].usesBounds()) {
            mem::initLiveBounds(targets[i], bounds[i]);
            targets[i].live = &bounds[i];
        }
    }
    if (synchronousScans_) {
        mem::ScanStats stats;
        const std::vector<mem::ScanResult> results = mem::scanProcess(targets, &stats);
        deliverScan(results, stats);
        return;
    }
    const std::size_t targetCount = targets.size();
    if (!asyncScan_.start(std::move(targets), std::move(bounds), mem::backgroundWorkerCount())) {
        log::warn("scan: background thread could not be started");
        scanSlices_.clear();
        return;
    }
    for (const ScanSlice& slice : scanSlices_) {
        slice.client->markScanStarted();
    }
    log::info("scan: started in background (%zu targets / %zu calibrators)", targetCount,
              scanSlices_.size());
}

void Trainer::pollScan() {
    if (!asyncScan_.done()) {
        mem::LiveBounds* bounds = asyncScan_.bounds();
        for (const ScanSlice& slice : scanSlices_) {
            slice.client->widenScanTargets(bounds + slice.first, slice.count);
        }
        return;
    }
    mem::ScanStats stats;
    const std::vector<mem::ScanResult> results = asyncScan_.take(stats);
    deliverScan(results, stats);
}

void Trainer::deliverScan(const std::vector<mem::ScanResult>& results,
                          const mem::ScanStats& stats) {
    std::size_t expected = 0;
    for (const ScanSlice& slice : scanSlices_) {
        expected = std::max(expected, slice.first + slice.count);
    }
    log::info("scan: %zu calibrators, %zu regions, %.1f MiB, %u threads, %zu faults, %.1f ms%s",
              scanSlices_.size(), stats.regions, static_cast<double>(stats.bytes) / kBytesPerMiB,
              stats.threads, stats.faults, stats.milliseconds, stats.cancelled ? " (cancelled)" : "");
    const std::vector<ScanSlice> slices = std::move(scanSlices_);
    scanSlices_.clear();
    for (const ScanSlice& slice : slices) {
        if (results.size() >= expected && !stats.cancelled) {
            slice.client->deliverScanResults(results.data() + slice.first, slice.count);
        } else {
            slice.client->abandonScan();
        }
    }
}

void Trainer::discardScan() noexcept {
    asyncScan_.cancel();
    for (const ScanSlice& slice : scanSlices_) {
        slice.client->abandonScan();
    }
    scanSlices_.clear();
}

void Trainer::resetCalibrations(std::uint32_t mask) {
    discardScan();  // its results would belong to the old objects
    if ((mask & (kRecalibratePosition | kRecalibrateOrientation)) != 0) {
        orientation_.reset();  // anchored to the position addresses
        script_ = MotionScript{};
        anchorActive_ = false;
    }
    if ((mask & kRecalibratePosition) != 0) {
        crumbCount_ = 0;
        crumbHead_ = 0;
    }
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
