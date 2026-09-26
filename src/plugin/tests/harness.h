// Shared scenario harness: fake game + fake platform + trainer driven frame by frame.
// The harness object lives in static storage (module image), like the plugin's own state, so
// its copies of telemetry values are never scan candidates.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "../trainer.h"
#include "fake_game.h"
#include "fake_platform.h"

namespace e2t::test {

constexpr std::uint64_t kFrameMs = 16;
constexpr float kDistinctiveFuel = 347.318f;

struct Harness {
    FakePlatform platform;
    std::optional<FakeGame> game;
    std::optional<Trainer> trainer;
    TelemetryState tel;
    Control control{};
    Status status{};
    bool heartbeatRunning = true;
};

Harness& harness();
void setUp(float fuel, float speed, float wear = 0.0f);
void tearDown();
void frame();
void runFrames(int count);
void issueCommand(CommandType type, double a0 = 0.0, double a1 = 0.0, double a2 = 0.0);
bool messageContains(const char* text);

// Engine off for a few frames (constant fuel: the lagging copy matches during the scan),
// then engine on until the fuel calibration is active.
bool calibrateFuel(int maxFrames);
// Alternating throttle/brake around the current speed until the velocity calibration is active.
bool calibrateVelocity(int maxFrames);

template <class Predicate>
bool runUntil(Predicate done, int maxFrames) {
    for (int i = 0; i < maxFrames; ++i) {
        frame();
        if (done()) {
            return true;
        }
    }
    return false;
}

template <class Candidate>
bool containsAddress(const std::vector<Candidate>& candidates, std::uintptr_t address) {
    for (const Candidate& candidate : candidates) {
        if (candidate.address == address) {
            return true;
        }
    }
    return false;
}

template <class T>
std::uintptr_t addressOf(const T& object) {
    return reinterpret_cast<std::uintptr_t>(&object);
}

}  // namespace e2t::test
