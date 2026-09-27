// Trainer: owns the calibrators, enforces the safety gates, processes app commands, applies the
// features once per frame (frame_end, game thread) and fills the Status block for the app.
//
// The Trainer object must live in static storage of the module (see telemetry_state.h).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../shared/bridge_protocol.h"
#include "async_scan.h"
#include "calibration.h"
#include "calibration_orientation.h"
#include "platform.h"
#include "telemetry_state.h"

namespace e2t {

constexpr std::uint32_t kPluginBuild = 2;

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
constexpr std::uint32_t kRecalibrateOrientation = 16u;
constexpr std::uint32_t kRecalibrateAll = 31u;

struct Breadcrumb {
    double pos[3] = {0.0, 0.0, 0.0};
    float heading = 0.0f;
};
constexpr std::size_t kBreadcrumbCapacity = 64;

// Multi-frame scripted motion: holds the truck at a pose (unflip, return to road, teleport with
// heading) or animates a barrel roll.
struct MotionScript {
    enum class Kind : std::uint8_t { None, Hold, BarrelRoll };
    Kind kind = Kind::None;
    double target[3] = {0.0, 0.0, 0.0};
    bool hasTarget = false;
    float heading = 0.0f;
    std::uint32_t framesLeft = 0;
    float elapsed = 0.0f;
};

class Trainer {
public:
    explicit Trainer(Platform& platform) noexcept;
    ~Trainer();
    Trainer(const Trainer&) = delete;
    Trainer& operator=(const Trainer&) = delete;

    void onFrameEnd(const TelemetryState& tel, const Control& control, Status& status);
    void checkTruckersMpNow() noexcept;
    bool truckersMpDetected() const noexcept { return truckersMp_; }
    // Stops a running background scan; call before the DLL unloads.
    void shutdown() noexcept;
    // Self-test: run scans synchronously inside frame_end (deterministic frame counts).
    void setSynchronousScans(bool synchronous) noexcept { synchronousScans_ = synchronous; }

    const ScalarCalibrator& fuelCalibrator() const noexcept { return fuel_; }
    const ScalarCalibrator& wearCalibrator(WearChannel channel) const noexcept {
        return wear_[static_cast<std::size_t>(channel)];
    }
    const VelocityCalibrator& velocityCalibrator() const noexcept { return velocity_; }
    const PositionCalibrator& positionCalibrator() const noexcept { return position_; }
    const OrientationCalibrator& orientationCalibrator() const noexcept { return orientation_; }
    std::size_t breadcrumbCount() const noexcept { return crumbCount_; }

private:
    enum class Gate : std::uint8_t { Open, TruckersMp, AppStale, NoTruck };
    struct ScanSlice {
        ScanClient* client;
        std::size_t first;
        std::size_t count;
    };
    static constexpr std::size_t kMessageBytes = sizeof(Status::message);
    static constexpr std::size_t kScanClientCount = kWearChannelCount + 3;

    // trainer.cpp - frame orchestration, safety and background scans
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
    void startScan(std::uint64_t now);
    void pollScan();
    void deliverScan(const std::vector<mem::ScanResult>& results, const mem::ScanStats& stats);
    void discardScan() noexcept;
    void resetCalibrations(std::uint32_t mask);
    void setCommandMessage(const char* message, std::uint64_t now) noexcept;
    void fillExtendedStatus(const TelemetryState& tel, const Control& control, Gate gate,
                            Status& status);

    // trainer_features.cpp - commands, fuel, damage, boost
    void executeCommand(const TelemetryState& tel, const Control& control, std::uint64_t now);
    void handleSaveWrite(const Control& control, std::uint64_t now);
    void startTeleport(const Control& control, std::uint64_t now);
    void applyPending(const TelemetryState& tel, const CalibrationContext& ctx, std::uint64_t now);
    void applyPendingRepair(std::uint64_t now);
    void applyPendingStop(const CalibrationContext& ctx, std::uint64_t now);
    void applyPendingTeleport(const TelemetryState& tel, const CalibrationContext& ctx,
                              std::uint64_t now);
    void applyFuel(const TelemetryState& tel, const Control& control);
    void applyNoDamage(const TelemetryState& tel, const Control& control);
    void applyVelocity(const TelemetryState& tel, const Control& control,
                       const CalibrationContext& ctx);

