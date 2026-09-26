#include "fake_game.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace e2t::test {
namespace {

constexpr std::size_t kPageBytes = 4096;
constexpr std::size_t kTruckOffsetInPage = 256;  // looks like an object inside a heap block
constexpr std::size_t kNoiseFloats = 1u << 20;   // 4 MiB of random floats
constexpr std::size_t kNoiseMutationsPerFrame = 2048;
constexpr float kNoiseMax = 40.0f;
constexpr float kConsumptionPerSecond = 0.05f;  // litres at full throttle share
constexpr float kIdleConsumptionShare = 0.2f;
constexpr float kWearPerSecond = 2.0e-5f;
constexpr float kEngineAccel = 1.2f;     // m/s^2 at full throttle
constexpr float kBrakeDecel = 3.0f;      // m/s^2 at full brake
constexpr float kDragPerSecond = 0.01f;  // 1/s
constexpr double kGroundY = 12.5;
constexpr double kGroundSnap = 0.1;  // fraction of the height error removed per frame
constexpr double kStartX = 1234.5678;
constexpr double kStartZ = -9876.5432;
constexpr double kMetersPerKm = 1000.0;
constexpr float kStartHeading = 0.6f;  // rad: world velocity has x and z components
constexpr std::uint64_t kTruckTypeTag = 0x4B55525454ull;
constexpr float kTwoPi = 6.28318530718f;
constexpr float kRandomScale = 1.0f / 4294967296.0f;

}  // namespace

void FakeGame::PageDeleter::operator()(void* block) const noexcept {
    VirtualFree(block, 0, MEM_RELEASE);
}

FakeGame::PageBlock FakeGame::allocatePages(std::size_t bytes) {
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (block == nullptr) {
        throw std::bad_alloc();
    }
    return PageBlock(block);
}

FakeTruckMemory* FakeGame::truckIn(void* block) noexcept {
    return reinterpret_cast<FakeTruckMemory*>(static_cast<char*>(block) + kTruckOffsetInPage);
}

FakeGame::FakeGame()
    : truckBlock_(allocatePages(kPageBytes)),
      decoyBlock_(allocatePages(kPageBytes)),
      noiseBlock_(allocatePages(kNoiseFloats * sizeof(float))),
      truck_(truckIn(truckBlock_.get())),
      decoys_(static_cast<FakeDecoys*>(decoyBlock_.get())),
      noise_(static_cast<float*>(noiseBlock_.get())) {
    for (std::size_t i = 0; i < kNoiseFloats; ++i) {
        noise_[i] = randomNoise();
    }
    reset(100.0f, 0.0f, 0.0f);
}

void FakeGame::reset(float fuel, float speed, float wear) {
    std::memset(truck_, 0, sizeof(FakeTruckMemory));
    truck_->typeTag = kTruckTypeTag;
    truck_->fuel = fuel;
    truck_->wearEngine = wear;
    truck_->odometerKm = 123456.0;
    heading_ = kStartHeading;
    const float forward[3] = {-std::sin(heading_), 0.0f, -std::cos(heading_)};
    for (int i = 0; i < 3; ++i) {
        truck_->velocity[i] = forward[i] * speed;
        truck_->unrelated0[i] = static_cast<float>(i) * 0.5f;
    }
    truck_->position[0] = kStartX;
    truck_->position[1] = kGroundY;
    truck_->position[2] = kStartZ;
    prevFuel_ = fuel;
    std::copy(std::begin(truck_->velocity), std::end(truck_->velocity), std::begin(prevVelocity_));
    updateDecoys();
    input_ = DriveInput{};
    trailer_ = false;
    paused_ = false;
    simTimeUs_ = 0;
    lastAccel_ = 0.0f;
}

void FakeGame::step(TelemetryState& tel) {
    if (!paused_) {
        simulate();
    }
    simTimeUs_ += kFrameUs;  // SCS simulation time keeps running while paused
    fillTelemetry(tel);
}

