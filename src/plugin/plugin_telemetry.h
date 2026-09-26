// SDK channel registration, configuration parsing and bridge telemetry conversion.
#pragma once

#include "../../shared/bridge_protocol.h"
#include "scs_sdk_min.h"
#include "telemetry_state.h"

namespace e2t::plugin {

// Registers all channels into `state` (which must outlive the registration). Returns the
// number of channels that could not be registered (each is logged).
int registerChannels(const scs::TelemetryInitParamsV100& params, TelemetryState& state);

void applyConfiguration(const scs::Configuration& config, TelemetryState& state);

void fillBridgeTelemetry(const TelemetryState& state, bool truckersMp, Telemetry& out);

}  // namespace e2t::plugin
