// Minimal, independently written declarations of the SCS telemetry SDK ABI (API 1.00 / 1.01).
//
// Only what this plugin uses. Layouts, constants and names were checked against the official
// SDK headers (scs_sdk/include: scssdk.h, scssdk_value.h, scssdk_telemetry*.h and common/*,
// ETS2 game telemetry 1.18). The static_asserts pin the x64 sizes documented there.
#pragma once

#include <cstddef>
#include <cstdint>

#define E2T_SCSAPI __stdcall  // ignored on x64, kept for ABI documentation

namespace scs {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using string_t = const char*;
using result_t = std::int32_t;
using context_t = void*;
using timestamp_t = std::uint64_t;  // microseconds
using log_type_t = std::int32_t;
using value_type_t = std::uint32_t;
using event_t = std::uint32_t;

constexpr u32 kU32Nil = 0xFFFFFFFFu;  // index for non-array channels

constexpr result_t kResultOk = 0;
constexpr result_t kResultUnsupported = -1;
constexpr result_t kResultInvalidParameter = -2;
constexpr result_t kResultAlreadyRegistered = -3;
constexpr result_t kResultNotFound = -4;
constexpr result_t kResultUnsupportedType = -5;
constexpr result_t kResultNotNow = -6;
constexpr result_t kResultGenericError = -7;

constexpr log_type_t kLogMessage = 0;
constexpr log_type_t kLogWarning = 1;
constexpr log_type_t kLogError = 2;

constexpr u32 makeVersion(u32 major, u32 minor) { return (major << 16) | minor; }
constexpr u32 versionMajor(u32 version) { return (version >> 16) & 0xFFFFu; }
constexpr u32 versionMinor(u32 version) { return version & 0xFFFFu; }
constexpr u32 kTelemetryVersion_1_00 = makeVersion(1, 0);
constexpr u32 kTelemetryVersion_1_01 = makeVersion(1, 1);

constexpr char kGameIdEts2[] = "eut2";
constexpr char kGameIdAts[] = "ats";

// ---- values -------------------------------------------------------------------------------

constexpr value_type_t kValueInvalid = 0;
constexpr value_type_t kValueBool = 1;
constexpr value_type_t kValueS32 = 2;
constexpr value_type_t kValueU32 = 3;
constexpr value_type_t kValueU64 = 4;
constexpr value_type_t kValueFloat = 5;
constexpr value_type_t kValueDouble = 6;
constexpr value_type_t kValueFVector = 7;
constexpr value_type_t kValueDVector = 8;
constexpr value_type_t kValueEuler = 9;
constexpr value_type_t kValueFPlacement = 10;
constexpr value_type_t kValueDPlacement = 11;
constexpr value_type_t kValueString = 12;
constexpr value_type_t kValueS64 = 13;

struct FVector {
    float x, y, z;
};
struct DVector {
    double x, y, z;
};
struct Euler {  // heading/pitch/roll as fractions of a full turn
    float heading, pitch, roll;
};
struct FPlacement {
    FVector position;
    Euler orientation;
};
struct DPlacement {
    DVector position;
    Euler orientation;
    u32 padding;
};

struct Value {
    value_type_t type;
    u32 padding;
    union {
        u8 asBool;  // nonzero = true
        s32 asS32;
        u32 asU32;
        u64 asU64;
        s64 asS64;
        float asFloat;
        double asDouble;
        FVector asFVector;
        DVector asDVector;
        Euler asEuler;
        FPlacement asFPlacement;
        DPlacement asDPlacement;
        string_t asString;
    };
};

struct NamedValue {
    string_t name;  // array terminator: name == nullptr
    u32 index;
    u32 padding;
    Value value;
};

// ---- events -------------------------------------------------------------------------------

constexpr event_t kEventInvalid = 0;
constexpr event_t kEventFrameStart = 1;
constexpr event_t kEventFrameEnd = 2;
constexpr event_t kEventPaused = 3;
constexpr event_t kEventStarted = 4;
constexpr event_t kEventConfiguration = 5;
constexpr event_t kEventGameplay = 6;

constexpr u32 kFrameStartFlagTimerRestart = 0x00000001u;

struct FrameStart {
    u32 flags;
    u32 padding;
    timestamp_t renderTime;
    timestamp_t simulationTime;        // keeps running while the physics are paused
    timestamp_t pausedSimulationTime;  // stops while paused
};

struct Configuration {
    string_t id;
    const NamedValue* attributes;  // never null, terminated by name == nullptr
};

struct GameplayEvent {
    string_t id;
    const NamedValue* attributes;
};

using log_fn = void(E2T_SCSAPI*)(log_type_t type, string_t message);
using event_callback_fn = void(E2T_SCSAPI*)(event_t event, const void* eventInfo, context_t context);
using register_for_event_fn = result_t(E2T_SCSAPI*)(event_t event, event_callback_fn callback,
                                                    context_t context);
using unregister_from_event_fn = result_t(E2T_SCSAPI*)(event_t event);

// ---- channels -----------------------------------------------------------------------------

constexpr u32 kChannelFlagNone = 0x00000000u;
constexpr u32 kChannelFlagEachFrame = 0x00000001u;
constexpr u32 kChannelFlagNoValue = 0x00000002u;

using channel_callback_fn = void(E2T_SCSAPI*)(string_t name, u32 index, const Value* value,
                                              context_t context);
using register_for_channel_fn = result_t(E2T_SCSAPI*)(string_t name, u32 index, value_type_t type,
                                                      u32 flags, channel_callback_fn callback,
                                                      context_t context);
using unregister_from_channel_fn = result_t(E2T_SCSAPI*)(string_t name, u32 index,
                                                         value_type_t type);

// ---- init parameters ----------------------------------------------------------------------

struct SdkInitParamsV100 {
    string_t gameName;
    string_t gameId;  // "eut2" for ETS2
    u32 gameVersion;  // game specific telemetry version, not the patch level
    u32 padding;      // x64 only
    log_fn log;
};

struct TelemetryInitParams {};  // opaque base, the SDK passes a pointer to a versioned struct

struct TelemetryInitParamsV100 : TelemetryInitParams {
    SdkInitParamsV100 common;
    register_for_event_fn registerForEvent;
    unregister_from_event_fn unregisterFromEvent;
    register_for_channel_fn registerForChannel;
    unregister_from_channel_fn unregisterFromChannel;
};
using TelemetryInitParamsV101 = TelemetryInitParamsV100;  // identical per SDK

static_assert(sizeof(void*) == 8, "the plugin targets 64-bit ETS2 only");
static_assert(sizeof(FVector) == 12 && sizeof(DVector) == 24, "SDK vector layout");
static_assert(sizeof(FPlacement) == 24 && sizeof(DPlacement) == 40, "SDK placement layout");
static_assert(sizeof(Value) == 48 && offsetof(Value, asFloat) == 8, "SDK value layout");
static_assert(sizeof(NamedValue) == 64 && offsetof(NamedValue, value) == 16, "SDK named value");
static_assert(sizeof(FrameStart) == 32, "SDK frame start layout");
static_assert(sizeof(Configuration) == 16 && sizeof(GameplayEvent) == 16, "SDK event layout");
static_assert(sizeof(SdkInitParamsV100) == 32, "SDK init params layout");
static_assert(sizeof(TelemetryInitParamsV100) == 64, "SDK telemetry init params layout");

// ---- channel names ------------------------------------------------------------------------

namespace channel {
constexpr char kGameTime[] = "game.time";         // u32, in-game minutes
constexpr char kNextRestStop[] = "rest.stop";     // s32, in-game minutes
constexpr char kTruckWorldPlacement[] = "truck.world.placement";              // dplacement
constexpr char kTruckLocalLinearVelocity[] = "truck.local.velocity.linear";   // fvector, m/s
constexpr char kTruckLocalAngularVelocity[] = "truck.local.velocity.angular"; // fvector, rot/s
constexpr char kTruckLocalLinearAcceleration[] = "truck.local.acceleration.linear";  // fvector
constexpr char kTruckSpeed[] = "truck.speed";                  // float, m/s (negative reversing)
constexpr char kTruckEngineRpm[] = "truck.engine.rpm";         // float
constexpr char kTruckEngineGear[] = "truck.engine.gear";       // s32
constexpr char kTruckInputThrottle[] = "truck.input.throttle";  // float 0..1
constexpr char kTruckInputBrake[] = "truck.input.brake";
constexpr char kTruckEffectiveThrottle[] = "truck.effective.throttle";
constexpr char kTruckEffectiveBrake[] = "truck.effective.brake";
constexpr char kTruckCruiseControl[] = "truck.cruise_control";  // float, m/s
constexpr char kTruckParkingBrake[] = "truck.brake.parking";    // bool
constexpr char kTruckFuel[] = "truck.fuel.amount";              // float, litres
constexpr char kTruckFuelAverageConsumption[] = "truck.fuel.consumption.average";  // l/km
constexpr char kTruckFuelRange[] = "truck.fuel.range";          // float, km
constexpr char kTruckEngineEnabled[] = "truck.engine.enabled";  // bool
constexpr char kTruckWearEngine[] = "truck.wear.engine";        // float 0..1
constexpr char kTruckWearTransmission[] = "truck.wear.transmission";
constexpr char kTruckWearCabin[] = "truck.wear.cabin";
constexpr char kTruckWearChassis[] = "truck.wear.chassis";
constexpr char kTruckWearWheels[] = "truck.wear.wheels";
constexpr char kTruckOdometer[] = "truck.odometer";                        // float, km
constexpr char kTruckNavigationSpeedLimit[] = "truck.navigation.speed.limit";  // float, m/s
constexpr char kJobCargoDamage[] = "job.cargo.damage";                     // float 0..1
// Since game telemetry 1.14 trailers are indexed: "trailer.<index>.<name>". The unindexed
// "trailer.<name>" form is the pre-1.14 spelling and is only used as a fallback.
constexpr char kTrailer0Connected[] = "trailer.0.connected";               // bool
constexpr char kTrailer0WearChassis[] = "trailer.0.wear.chassis";          // float
constexpr char kTrailer0CargoDamage[] = "trailer.0.cargo.damage";          // float
constexpr char kTrailerLegacyConnected[] = "trailer.connected";
constexpr char kTrailerLegacyWearChassis[] = "trailer.wear.chassis";
constexpr char kTrailerLegacyCargoDamage[] = "trailer.cargo.damage";
}  // namespace channel

namespace config {
constexpr char kTruck[] = "truck";
constexpr char kJob[] = "job";
constexpr char kAttrBrandId[] = "brand_id";        // string
constexpr char kAttrBrand[] = "brand";             // string
constexpr char kAttrId[] = "id";                   // string
constexpr char kAttrName[] = "name";               // string
constexpr char kAttrFuelCapacity[] = "fuel.capacity";  // float, litres
constexpr char kAttrRpmLimit[] = "rpm.limit";      // float
constexpr char kAttrForwardGears[] = "gears.forward";  // u32
constexpr char kAttrCargo[] = "cargo";             // string
constexpr char kAttrDestinationCity[] = "destination.city";  // string
constexpr char kAttrDestinationCityId[] = "destination.city.id";        // string
constexpr char kAttrDestinationCompanyId[] = "destination.company.id";  // string
constexpr char kAttrSourceCityId[] = "source.city.id";                  // string
constexpr char kAttrSourceCompanyId[] = "source.company.id";            // string
}  // namespace config

namespace gameplay {
constexpr char kJobDelivered[] = "job.delivered";
}  // namespace gameplay

// ---- input SDK (scssdk_input*.h, input API 1.00) -------------------------------------------

constexpr u32 kInputVersion_1_00 = makeVersion(1, 0);
constexpr u32 kInputDeviceTypeSemantical = 2;  // inputs are named after game controls
constexpr u32 kInputEventFlagFirstInFrame = 0x00000001u;
constexpr result_t kResultNotFoundInput = -4;  // == SCS_RESULT_not_found: no more events

struct InputDeviceInput {
    string_t name;
    string_t displayName;
    value_type_t valueType;
    u32 padding;
};
static_assert(sizeof(InputDeviceInput) == 24, "scs_input_device_input_t (x64)");

struct InputEvent {
    u32 inputIndex;
    union {
        u8 valueBool;
        float valueFloat;
        float sizing[6];
    };
};
static_assert(sizeof(InputEvent) == 28, "scs_input_event_t");

using input_active_callback_fn = void(E2T_SCSAPI*)(u8 active, context_t context);
using input_event_callback_fn = result_t(E2T_SCSAPI*)(InputEvent* event, u32 flags,
                                                      context_t context);

struct InputDevice {
    string_t name;
    string_t displayName;
    u32 type;
    u32 inputCount;
    const InputDeviceInput* inputs;
    context_t callbackContext;
    input_active_callback_fn inputActiveCallback;
    input_event_callback_fn inputEventCallback;
};
static_assert(sizeof(InputDevice) == 56, "scs_input_device_t (x64)");

using register_device_fn = result_t(E2T_SCSAPI*)(const InputDevice* device);

struct InputInitParams {};  // opaque base
struct InputInitParamsV100 : InputInitParams {
    SdkInitParamsV100 common;
    register_device_fn registerDevice;
};
static_assert(sizeof(InputInitParamsV100) == 40, "scs_input_init_params_v100_t (x64)");

}  // namespace scs

// Exported via ets2_trainer.def (undecorated names on x64).
extern "C" {
scs::result_t E2T_SCSAPI scs_telemetry_init(scs::u32 version,
                                            const scs::TelemetryInitParams* params);
void E2T_SCSAPI scs_telemetry_shutdown(void);
scs::result_t E2T_SCSAPI scs_input_init(scs::u32 version, const scs::InputInitParams* params);
void E2T_SCSAPI scs_input_shutdown(void);
}
