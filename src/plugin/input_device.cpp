// Virtual SCS input device (see input_device.h).
#include "input_device.h"

#include <array>
#include <atomic>
#include <cstddef>

#include "log.h"

namespace e2t::input {
namespace {

constexpr std::size_t kButtonCount = static_cast<std::size_t>(Button::Count);
constexpr std::uint32_t kMaxQueuedPulses = 8;
constexpr std::uint32_t kPulseHoldFrames = 2;  // keep a press down long enough to register

// Semantical input names must match the game's control names (controls.sii "mix <name>").
constexpr scs::InputDeviceInput kInputs[kButtonCount] = {
    {"horn", "Horn", scs::kValueBool, 0},
    {"airhorn", "Air horn", scs::kValueBool, 0},
    {"light", "Lights", scs::kValueBool, 0},
    {"hblight", "High beam", scs::kValueBool, 0},
    {"lblinker", "Left blinker", scs::kValueBool, 0},
    {"rblinker", "Right blinker", scs::kValueBool, 0},
    {"flasher4way", "Hazard lights", scs::kValueBool, 0},
    {"beacon", "Beacon", scs::kValueBool, 0},
    {"wipers", "Wipers", scs::kValueBool, 0},
    {"cabinlight", "Cabin light", scs::kValueBool, 0},
    {"frontsuspup", "Front suspension up", scs::kValueBool, 0},
    {"frontsuspdwn", "Front suspension down", scs::kValueBool, 0},
    {"rearsuspup", "Rear suspension up", scs::kValueBool, 0},
    {"rearsuspdwn", "Rear suspension down", scs::kValueBool, 0},
    {"suspreset", "Suspension reset", scs::kValueBool, 0},
};

struct DeviceState {
    std::array<std::atomic<bool>, kButtonCount> held{};
    std::array<std::atomic<std::uint32_t>, kButtonCount> pulses{};
    std::array<bool, kButtonCount> reported{};
    std::array<bool, kButtonCount> pulseDown{};
    std::array<std::uint32_t, kButtonCount> pressFrame{};
    std::uint32_t frame = 0;
    std::atomic<bool> registered{false};
};

DeviceState g_device;  // static storage (never scanned: module image)

bool desiredState(std::size_t i) noexcept {
    if (!g_device.pulseDown[i] && !g_device.reported[i] &&
        g_device.pulses[i].load(std::memory_order_relaxed) > 0) {
        g_device.pulses[i].fetch_sub(1, std::memory_order_relaxed);
        g_device.pulseDown[i] = true;
        g_device.pressFrame[i] = g_device.frame;
    } else if (g_device.pulseDown[i] && g_device.frame >= g_device.pressFrame[i] + kPulseHoldFrames) {
        g_device.pulseDown[i] = false;
    }
    return g_device.pulseDown[i] || g_device.held[i].load(std::memory_order_relaxed);
}

scs::result_t E2T_SCSAPI onInputEvent(scs::InputEvent* event, scs::u32 flags, scs::context_t) {
    if ((flags & scs::kInputEventFlagFirstInFrame) != 0) {
        ++g_device.frame;
    }
    if (event == nullptr) {
        return scs::kResultNotFoundInput;
    }
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        const bool desired = desiredState(i);
        if (desired != g_device.reported[i]) {
            g_device.reported[i] = desired;
            event->inputIndex = static_cast<scs::u32>(i);
            event->valueBool = desired ? 1 : 0;
            return scs::kResultOk;
        }
    }
    return scs::kResultNotFoundInput;
}

void E2T_SCSAPI onInputActive(scs::u8 active, scs::context_t) {
    if (active == 0) {
        g_device.reported.fill(false);  // the game releases everything; resend held state later
        g_device.pulseDown.fill(false);
    }
}

}  // namespace

bool registerDevice(const scs::InputInitParamsV100& params) noexcept {
    if (params.registerDevice == nullptr) {
        return false;
    }
    static const scs::InputDevice kDevice{"ets2_trainer",       "ETS2 Trainer",
                                          scs::kInputDeviceTypeSemantical,
                                          static_cast<scs::u32>(kButtonCount),
                                          kInputs,              nullptr,
                                          &onInputActive,       &onInputEvent};
    const scs::result_t result = params.registerDevice(&kDevice);
    g_device.registered.store(result == scs::kResultOk, std::memory_order_relaxed);
    if (result != scs::kResultOk) {
        log::warn("input device: registration failed (%d)", result);
        return false;
    }
    log::info("input device: registered (%zu controls)", kButtonCount);
    return true;
}

void unregisterDevice() noexcept {
    releaseAll();
    g_device.registered.store(false, std::memory_order_relaxed);
}

bool registered() noexcept { return g_device.registered.load(std::memory_order_relaxed); }

void setHeld(Button button, bool held) noexcept {
    const auto i = static_cast<std::size_t>(button);
    if (i < kButtonCount) {
        g_device.held[i].store(held, std::memory_order_relaxed);
    }
}

void pulse(Button button) noexcept {
    const auto i = static_cast<std::size_t>(button);
    if (i < kButtonCount && g_device.pulses[i].load(std::memory_order_relaxed) < kMaxQueuedPulses) {
        g_device.pulses[i].fetch_add(1, std::memory_order_relaxed);
    }
}

void releaseAll() noexcept {
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        g_device.held[i].store(false, std::memory_order_relaxed);
        g_device.pulses[i].store(0, std::memory_order_relaxed);
    }
}

}  // namespace e2t::input
