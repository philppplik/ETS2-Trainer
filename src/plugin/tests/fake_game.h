// In-process fake "game" for the self-test: heap-like objects holding the values the trainer
// calibrates, decoy copies, noise, and a tiny physics step that reads its state back from
// memory (so trainer writes take effect) and reports telemetry like the real game would.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../telemetry_state.h"

namespace e2t::test {

struct FakeTruckMemory {  // lives in its own VirtualAlloc'ed pages (MEM_PRIVATE, RW)
    std::uint64_t typeTag;
    float unrelated0[5];
    float fuel;  // litres
    float unrelated1[3];
    float wearEngine;
    double odometerKm;
    float velocity[3];  // world space, m/s
    float unrelated2;
    double position[3];  // world space, m
};

struct FakeDecoys {  // copies the game keeps but never reads back
    float laggingFuel;         // previous frame's fuel
    float mirrorFuel;          // HUD copy, equals fuel every frame
    double mirrorFuelDouble;   // double copy of the fuel
    float laggingVelocity[3];  // previous frame's velocity
    float mirrorVelocity[3];   // render copy
    double mirrorPosition[3];  // camera target copy
};

struct DriveInput {
    bool engineOn = false;
    float throttle = 0.0f;
    float brake = 0.0f;
    float yawRate = 0.0f;  // rad/s
};

enum class ReallocMode { ReleaseOld, KeepStaleCopy };

class FakeGame {
public:
    static constexpr float kDt = 1.0f / 60.0f;
    static constexpr std::uint64_t kFrameUs = 16'667;
    static constexpr float kCapacity = 600.0f;

    FakeGame();
    FakeGame(const FakeGame&) = delete;
    FakeGame& operator=(const FakeGame&) = delete;

    void reset(float fuel, float speed, float wear);
    void step(TelemetryState& tel);
    void setInput(const DriveInput& input) noexcept { input_ = input; }
    const DriveInput& input() const noexcept { return input_; }
    void setTrailer(bool attached) noexcept { trailer_ = attached; }
    void setPaused(bool paused) noexcept { paused_ = paused; }
    void reallocateTruck(ReallocMode mode);

    FakeTruckMemory& truck() noexcept { return *truck_; }
    FakeDecoys& decoys() noexcept { return *decoys_; }
    float speed() const noexcept;
    float lastNaturalAccel() const noexcept { return lastAccel_; }
    const FakeTruckMemory* staleCopy() const noexcept;

    std::uintptr_t fuelAddress() const noexcept;
    std::uintptr_t wearAddress() const noexcept;
    std::uintptr_t velocityAddress() const noexcept;
    std::uintptr_t positionAddress() const noexcept;

private:
    struct PageDeleter {
        void operator()(void* block) const noexcept;
    };
    using PageBlock = std::unique_ptr<void, PageDeleter>;

    static PageBlock allocatePages(std::size_t bytes);
    static FakeTruckMemory* truckIn(void* block) noexcept;
    void simulate();
    void updateDecoys();
    void mutateNoise();
    void fillTelemetry(TelemetryState& tel) const;
    std::uint32_t nextRandom() noexcept;
    float randomNoise() noexcept;

    PageBlock truckBlock_;
    PageBlock decoyBlock_;
    PageBlock noiseBlock_;
    std::vector<PageBlock> staleBlocks_;
    FakeTruckMemory* truck_ = nullptr;
    FakeDecoys* decoys_ = nullptr;
    float* noise_ = nullptr;
    DriveInput input_{};
    float heading_ = 0.6f;
    float prevFuel_ = 0.0f;
    float prevVelocity_[3] = {0.0f, 0.0f, 0.0f};
    float lastAccel_ = 0.0f;
    bool trailer_ = false;
    bool paused_ = false;
    std::uint64_t simTimeUs_ = 0;
    std::uint32_t rng_ = 0x12345678u;
};

}  // namespace e2t::test