    // trainer_motion.cpp - teleport, recovery, tricks and the input light/horn show
    void updateMotion(const TelemetryState& tel, const Control& control,
                      const CalibrationContext& ctx, std::uint64_t now);
    void recordBreadcrumb(const TelemetryState& tel);
    void detectVelocitySpace(const TelemetryState& tel);
    void handleTrickKeys(const TelemetryState& tel, const Control& control, std::uint64_t now);
    void runPendingRecovery(const TelemetryState& tel, std::uint64_t now);
    bool startUnflip(const TelemetryState& tel, std::uint64_t now);
    bool startReturnToRoad(const TelemetryState& tel, std::uint64_t now);
    void startHold(const double (&target)[3], float heading, std::uint32_t frames);
    void runScript(const TelemetryState& tel, const CalibrationContext& ctx);
    void applyFun(const TelemetryState& tel, const Control& control,
                  const CalibrationContext& ctx);
    void applyAutoUpright(const TelemetryState& tel, const Control& control, std::uint64_t now);
    void applyInputShow(const Control& control, std::uint64_t now);
    void stopInputShow();
    bool kick(const TelemetryState& tel, double forward, double up, const char* label,
              std::uint64_t now);
    bool startBarrelRoll(const TelemetryState& tel, std::uint64_t now);
    void forwardDirection(const TelemetryState& tel, double (&out)[3]) const noexcept;

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
    OrientationCalibrator orientation_;

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

    // background scans
    mem::AsyncScan asyncScan_;
    std::vector<ScanSlice> scanSlices_;
    bool synchronousScans_ = false;
    std::uint64_t lastScanMs_ = 0;
    bool hasScanned_ = false;

    bool pendingRefuel_ = false;
    bool pendingRepair_ = false;
    bool pendingStop_ = false;
    bool pendingTeleport_ = false;
    bool pendingUnflip_ = false;
    bool pendingReturn_ = false;
    double teleportTarget_[3] = {0.0, 0.0, 0.0};
    float teleportHeading_ = 0.0f;
    bool teleportHasHeading_ = false;
    std::uint32_t teleportFailures_ = 0;
    bool positionRequested_ = false;
    std::uint64_t recoveryRequestedMs_ = 0;

    std::array<float, kWearChannelCount> ceilings_{};
    std::array<bool, kWearChannelCount> ceilingSet_{};
    bool menuKeyWasDown_ = false;
    std::uint32_t menuToggleCount_ = 0;
    std::uint32_t activeFlags_ = 0;
    char commandMessage_[kMessageBytes] = {};
    std::uint64_t commandMessageUntilMs_ = 0;

    // motion + tricks
    std::array<Breadcrumb, kBreadcrumbCapacity> crumbs_{};
    std::size_t crumbCount_ = 0;
    std::size_t crumbHead_ = 0;
    MotionScript script_;
    bool anchorActive_ = false;
    double anchorPos_[3] = {0.0, 0.0, 0.0};
    float anchorHeading_ = 0.0f;
    float spinHeading_ = 0.0f;
    bool spinning_ = false;
    int velocitySpaceVotes_ = 0;  // > 0: world space, < 0: vehicle space
    std::array<bool, 4> trickKeyDown_{};  // jump, rocket, roll, unflip
    std::uint64_t tippedSinceMs_ = 0;
    std::uint64_t lastAutoUprightMs_ = 0;

    // input show
    bool discoOn_ = false;
    bool hornOn_ = false;
    bool lowriderOn_ = false;
    std::uint64_t showStartMs_ = 0;
    std::uint32_t showStep_ = 0;
    std::uint32_t beaconPresses_ = 0;
    std::uint32_t hazardPresses_ = 0;
    std::uint32_t highBeamPresses_ = 0;

    // cloud saves
    std::uint32_t saveWriteAck_ = 0;
    std::int32_t saveWriteResult_ = 0;
    bool cloudAvailable_ = false;
    std::uint64_t lastCloudCheckMs_ = 0;
    bool cloudChecked_ = false;
};

// Wear channel table shared by the trainer translation units.
struct WearChannelInfo {
    const char* label;                        // German UI label
    float (*read)(const TelemetryState& tel);  // telemetry value
    bool needsTrailer;
};
const WearChannelInfo& wearChannelInfo(std::size_t index) noexcept;

}  // namespace e2t
