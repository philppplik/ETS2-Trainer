// Trainer: owns the calibrators, enforces the safety gates, processes app commands, applies the
// features once per frame (frame_end, game thread) and fills the Status block for the app.
//
// The Trainer object must live in static storage of the module (see telemetry_state.h).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../shared/bridge_protocol.h"
#include "calibration.h"
#include "platform.h"
#include "telemetry_state.h"

namespace e2t {

constexpr std::uint32_t kPluginBuild = 1;

enum class WearChannel : std::uint8_t {
    Engine,
    Transmission,
    Cabin,
    Chassis,
    Wheels,
    TrailerChassis,
    CargoDamage,
};
constexpr std::size_t kWearChannelCount = 7;

// Recalibrate command bitmask (Control::commandArgs[0]).
constexpr std::uint32_t kRecalibrateFuel = 1u;
constexpr std::uint32_t kRecalibrateWear = 2u;
constexpr std::uint32_t kRecalibrateVelocity = 4u;
constexpr std::uint32_t kRecalibratePosition = 8u;
constexpr std::uint32_t kRecalibrateAll = 15u;

class Trainer {
public:
    explicit Trainer(Platform& platform) noexcept;
    Trainer(const Trainer&) = delete;
    Trainer& operator=(const Trainer&) = delete;

    void onFrameEnd(const TelemetryState& tel, const Control& control, Status& status);
    void checkTruckersMpNow() noexcept;
    bool truckersMpDetected() const noexcept { return truckersMp_; }

    const ScalarCalibrator& fuelCalibrator() const noexcept { return fuel_; }
    const ScalarCalibrator& wearCalibrator(WearChannel channel) const noexcept {
        return wear_[static_cast<std::size_t>(channel)];
    }
    const VelocityCalibrator& velocityCalibrator() const noexcept { return velocity_; }
    const PositionCalibrator& positionCalibrator() const noexcept { return position_; }

private:
    enum class Gate : std::uint8_t { Open, TruckersMp, AppStale, NoTruck };
    static constexpr std::size_t kMessageBytes = sizeof(Status::message);

    // trainer.cpp - frame orchestration and safety
    void refreshSafetyInputs(const Control& control, std::uint64_t now) noexcept;
    void detectTruckChange(const TelemetryState& tel);
    Gate evaluateGate(const TelemetryState& tel, std::uint64_t now) const noexcept;
    void logGateChange(Gate gate) noexcept;
    void handleMenuHotkey(const Control& control) noexcept;
    void runFrame(const TelemetryState& tel, const Control& control, bool newCommand,
                  std::uint64_t now);
    CalibrationContext makeContext(const TelemetryState& tel, std::uint64_t now) const noexcept;
    void updateRequests(const Control& control);
    void updateCalibrators(const TelemetryState& tel, const CalibrationContext& ctx);
    void runBatchedScan(std::uint64_t now);
    void resetCalibrations(std::uint32_t mask);
    void setCommandMessage(const char* message, std::uint64_t now) noexcept;

    // trainer_features.cpp - commands and features
    void executeCommand(const TelemetryState& tel, const Control& control, std::uint64_t now);
    void startTeleport(const TelemetryState& tel, const Control& control, std::uint64_t now);
    void applyPending(const TelemetryState& tel, const CalibrationContext& ctx, std::uint64_t now);
    void applyPendingRepair(std::uint64_t now);
    void applyPendingStop(const CalibrationContext& ctx, std::uint64_t now);
    void applyPendingTeleport(const TelemetryState& tel, const CalibrationContext& ctx,
                              std::uint64_t now);
    void applyFuel(const TelemetryState& tel, const Control& control);
    void applyNoDamage(const TelemetryState& tel, const Control& control);
    void applyVelocity(const TelemetryState& tel, const Control& control,
                       const CalibrationContext& ctx);

    // trainer_status.cpp - Status block and German status line
    void fillStatus(const Control& control, Gate gate, std::uint64_t now, Status& status) const;
    FeatureStatus wearStatus(const Control& control, Gate gate) const noexcept;
    void composeMessage(const Control& control, Gate gate, std::uint64_t now, char* out,
                        std::size_t size) const;
    bool composeHint(const Control& control, char* out, std::size_t size) const;
    void composeActiveList(char* out, std::size_t size) const;

    Platform& platform_;
    ScalarCalibrator fuel_;
    std::array<ScalarCalibrator, kWearChannelCount> wear_;
    VelocityCalibrator velocity_;
    PositionCalibrator position_;

    std::uint32_t frame_ = 0;
    std::uint32_t heartbeat_ = 0;
    Gate lastGate_ = Gate::Open;
    bool truckersMp_ = false;
    bool truckersMpChecked_ = false;
    std::uint64_t lastTruckersMpCheckMs_ = 0;
    std::uint32_t lastAppHeartbeat_ = 0;
    std::uint64_t lastAppHeartbeatChangeMs_ = 0;
    bool appHeartbeatSeen_ = false;
    bool commandResyncPending_ = false;
    std::uint32_t lastCommandSeq_ = 0;
    bool lastHadTruck_ = false;
    char lastTruckId_[kTelemetryIdText] = {};

    bool pendingRefuel_ = false;
    bool pendingRepair_ = false;
    bool pendingStop_ = false;
    bool pendingTeleport_ = false;
    double teleportTarget_[3] = {0.0, 0.0, 0.0};
    bool positionRequested_ = false;

    std::array<float, kWearChannelCount> ceilings_{};
    std::array<bool, kWearChannelCount> ceilingSet_{};
    bool menuKeyWasDown_ = false;
    std::uint32_t menuToggleCount_ = 0;
    std::uint32_t activeFlags_ = 0;
    std::uint64_t lastScanMs_ = 0;
    bool hasScanned_ = false;
    char commandMessage_[kMessageBytes] = {};
    std::uint64_t commandMessageUntilMs_ = 0;
};

// Wear channel table shared by the trainer translation units.
struct WearChannelInfo {
    const char* label;                        // German UI label
    float (*read)(const TelemetryState& tel);  // telemetry value
    bool needsTrailer;
};
const WearChannelInfo& wearChannelInfo(std::size_t index) noexcept;

}  // namespace e2t