void FakeGame::simulate() {
    FakeTruckMemory& m = *truck_;  // read back: trainer writes take effect here
    float fuel = m.fuel;
    if (input_.engineOn) {
        fuel -= kConsumptionPerSecond * (kIdleConsumptionShare + input_.throttle) * kDt;
        m.wearEngine += kWearPerSecond * kDt;
    }
    m.fuel = std::min(kCapacity, std::max(0.0f, fuel));

    heading_ += input_.yawRate * kDt;
    const float forward[3] = {-std::sin(heading_), 0.0f, -std::cos(heading_)};
    const float along = m.velocity[0] * forward[0] + m.velocity[1] * forward[1] +
                        m.velocity[2] * forward[2];
    const float accel = input_.throttle * kEngineAccel - input_.brake * kBrakeDecel -
                        kDragPerSecond * along;
    const float newSpeed = std::max(0.0f, along + accel * kDt);
    lastAccel_ = (newSpeed - along) / kDt;  // physics only, excludes trainer injections
    for (int i = 0; i < 3; ++i) {
        m.velocity[i] = forward[i] * newSpeed;
        m.position[i] += static_cast<double>(m.velocity[i]) * kDt;
    }
    m.position[1] += (kGroundY - m.position[1]) * kGroundSnap;
    m.odometerKm += static_cast<double>(newSpeed * kDt) / kMetersPerKm;
    updateDecoys();
    mutateNoise();
}

void FakeGame::updateDecoys() {
    FakeDecoys& d = *decoys_;
    const FakeTruckMemory& m = *truck_;
    d.laggingFuel = prevFuel_;
    d.mirrorFuel = m.fuel;
    d.mirrorFuelDouble = static_cast<double>(m.fuel);
    for (int i = 0; i < 3; ++i) {
        d.laggingVelocity[i] = prevVelocity_[i];
        d.mirrorVelocity[i] = m.velocity[i];
        d.mirrorPosition[i] = m.position[i];
        prevVelocity_[i] = m.velocity[i];
    }
    prevFuel_ = m.fuel;
}

void FakeGame::mutateNoise() {
    for (std::size_t k = 0; k < kNoiseMutationsPerFrame; ++k) {
        noise_[nextRandom() % kNoiseFloats] = randomNoise();
    }
}

void FakeGame::fillTelemetry(TelemetryState& tel) const {
    const FakeTruckMemory& m = *truck_;
    tel.paused = paused_;
    tel.dt = kDt;
    tel.simTimeUs = simTimeUs_;
    tel.renderTimeUs = simTimeUs_;
    ++tel.frameCounter;
    tel.hasTruck = true;
    tel.fuelCapacity = kCapacity;
    tel.fuel = m.fuel;
    tel.wearEngine = m.wearEngine;
    tel.trailerConnected = trailer_;
    tel.engineOn = input_.engineOn;
    for (int i = 0; i < 3; ++i) {
        tel.pos[i] = m.position[i];
    }
    const float currentSpeed = speed();
    tel.speed = currentSpeed;
    tel.velLocal[0] = tel.velLocal[1] = 0.0f;
    tel.velLocal[2] = -currentSpeed;  // vehicle forward is -Z
    tel.accLocal[0] = tel.accLocal[1] = 0.0f;
    tel.accLocal[2] = paused_ ? 0.0f : -lastAccel_;
    tel.angVelLocal[0] = tel.angVelLocal[2] = 0.0f;
    tel.angVelLocal[1] = input_.yawRate / kTwoPi;  // rotations per second
    tel.inputThrottle = tel.effThrottle = input_.throttle;
    tel.inputBrake = tel.effBrake = input_.brake;
}

void FakeGame::reallocateTruck(ReallocMode mode) {
    PageBlock fresh = allocatePages(kPageBytes);
    FakeTruckMemory* moved = truckIn(fresh.get());
    std::memcpy(moved, truck_, sizeof(FakeTruckMemory));
    if (mode == ReallocMode::KeepStaleCopy) {
        staleBlocks_.push_back(std::move(truckBlock_));
    }
    truckBlock_ = std::move(fresh);  // ReleaseOld: the previous block is freed here
    truck_ = moved;
}

float FakeGame::speed() const noexcept {
    const float* v = truck_->velocity;
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

const FakeTruckMemory* FakeGame::staleCopy() const noexcept {
    return staleBlocks_.empty() ? nullptr : truckIn(staleBlocks_.back().get());
}

std::uintptr_t FakeGame::fuelAddress() const noexcept {
    return reinterpret_cast<std::uintptr_t>(&truck_->fuel);
}

std::uintptr_t FakeGame::wearAddress() const noexcept {
    return reinterpret_cast<std::uintptr_t>(&truck_->wearEngine);
}

std::uintptr_t FakeGame::velocityAddress() const noexcept {
    return reinterpret_cast<std::uintptr_t>(&truck_->velocity[0]);
}

std::uintptr_t FakeGame::positionAddress() const noexcept {
    return reinterpret_cast<std::uintptr_t>(&truck_->position[0]);
}

std::uint32_t FakeGame::nextRandom() noexcept {
    rng_ ^= rng_ << 13;  // xorshift32
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

float FakeGame::randomNoise() noexcept {
    return static_cast<float>(nextRandom()) * kRandomScale * kNoiseMax;
}

}  // namespace e2t::test
