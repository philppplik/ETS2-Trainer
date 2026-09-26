// ETS2 Trainer — shared-memory bridge between the in-game plugin (C++) and the trainer app (C#).
//
// The C# mirror lives in src/Ets2Trainer.Core/Bridge/BridgeProtocol.cs. Any change here must be
// reflected there; the static_asserts below and the C# layout tests pin the byte layout.
//
// Ownership:
//   Header + Telemetry + Status  -> written by the plugin, read by the app
//   Control                      -> written by the app, read by the plugin
#pragma once

#include <cstddef>
#include <cstdint>

namespace e2t {

constexpr uint32_t kBridgeMagic = 0x42543245u;  // "E2TB" little endian
constexpr uint32_t kBridgeVersion = 1u;
constexpr wchar_t kBridgeMappingName[] = L"Local\\ETS2TrainerBridge_v1";

enum TelemetryFlags : uint32_t {
    kFlagPaused = 1u << 0,
    kFlagEngineOn = 1u << 1,
    kFlagParkingBrake = 1u << 2,
    kFlagTrailerAttached = 1u << 3,
    kFlagTruckersMpDetected = 1u << 4,  // TruckersMP client loaded -> all write features are blocked
    kFlagHasTruck = 1u << 5,            // truck configuration received
};

enum class FeatureState : uint32_t {
    Off = 0,             // feature not requested
    WaitingForData = 1,  // needs a changing value (e.g. drive a bit so fuel drops)
    Calibrating = 2,     // memory scan / candidate filtering in progress
    Verifying = 3,       // test-writing candidates and watching telemetry react
    Active = 4,          // address confirmed, feature applied each frame
    Failed = 5,          // calibration gave up (see Status::message)
    Blocked = 6,         // blocked for safety (TruckersMP, app heartbeat stale, ...)
};

enum ActiveFlags : uint32_t {
    kActiveFuel = 1u << 0,
    kActiveNoDamage = 1u << 1,
    kActiveBoost = 1u << 2,
    kActiveNitro = 1u << 3,
    kActiveSpeedCap = 1u << 4,
};

enum class CommandType : uint32_t {
    None = 0,
    Refuel = 1,       // one-shot: fuel to capacity (needs fuel calibration)
    Repair = 2,       // one-shot: all calibrated wear values to 0
    StopTruck = 3,    // one-shot: zero the calibrated velocity vectors
    Recalibrate = 4,  // args[0] = bitmask: 1 fuel, 2 wear, 4 velocity, 8 position
    Teleport = 5,     // experimental: args[0..2] = x, y, z world position (no trailer attached only)
    ResetAll = 6,     // forget all calibrations
};

#pragma pack(push, 1)

struct Telemetry {  // plugin -> app, updated every frame, seqlock: seqBegin == seqEnd when consistent
    uint32_t seqBegin;
    uint32_t frameCounter;
    uint32_t gameVersion;  // SCS game-specific telemetry version (major << 16 | minor)
    uint32_t flags;        // TelemetryFlags
    uint64_t simTimeUs;
    uint64_t renderTimeUs;
    double posX, posY, posZ;  // truck.world.placement position (m)
    float heading, pitch, roll;  // SCS units: heading 0..1 = 0..360 deg counter-clockwise, pitch/roll fractions of a turn
    float speed;                 // m/s, negative when reversing
    float velLocal[3];           // truck.local.velocity.linear (m/s, vehicle space)
    float accLocal[3];           // truck.local.acceleration.linear (m/s^2, vehicle space)
    float rpm, rpmMax;
    int32_t gear, gearsForward;
    float inputThrottle, inputBrake, effThrottle, effBrake;
    float fuel, fuelCapacity, fuelRange, fuelAvgConsumption;
    float wearEngine, wearTransmission, wearCabin, wearChassis, wearWheels;
    float trailerWearChassis, cargoDamage;
    float speedLimit, cruiseControl;  // m/s
    float odometer;                   // km
    uint32_t gameTimeMin;             // absolute in-game minutes
    int32_t restStopMin;
    char truckBrand[32];  // UTF-8, zero terminated
    char truckName[48];
    char truckId[48];     // "<brand_id>.<id>", e.g. "scania.s_2016"
    char cargo[48];
    char destinationCity[48];
    uint32_t reserved0;
    uint32_t seqEnd;
};

struct FeatureStatus {
    uint32_t state;       // FeatureState
    uint32_t candidates;  // remaining memory candidates while calibrating
    uint32_t confirmed;   // confirmed addresses (can be > 1, e.g. truck + trailer bodies)
    float progress;       // 0..1, rough UI hint
};

struct Status {  // plugin -> app
    uint32_t pluginBuild;
    uint32_t heartbeat;  // incremented every frame by the plugin
    FeatureStatus fuel;
    FeatureStatus wear;
    FeatureStatus velocity;
    FeatureStatus position;
    uint32_t lastCommandAck;   // == Control::commandSeq once the command was processed
    uint32_t activeFlags;      // ActiveFlags actually applied this frame
    uint32_t menuToggleCount;  // incremented when Control::menuHotkeyVk is pressed while the game has focus
    uint32_t reserved0;
    char message[256];  // latest human-readable status line (UTF-8, German)
};

struct Control {  // app -> plugin
    uint32_t appHeartbeat;  // app increments ~10x per second; plugin blocks all writes when stale for > 3 s
    uint32_t infiniteFuel;  // 0/1
    uint32_t noDamage;      // 0/1
    uint32_t powerBoost;    // 0/1
    float powerFactor;      // 1..20, multiplies positive longitudinal acceleration
    uint32_t nitroEnabled;  // 0/1
    uint32_t nitroVk;       // Win32 virtual-key code held for nitro
    float nitroAccel;       // m/s^2 added along the direction of travel while held
    uint32_t speedCapEnabled;
    float speedCapKmh;
    uint32_t menuHotkeyVk;  // 0 = none
    uint32_t commandSeq;    // app increments to issue commandType/commandArgs
    uint32_t commandType;   // CommandType
    uint32_t reserved0;
    double commandArgs[6];
};

struct Shared {
    uint32_t magic;    // kBridgeMagic, written last by the plugin on init
    uint32_t version;  // kBridgeVersion
    uint32_t size;     // sizeof(Shared)
    uint32_t reserved0;
    Telemetry telemetry;
    Status status;
    Control control;
};

#pragma pack(pop)

static_assert(sizeof(Telemetry) == 424, "Telemetry layout changed - update BridgeProtocol.cs");
static_assert(sizeof(FeatureStatus) == 16, "FeatureStatus layout changed - update BridgeProtocol.cs");
static_assert(sizeof(Status) == 344, "Status layout changed - update BridgeProtocol.cs");
static_assert(sizeof(Control) == 104, "Control layout changed - update BridgeProtocol.cs");
static_assert(sizeof(Shared) == 888, "Shared layout changed - update BridgeProtocol.cs");
static_assert(offsetof(Shared, telemetry) == 16, "layout");
static_assert(offsetof(Shared, status) == 440, "layout");
static_assert(offsetof(Shared, control) == 784, "layout");
static_assert(offsetof(Control, commandArgs) == 56, "layout");

}  // namespace e2t
