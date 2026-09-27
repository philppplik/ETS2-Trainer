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
constexpr uint32_t kBridgeVersion = 2u;
constexpr wchar_t kBridgeMappingName[] = L"Local\\ETS2TrainerBridge_v2";

enum TelemetryFlags : uint32_t {
    kFlagPaused = 1u << 0,
    kFlagEngineOn = 1u << 1,
    kFlagParkingBrake = 1u << 2,
    kFlagTrailerAttached = 1u << 3,
    kFlagTruckersMpDetected = 1u << 4,  // TruckersMP client loaded -> all write features are blocked
    kFlagHasTruck = 1u << 5,            // truck configuration received
    kFlagHasJob = 1u << 6,              // a job (cargo) is active
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
    kActiveMoonGravity = 1u << 5,
    kActiveAnchor = 1u << 6,
    kActiveSpin = 1u << 7,
    kActiveHover = 1u << 8,
    kActiveDisco = 1u << 9,
    kActiveHorn = 1u << 10,
    kActiveLowrider = 1u << 11,
    kActiveAutoUpright = 1u << 12,
};

// Control::funFlags
enum FunFlags : uint32_t {
    kFunMoonGravity = 1u << 0,  // Control::moonGravity cancels that fraction of gravity
    kFunAnchor = 1u << 1,       // freeze the truck where it is (even mid-air)
    kFunSpin = 1u << 2,         // spin around the vertical axis (Control::spinTurnsPerSecond)
    kFunDisco = 1u << 3,        // light show via the plugin's input device
    kFunHorn = 1u << 4,         // horn concert via the input device
    kFunLowrider = 1u << 5,     // air-suspension hopping via the input device
    kFunAutoUpright = 1u << 6,  // put the truck back on its wheels when it tips over
};

// Status::capabilities
enum Capabilities : uint32_t {
    kCapCloudSaves = 1u << 0,   // Steam Remote Storage reachable -> WriteSaveFiles works
    kCapInputDevice = 1u << 1,  // input device registered -> disco/horn/lowrider work
};

enum class CommandType : uint32_t {
    None = 0,
    Refuel = 1,         // one-shot: fuel to capacity (needs fuel calibration)
    Repair = 2,         // one-shot: all calibrated wear values to 0
    StopTruck = 3,      // one-shot: zero the calibrated velocity vectors
    Recalibrate = 4,    // args[0] = bitmask: 1 fuel, 2 wear, 4 velocity, 8 position, 16 orientation
    Teleport = 5,       // args[0..2] = x, y, z; args[3] = heading (0..1), args[4] = 1 if heading valid
    ResetAll = 6,       // forget all calibrations
    Unflip = 7,         // put the truck back on its wheels (lift + upright + stop)
    ReturnToRoad = 8,   // teleport to the last safe breadcrumb behind the truck
    Jump = 9,           // args[0] = upward speed m/s (0 = default)
    Rocket = 10,        // forward + upward kick
    BarrelRoll = 11,    // 360 degree roll stunt
    WriteSaveFiles = 12,  // write the files listed in %LOCALAPPDATA%\ETS2Trainer\cloud_request.txt
                          // through the game's Steam Remote Storage (works without a truck)
};

// Status::saveWriteResult (>= 0: number of files written)
enum SaveWriteError : int32_t {
    kSaveWriteNoCloud = -1,       // Steam Remote Storage not available
    kSaveWriteBadRequest = -2,    // request file missing or invalid
    kSaveWriteReadFailed = -3,    // a staged file could not be read
    kSaveWriteQuota = -4,         // not enough Steam Cloud quota
    kSaveWriteRejected = -5,      // Steam refused a file write
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
    char destinationCity[48];     // display name
    char destinationCityId[48];   // token, e.g. "berlin"
    char destinationCompanyId[48];
    char sourceCityId[48];
    char sourceCompanyId[48];
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
    FeatureStatus orientation;
    uint32_t lastCommandAck;   // == Control::commandSeq once the command was processed
    uint32_t activeFlags;      // ActiveFlags actually applied this frame
    uint32_t menuToggleCount;  // incremented when Control::menuHotkeyVk is pressed while the game has focus
    uint32_t capabilities;     // Capabilities
    uint32_t breadcrumbCount;  // safe positions recorded for ReturnToRoad
    uint32_t jobStartedCount;  // incremented when a job configuration arrives (truck at the source company)
    uint32_t jobDeliveredCount;  // incremented on the job.delivered gameplay event
    uint32_t saveWriteAck;       // commandSeq of the last processed WriteSaveFiles
    int32_t saveWriteResult;     // files written or SaveWriteError
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
    uint32_t prepareMotion; // 0/1: calibrate velocity, position and orientation for teleport + tricks
    uint32_t funFlags;      // FunFlags
    float moonGravity;      // 0..1 fraction of gravity cancelled while kFunMoonGravity is set
    float spinTurnsPerSecond;
    uint32_t jumpVk;        // hotkeys handled in-game (0 = none)
    uint32_t rocketVk;
    uint32_t rollVk;
    uint32_t hoverVk;       // held: climb
    uint32_t unflipVk;
    uint32_t commandSeq;    // app increments to issue commandType/commandArgs
    uint32_t commandType;   // CommandType
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

static_assert(sizeof(Telemetry) == 616, "Telemetry layout changed - update BridgeProtocol.cs");
static_assert(sizeof(FeatureStatus) == 16, "FeatureStatus layout changed - update BridgeProtocol.cs");
static_assert(sizeof(Status) == 384, "Status layout changed - update BridgeProtocol.cs");
static_assert(sizeof(Control) == 136, "Control layout changed - update BridgeProtocol.cs");
static_assert(sizeof(Shared) == 1152, "Shared layout changed - update BridgeProtocol.cs");
static_assert(offsetof(Shared, telemetry) == 16, "layout");
static_assert(offsetof(Shared, status) == 632, "layout");
static_assert(offsetof(Shared, control) == 1016, "layout");
static_assert(offsetof(Status, message) == 128, "layout");
static_assert(offsetof(Control, commandArgs) == 88, "layout");

}  // namespace e2t
