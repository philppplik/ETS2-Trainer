// Virtual input device registered through the official SCS input SDK (semantical type): its
// inputs are named after game controls, so the plugin can press horn, lights, hazards, beacon or
// the air suspension without any key binding. Drives the disco / horn concert / lowrider tricks.
#pragma once

#include <cstdint>

#include "scs_sdk_min.h"

namespace e2t::input {

enum class Button : std::uint8_t {
    Horn,
    AirHorn,
    Light,
    HighBeam,
    LeftBlinker,
    RightBlinker,
    Hazard,
    Beacon,
    Wipers,
    CabinLight,
    FrontSuspUp,
    FrontSuspDown,
    RearSuspUp,
    RearSuspDown,
    SuspReset,
    Count,
};

// Called from scs_input_init. Returns false (and logs) when the game refuses the device.
bool registerDevice(const scs::InputInitParamsV100& params) noexcept;
void unregisterDevice() noexcept;
bool registered() noexcept;

// Game-thread API used by the trainer.
void setHeld(Button button, bool held) noexcept;  // held as long as set
void pulse(Button button) noexcept;               // one short press (toggles lights etc.)
void releaseAll() noexcept;

}  // namespace e2t::input
