// Latest SDK telemetry as seen by the plugin (filled by the channel/config callbacks).
// Must live in static storage of the module (never on the heap): the memory scanner skips the
// module image, so the plugin's own copies of the values can never become scan candidates.
#pragma once

#include <cmath>
#include <cstdint>

namespace e2t {

constexpr std::size_t kTelemetryShortText = 32;
constexpr std::size_t kTelemetryText = 48;
constexpr std::size_t kTelemetryIdText = 64;

struct TelemetryState {
    // frame timing
    std::uint64_t simTimeUs = 0;
    std::uint64_t renderTimeUs = 0;
    float dt = 0.0f;  // seconds of simulation time since the previous frame, 0 after restarts
    std::uint32_t frameCounter = 0;
    bool paused = true;

    // truck (channels)
    double pos[3] = {0.0, 0.0, 0.0};
    float heading = 0.0f, pitch = 0.0f, roll = 0.0f;
    float speed = 0.0f;  // m/s, negative when reversing
    float velLocal[3] = {0.0f, 0.0f, 0.0f};
    float angVelLocal[3] = {0.0f, 0.0f, 0.0f};  // rotations per second
    float accLocal[3] = {0.0f, 0.0f, 0.0f};
    float rpm = 0.0f;
    std::int32_t gear = 0;
    float inputThrottle = 0.0f, inputBrake = 0.0f;
    float effThrottle = 0.0f, effBrake = 0.0f;
    float fuel = 0.0f, fuelRange = 0.0f, fuelAvgConsumption = 0.0f;
    float wearEngine = 0.0f, wearTransmission = 0.0f, wearCabin = 0.0f;
    float wearChassis = 0.0f, wearWheels = 0.0f;
    float cruiseControl = 0.0f, speedLimit = 0.0f, odometer = 0.0f;
    bool engineOn = false;
    bool parkingBrake = false;

    // trailer 0 / job (channels)
    bool trailerConnected = false;
    float trailerWearChassis = 0.0f;
    float trailerCargoDamage = 0.0f;
    float jobCargoDamage = 0.0f;
    bool trailerCargoDamageAvailable = false;  // indexed/legacy trailer channel registered

    // common (channels)
    std::uint32_t gameTimeMin = 0;
    std::int32_t restStopMin = 0;

    // configuration
    bool hasTruck = false;
    float fuelCapacity = 0.0f;
    float rpmMax = 0.0f;
    std::int32_t gearsForward = 0;
    char truckBrand[kTelemetryShortText] = {};
    char truckBrandId[kTelemetryShortText] = {};
    char truckModelId[kTelemetryShortText] = {};
    char truckName[kTelemetryText] = {};
    char truckId[kTelemetryIdText] = {};  // "<brand_id>.<id>"
    char cargo[kTelemetryText] = {};
    char destinationCity[kTelemetryText] = {};
    char destinationCityId[kTelemetryText] = {};
    char destinationCompanyId[kTelemetryText] = {};
    char sourceCityId[kTelemetryText] = {};
    char sourceCompanyId[kTelemetryText] = {};
    std::uint32_t jobStartedCount = 0;    // job configuration arrived (truck at the source)
    std::uint32_t jobDeliveredCount = 0;  // job.delivered gameplay events

    std::uint32_t gameVersion = 0;  // SCS game telemetry version (major << 16 | minor)
};

inline float vectorLength(const float (&v)[3]) noexcept {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Magnitude of the truck body velocity. Rotation-invariant, so it equals the magnitude of the
// world-space velocity stored by the physics; falls back to the speedometer value.
inline float bodySpeed(const TelemetryState& tel) noexcept {
    const float local = vectorLength(tel.velLocal);
    return local > 0.0f ? local : std::fabs(tel.speed);
}

// Angular velocity magnitude in rad/s (SDK reports rotations per second).
inline float angularSpeedRad(const TelemetryState& tel) noexcept {
    constexpr float kTwoPi = 6.28318530718f;
    return vectorLength(tel.angVelLocal) * kTwoPi;
}

// Cargo damage of the (first) trailer; the job value serves when no trailer channel exists.
inline float cargoDamage(const TelemetryState& tel) noexcept {
    return tel.trailerCargoDamageAvailable ? tel.trailerCargoDamage : tel.jobCargoDamage;
}

}  // namespace e2t
