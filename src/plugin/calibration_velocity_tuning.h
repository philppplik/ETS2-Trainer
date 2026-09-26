// Tuning of the velocity calibrator (internal, shared by calibration_velocity*.cpp).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "calibration.h"
#include "vector_io.h"

namespace e2t::velocity_tuning {

inline constexpr float kMinCalibrationSpeed = 5.0f;           // m/s, scan/verify only above
inline constexpr float kMinFilterSpeed = 1.0f;                // m/s
inline constexpr float kMaxCalibrationAngularSpeed = 0.05f;   // rad/s: calibrate while going straight
inline constexpr float kScanAbsTolerance = 0.05f;             // m/s
inline constexpr float kScanRelTolerance = 0.02f;
inline constexpr float kFilterSpeedDelta = 0.1f;              // m/s change between filter rounds
inline constexpr float kChangeMatchFraction = 0.5f;           // candidate change must follow telemetry
inline constexpr float kChangeAbsSlack = 0.02f;               // m/s
inline constexpr std::uint32_t kMinFilterRounds = 4;
inline constexpr std::size_t kMaxVerifyCandidates = 16;
inline constexpr std::uint32_t kStallRounds = 8;
inline constexpr std::size_t kMaxStallVerifyCandidates = 48;
inline constexpr double kVerifyScale = 1.10;
inline constexpr float kVerifyMinRatio = 1.05f;
inline constexpr float kVerifyMaxRatio = 1.20f;
inline constexpr std::uint32_t kVerifyFrames = 3;
inline constexpr float kValidateAbsTolerance = 0.15f;         // m/s
inline constexpr float kValidateRelTolerance = 0.05f;
inline constexpr float kLeverArmMeters = 3.0f;                // CoM vs. model origin while turning
inline constexpr double kMinScaleMagnitude = 0.5;             // m/s: no direction below this
inline constexpr float kOverflowRetrySpeedDelta = 2.0f;       // m/s
inline constexpr std::size_t kMaxHitsPerEncoding = 2'000'000;
inline constexpr std::uint32_t kMaxRetries = 3;
inline constexpr std::uint8_t kMaxMismatchFrames = 3;
inline constexpr float kMinEffectDelta = 0.1f;  // m/s: smaller writes drown in physics noise
inline constexpr std::uint32_t kMaxIneffectiveWrites = 5;

inline bool readMagnitude(const VectorCandidate& candidate, float& magnitude) noexcept {
    return readVectorMagnitude(candidate.address, candidate.encoding, magnitude);
}

inline float scanTolerance(float speed) noexcept {
    return std::max(kScanAbsTolerance, kScanRelTolerance * speed);
}

inline float validateTolerance(float speed, float angularSpeed) noexcept {
    return std::max(kValidateAbsTolerance, kValidateRelTolerance * speed) +
           angularSpeed * kLeverArmMeters;
}

}  // namespace e2t::velocity_tuning
