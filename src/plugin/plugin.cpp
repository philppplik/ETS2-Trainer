// SCS telemetry plugin entry points: init/shutdown, event callbacks, frame loop glue.
//
// All state lives in static storage of this DLL (module image): the memory scanner skips the
// module, so the plugin's own copies of telemetry values never become calibration candidates.
#include <cstring>
#include <exception>
#include <optional>

#include "bridge.h"
#include "log.h"
#include "platform.h"
#include "plugin_telemetry.h"
#include "scs_sdk_min.h"
#include "telemetry_state.h"
#include "trainer.h"

namespace {

using namespace e2t;

constexpr double kMicrosecondsPerSecond = 1.0e6;
constexpr std::uint32_t kMaxExceptionLogs = 20;

TelemetryState g_state;
WinPlatform g_platform;
Bridge g_bridge;
Status g_status;
std::optional<Trainer> g_trainer;
std::uint64_t g_previousSimTimeUs = 0;
bool g_hasPreviousSimTime = false;
std::uint32_t g_exceptionLogs = 0;

void logException(const char* where, const char* what) noexcept {
    if (g_exceptionLogs < kMaxExceptionLogs) {
        ++g_exceptionLogs;
        log::error("%s: exception: %s", where, what);
    }
}

void onFrameStart(const scs::FrameStart& info) {
    const bool restarted = (info.flags & scs::kFrameStartFlagTimerRestart) != 0;
    if (restarted || !g_hasPreviousSimTime || info.simulationTime < g_previousSimTimeUs) {
        g_state.dt = 0.0f;
    } else {
        g_state.dt = static_cast<float>(
            static_cast<double>(info.simulationTime - g_previousSimTimeUs) /
            kMicrosecondsPerSecond);
    }
    g_previousSimTimeUs = info.simulationTime;
    g_hasPreviousSimTime = true;
    g_state.simTimeUs = info.simulationTime;
    g_state.renderTimeUs = info.renderTime;
}

void onFrameEnd() {
    ++g_state.frameCounter;
    if (!g_trainer) {
        return;
    }
    const Control control = g_bridge.readControl();
    g_trainer->onFrameEnd(g_state, control, g_status);

    Telemetry telemetry{};
    plugin::fillBridgeTelemetry(g_state, g_trainer->truckersMpDetected(), telemetry);
    g_bridge.publishTelemetry(telemetry);
    g_bridge.publishStatus(g_status);
}

void onGameplayEvent(const scs::GameplayEvent& event) {
    log::info("gameplay event: %s", event.id != nullptr ? event.id : "?");
}

void E2T_SCSAPI eventCallback(scs::event_t event, const void* info, scs::context_t) {
    try {
        switch (event) {
            case scs::kEventFrameStart:
                if (info != nullptr) {
                    onFrameStart(*static_cast<const scs::FrameStart*>(info));
                }
                break;
            case scs::kEventFrameEnd: onFrameEnd(); break;
            case scs::kEventPaused:
                g_state.paused = true;
                log::info("simulation paused");
                break;
            case scs::kEventStarted:
                g_state.paused = false;
                log::info("simulation started");
                break;
            case scs::kEventConfiguration:
                if (info != nullptr) {
                    plugin::applyConfiguration(*static_cast<const scs::Configuration*>(info),
                                               g_state);
                }
                break;
            case scs::kEventGameplay:
                if (info != nullptr) {
                    onGameplayEvent(*static_cast<const scs::GameplayEvent*>(info));
                }
                break;
            default: break;
        }
    } catch (const std::exception& ex) {
        logException("event callback", ex.what());
    } catch (...) {
        logException("event callback", "unknown");
    }
}

bool registerEvents(const scs::TelemetryInitParamsV100& params) {
    struct EventSpec {
        scs::event_t event;
        bool required;
    };
    // The configuration event must be registered during init to receive the initial config.
    constexpr EventSpec kEvents[] = {
        {scs::kEventConfiguration, true}, {scs::kEventFrameStart, true},
        {scs::kEventFrameEnd, true},      {scs::kEventPaused, true},
        {scs::kEventStarted, true},       {scs::kEventGameplay, false},
    };
    for (const EventSpec& spec : kEvents) {
        const scs::result_t result = params.registerForEvent(spec.event, &eventCallback, nullptr);
        if (result != scs::kResultOk) {
            log::write(spec.required ? log::Level::Error : log::Level::Warning,
                       "event %u not registered (result %d)", spec.event, result);
            if (spec.required) {
                return false;
            }
        }
    }
    return true;
}

bool isSupportedGame(const char* gameId) {
    return gameId != nullptr &&
           (std::strcmp(gameId, scs::kGameIdEts2) == 0 || std::strcmp(gameId, scs::kGameIdAts) == 0);
}

scs::result_t initialize(const scs::TelemetryInitParamsV100& params, scs::u32 version) {
    log::init(params.common.log, log::defaultLogPath());
    const scs::u32 game = params.common.gameVersion;
    log::info("ETS2 Trainer plugin build %u - %s (%s), telemetry API %u.%02u, game telemetry "
              "%u.%02u",
              kPluginBuild, params.common.gameName != nullptr ? params.common.gameName : "?",
              params.common.gameId != nullptr ? params.common.gameId : "?",
              scs::versionMajor(version), scs::versionMinor(version), scs::versionMajor(game),
              scs::versionMinor(game));
    if (!isSupportedGame(params.common.gameId)) {
        log::error("unsupported game - plugin not loaded");
        return scs::kResultUnsupported;
    }
    if (params.registerForEvent == nullptr || params.registerForChannel == nullptr) {
        return scs::kResultInvalidParameter;
    }
    g_state = TelemetryState{};
    g_state.gameVersion = game;
    g_status = Status{};
    g_hasPreviousSimTime = false;
    g_exceptionLogs = 0;
    if (!registerEvents(params)) {
        return scs::kResultGenericError;
    }
    const int missingChannels = plugin::registerChannels(params, g_state);
    if (missingChannels > 0) {
        log::warn("%d telemetry channel(s) unavailable", missingChannels);
    }
    g_bridge.open();
    g_trainer.emplace(g_platform);
    g_trainer->checkTruckersMpNow();
    log::info("plugin initialised");
    return scs::kResultOk;
}

}  // namespace

extern "C" scs::result_t E2T_SCSAPI scs_telemetry_init(scs::u32 version,
                                                       const scs::TelemetryInitParams* params) {
    if (version != scs::kTelemetryVersion_1_00 && version != scs::kTelemetryVersion_1_01) {
        return scs::kResultUnsupported;
    }
    if (params == nullptr) {
        return scs::kResultInvalidParameter;
    }
    try {
        const scs::result_t result =
            initialize(*static_cast<const scs::TelemetryInitParamsV100*>(params), version);
        if (result != scs::kResultOk) {
            g_trainer.reset();
            g_bridge.close();
            log::shutdown();
        }
        return result;
    } catch (const std::exception& ex) {
        log::error("init failed: %s", ex.what());
    } catch (...) {
        log::error("init failed: unknown exception");
    }
    g_trainer.reset();
    g_bridge.close();
    log::shutdown();
    return scs::kResultGenericError;
}

extern "C" void E2T_SCSAPI scs_telemetry_shutdown(void) {
    try {
        log::info("plugin shutting down");
        g_trainer.reset();
        g_bridge.close();
    } catch (...) {
        // nothing sensible left to do during shutdown
    }
    log::shutdown();
}
