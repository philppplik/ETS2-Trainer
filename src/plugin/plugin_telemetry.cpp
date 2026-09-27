#include "plugin_telemetry.h"

#include <cstdio>
#include <cstring>

#include "log.h"
#include "text_util.h"

namespace e2t::plugin {
namespace {

// ---- channel callbacks: the context points at the TelemetryState field ---------------------

void E2T_SCSAPI storeFloat(scs::string_t, scs::u32, const scs::Value* value,
                           scs::context_t context) {
    auto* target = static_cast<float*>(context);
    if (value == nullptr) {
        *target = 0.0f;  // only for channels registered with kChannelFlagNoValue
    } else if (value->type == scs::kValueFloat) {
        *target = value->asFloat;
    }
}

void E2T_SCSAPI storeBool(scs::string_t, scs::u32, const scs::Value* value,
                          scs::context_t context) {
    auto* target = static_cast<bool*>(context);
    if (value == nullptr) {
        *target = false;
    } else if (value->type == scs::kValueBool) {
        *target = value->asBool != 0;
    }
}

void E2T_SCSAPI storeS32(scs::string_t, scs::u32, const scs::Value* value,
                         scs::context_t context) {
    if (value != nullptr && value->type == scs::kValueS32) {
        *static_cast<std::int32_t*>(context) = value->asS32;
    }
}

void E2T_SCSAPI storeU32(scs::string_t, scs::u32, const scs::Value* value,
                         scs::context_t context) {
    if (value != nullptr && value->type == scs::kValueU32) {
        *static_cast<std::uint32_t*>(context) = value->asU32;
    }
}

void E2T_SCSAPI storeFVector(scs::string_t, scs::u32, const scs::Value* value,
                             scs::context_t context) {
    if (value != nullptr && value->type == scs::kValueFVector) {
        auto* target = static_cast<float*>(context);
        target[0] = value->asFVector.x;
        target[1] = value->asFVector.y;
        target[2] = value->asFVector.z;
    }
}

void E2T_SCSAPI storePlacement(scs::string_t, scs::u32, const scs::Value* value,
                               scs::context_t context) {
    if (value == nullptr || value->type != scs::kValueDPlacement) {
        return;
    }
    auto* state = static_cast<TelemetryState*>(context);
    const scs::DPlacement& placement = value->asDPlacement;
    state->pos[0] = placement.position.x;
    state->pos[1] = placement.position.y;
    state->pos[2] = placement.position.z;
    state->heading = placement.orientation.heading;
    state->pitch = placement.orientation.pitch;
    state->roll = placement.orientation.roll;
}

struct ChannelSpec {
    const char* name;
    scs::value_type_t type;
    scs::channel_callback_fn callback;
    void* target;
};

bool registerChannel(const scs::TelemetryInitParamsV100& params, const ChannelSpec& spec,
                     scs::u32 flags, bool logFailure) {
    const scs::result_t result = params.registerForChannel(spec.name, scs::kU32Nil, spec.type,
                                                           flags, spec.callback, spec.target);
    if (result != scs::kResultOk && logFailure) {
        log::warn("channel %s not registered (result %d)", spec.name, result);
    }
    return result == scs::kResultOk;
}

// Indexed trailer channel ("trailer.0.*", game telemetry >= 1.14) with legacy fallback.
bool registerTrailerChannel(const scs::TelemetryInitParamsV100& params, ChannelSpec indexed,
                            const char* legacyName) {
    constexpr scs::u32 kTrailerFlags = scs::kChannelFlagNoValue;  // reset when trailer is gone
    if (registerChannel(params, indexed, kTrailerFlags, false)) {
        return true;
    }
    ChannelSpec legacy = indexed;
    legacy.name = legacyName;
    if (registerChannel(params, legacy, kTrailerFlags, true)) {
        log::info("using legacy trailer channel %s", legacyName);
        return true;
    }
    return false;
}

bool isAttribute(const scs::NamedValue& attribute, const char* name, scs::value_type_t type) {
    return std::strcmp(attribute.name, name) == 0 && attribute.value.type == type;
}

const char* stringValue(const scs::NamedValue& attribute) {
    return attribute.value.asString != nullptr ? attribute.value.asString : "";
}

void clearTruck(TelemetryState& state) {
    state.hasTruck = false;
    state.fuelCapacity = 0.0f;
    state.rpmMax = 0.0f;
    state.gearsForward = 0;
    state.truckBrand[0] = state.truckBrandId[0] = state.truckModelId[0] = '\0';
    state.truckName[0] = state.truckId[0] = '\0';
}

void applyTruckConfig(const scs::NamedValue* attributes, TelemetryState& state) {
    clearTruck(state);
    if (attributes == nullptr || attributes->name == nullptr) {
        log::info("truck configuration cleared (no truck)");
        return;
    }
    for (const scs::NamedValue* a = attributes; a->name != nullptr; ++a) {
        if (isAttribute(*a, scs::config::kAttrBrandId, scs::kValueString)) {
            copyUtf8(state.truckBrandId, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrBrand, scs::kValueString)) {
            copyUtf8(state.truckBrand, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrId, scs::kValueString)) {
            copyUtf8(state.truckModelId, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrName, scs::kValueString)) {
            copyUtf8(state.truckName, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrFuelCapacity, scs::kValueFloat)) {
            state.fuelCapacity = a->value.asFloat;
        } else if (isAttribute(*a, scs::config::kAttrRpmLimit, scs::kValueFloat)) {
            state.rpmMax = a->value.asFloat;
        } else if (isAttribute(*a, scs::config::kAttrForwardGears, scs::kValueU32)) {
            state.gearsForward = static_cast<std::int32_t>(a->value.asU32);
        }
    }
    std::snprintf(state.truckId, sizeof(state.truckId), "%s.%s", state.truckBrandId,
                  state.truckModelId);
    state.hasTruck = true;
    log::info("truck: %s %s (%s), tank %.0f l", state.truckBrand, state.truckName, state.truckId,
              static_cast<double>(state.fuelCapacity));
}

void applyJobConfig(const scs::NamedValue* attributes, TelemetryState& state) {
    const bool hadJob = state.cargo[0] != '\0';
    state.cargo[0] = state.destinationCity[0] = '\0';
    state.destinationCityId[0] = state.destinationCompanyId[0] = '\0';
    state.sourceCityId[0] = state.sourceCompanyId[0] = '\0';
    if (attributes == nullptr) {
        return;
    }
    for (const scs::NamedValue* a = attributes; a->name != nullptr; ++a) {
        if (isAttribute(*a, scs::config::kAttrCargo, scs::kValueString)) {
            copyUtf8(state.cargo, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrDestinationCity, scs::kValueString)) {
            copyUtf8(state.destinationCity, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrDestinationCityId, scs::kValueString)) {
            copyUtf8(state.destinationCityId, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrDestinationCompanyId, scs::kValueString)) {
            copyUtf8(state.destinationCompanyId, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrSourceCityId, scs::kValueString)) {
            copyUtf8(state.sourceCityId, stringValue(*a));
        } else if (isAttribute(*a, scs::config::kAttrSourceCompanyId, scs::kValueString)) {
            copyUtf8(state.sourceCompanyId, stringValue(*a));
        }
    }
    if (state.cargo[0] != '\0') {
        if (!hadJob) {
            ++state.jobStartedCount;  // the truck stands at the source company right now
        }
        log::info("job: %s %s/%s -> %s/%s", state.cargo, state.sourceCompanyId, state.sourceCityId,
                  state.destinationCompanyId, state.destinationCityId);
    }
}

}  // namespace

int registerChannels(const scs::TelemetryInitParamsV100& params, TelemetryState& s) {
    namespace ch = scs::channel;
    const ChannelSpec specs[] = {
        {ch::kTruckWorldPlacement, scs::kValueDPlacement, &storePlacement, &s},
        {ch::kTruckSpeed, scs::kValueFloat, &storeFloat, &s.speed},
        {ch::kTruckLocalLinearVelocity, scs::kValueFVector, &storeFVector, s.velLocal},
        {ch::kTruckLocalAngularVelocity, scs::kValueFVector, &storeFVector, s.angVelLocal},
        {ch::kTruckLocalLinearAcceleration, scs::kValueFVector, &storeFVector, s.accLocal},
        {ch::kTruckEngineRpm, scs::kValueFloat, &storeFloat, &s.rpm},
        {ch::kTruckEngineGear, scs::kValueS32, &storeS32, &s.gear},
        {ch::kTruckInputThrottle, scs::kValueFloat, &storeFloat, &s.inputThrottle},
        {ch::kTruckInputBrake, scs::kValueFloat, &storeFloat, &s.inputBrake},
        {ch::kTruckEffectiveThrottle, scs::kValueFloat, &storeFloat, &s.effThrottle},
        {ch::kTruckEffectiveBrake, scs::kValueFloat, &storeFloat, &s.effBrake},
        {ch::kTruckFuel, scs::kValueFloat, &storeFloat, &s.fuel},
        {ch::kTruckFuelRange, scs::kValueFloat, &storeFloat, &s.fuelRange},
        {ch::kTruckFuelAverageConsumption, scs::kValueFloat, &storeFloat, &s.fuelAvgConsumption},
        {ch::kTruckWearEngine, scs::kValueFloat, &storeFloat, &s.wearEngine},
        {ch::kTruckWearTransmission, scs::kValueFloat, &storeFloat, &s.wearTransmission},
        {ch::kTruckWearCabin, scs::kValueFloat, &storeFloat, &s.wearCabin},
        {ch::kTruckWearChassis, scs::kValueFloat, &storeFloat, &s.wearChassis},
        {ch::kTruckWearWheels, scs::kValueFloat, &storeFloat, &s.wearWheels},
        {ch::kTruckCruiseControl, scs::kValueFloat, &storeFloat, &s.cruiseControl},
        {ch::kTruckNavigationSpeedLimit, scs::kValueFloat, &storeFloat, &s.speedLimit},
        {ch::kTruckOdometer, scs::kValueFloat, &storeFloat, &s.odometer},
        {ch::kTruckEngineEnabled, scs::kValueBool, &storeBool, &s.engineOn},
        {ch::kTruckParkingBrake, scs::kValueBool, &storeBool, &s.parkingBrake},
        {ch::kGameTime, scs::kValueU32, &storeU32, &s.gameTimeMin},
        {ch::kNextRestStop, scs::kValueS32, &storeS32, &s.restStopMin},
        {ch::kJobCargoDamage, scs::kValueFloat, &storeFloat, &s.jobCargoDamage},
    };
    int failures = 0;
    for (const ChannelSpec& spec : specs) {
        failures += registerChannel(params, spec, scs::kChannelFlagNone, true) ? 0 : 1;
    }
    failures += registerTrailerChannel(
                    params, {ch::kTrailer0Connected, scs::kValueBool, &storeBool, &s.trailerConnected},
                    ch::kTrailerLegacyConnected)
                    ? 0
                    : 1;
    failures += registerTrailerChannel(params,
                                       {ch::kTrailer0WearChassis, scs::kValueFloat, &storeFloat,
                                        &s.trailerWearChassis},
                                       ch::kTrailerLegacyWearChassis)
                    ? 0
                    : 1;
    s.trailerCargoDamageAvailable = registerTrailerChannel(
        params,
        {ch::kTrailer0CargoDamage, scs::kValueFloat, &storeFloat, &s.trailerCargoDamage},
        ch::kTrailerLegacyCargoDamage);
    failures += s.trailerCargoDamageAvailable ? 0 : 1;
    return failures;
}

void applyConfiguration(const scs::Configuration& config, TelemetryState& state) {
    if (config.id == nullptr) {
        return;
    }
    if (std::strcmp(config.id, scs::config::kTruck) == 0) {
        applyTruckConfig(config.attributes, state);
    } else if (std::strcmp(config.id, scs::config::kJob) == 0) {
        applyJobConfig(config.attributes, state);
    }
}

void fillBridgeTelemetry(const TelemetryState& s, bool truckersMp, Telemetry& out) {
    out = Telemetry{};
    out.frameCounter = s.frameCounter;
    out.gameVersion = s.gameVersion;
    out.flags = (s.paused ? kFlagPaused : 0u) | (s.engineOn ? kFlagEngineOn : 0u) |
                (s.parkingBrake ? kFlagParkingBrake : 0u) |
                (s.trailerConnected ? kFlagTrailerAttached : 0u) |
                (truckersMp ? kFlagTruckersMpDetected : 0u) | (s.hasTruck ? kFlagHasTruck : 0u);
    out.simTimeUs = s.simTimeUs;
    out.renderTimeUs = s.renderTimeUs;
    out.posX = s.pos[0];
    out.posY = s.pos[1];
    out.posZ = s.pos[2];
    out.heading = s.heading;
    out.pitch = s.pitch;
    out.roll = s.roll;
    out.speed = s.speed;
    for (int i = 0; i < 3; ++i) {
        out.velLocal[i] = s.velLocal[i];
        out.accLocal[i] = s.accLocal[i];
    }
    out.rpm = s.rpm;
    out.rpmMax = s.rpmMax;
    out.gear = s.gear;
    out.gearsForward = s.gearsForward;
    out.inputThrottle = s.inputThrottle;
    out.inputBrake = s.inputBrake;
    out.effThrottle = s.effThrottle;
    out.effBrake = s.effBrake;
    out.fuel = s.fuel;
    out.fuelCapacity = s.fuelCapacity;
    out.fuelRange = s.fuelRange;
    out.fuelAvgConsumption = s.fuelAvgConsumption;
    out.wearEngine = s.wearEngine;
    out.wearTransmission = s.wearTransmission;
    out.wearCabin = s.wearCabin;
    out.wearChassis = s.wearChassis;
    out.wearWheels = s.wearWheels;
    out.trailerWearChassis = s.trailerWearChassis;
    out.cargoDamage = cargoDamage(s);
    out.speedLimit = s.speedLimit;
    out.cruiseControl = s.cruiseControl;
    out.odometer = s.odometer;
    out.gameTimeMin = s.gameTimeMin;
    out.restStopMin = s.restStopMin;
    copyUtf8(out.truckBrand, s.truckBrand);
    copyUtf8(out.truckName, s.truckName);
    copyUtf8(out.truckId, s.hasTruck ? s.truckId : "");
    copyUtf8(out.cargo, s.cargo);
    copyUtf8(out.destinationCity, s.destinationCity);
    copyUtf8(out.destinationCityId, s.destinationCityId);
    copyUtf8(out.destinationCompanyId, s.destinationCompanyId);
    copyUtf8(out.sourceCityId, s.sourceCityId);
    copyUtf8(out.sourceCompanyId, s.sourceCompanyId);
    if (s.cargo[0] != '\0') {
        out.flags |= kFlagHasJob;
    }
}

}  // namespace e2t::plugin
