// VelocityCalibrator verification: scale all remaining candidates together and watch the
// telemetry speed react.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "calibration.h"
#include "calibration_velocity_tuning.h"
#include "log.h"
#include "vector_io.h"

namespace e2t {

using namespace velocity_tuning;

void VelocityCalibrator::startVerification(const CalibrationContext& ctx) {
    if (!ctx.velocityWritesAllowed || speed_ < kMinCalibrationSpeed ||
        angularSpeed_ > kMaxCalibrationAngularSpeed) {
        return;
    }
    // Most exact magnitude first; overlapping triples (e.g. prev.z, vel.x, vel.y) also track the
    // speed, and writing two of them would scale shared components twice.
    const float tolerance = validateTolerance(speed_, angularSpeed_);
    std::vector<std::pair<float, std::size_t>> order;
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        float magnitude = 0.0f;
        if (readMagnitude(candidates_[i], magnitude) &&
            std::fabs(magnitude - speed_) <= tolerance) {
            order.emplace_back(std::fabs(magnitude - speed_), i);
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    testSaved_.clear();
    for (const auto& entry : order) {
        const VectorCandidate& candidate = candidates_[entry.second];
        const std::size_t bytes = vectorBytes(candidate.encoding);
        if (overlapsSaved(candidate.address, bytes)) {
            continue;
        }
        SavedVector saved{candidate.address, candidate.encoding, {}, {}};
        double v[3];
        if (!mem::safeRead(reinterpret_cast<const void*>(candidate.address), saved.original,
                           bytes) ||
            !decodeVector(saved.original, candidate.encoding, v)) {
            continue;
        }
        for (double& component : v) {
            component *= kVerifyScale;
        }
        encodeVector(v, candidate.encoding, saved.written);
        if (mem::safeWrite(reinterpret_cast<void*>(candidate.address), saved.written, bytes)) {
            testSaved_.push_back(saved);
        }
    }
    if (testSaved_.empty()) {
        retry("Kandidaten vor dem Test ungueltig geworden");
        return;
    }
    testPending_ = true;
    testFrames_ = 0;
    testBaseSpeed_ = speed_;
    log::info("velocity: scaled %zu vectors by %.2f at %.2f m/s", testSaved_.size(),
              kVerifyScale, static_cast<double>(speed_));
}

void VelocityCalibrator::checkVerification() {
    ++testFrames_;
    const float ratio = testBaseSpeed_ > 0.0f ? speed_ / testBaseSpeed_ : 0.0f;
    if (ratio > kVerifyMinRatio && ratio < kVerifyMaxRatio) {
        confirmed_.clear();
        for (const SavedVector& saved : testSaved_) {
            confirmed_.push_back({saved.address, speed_, saved.encoding, 0});
        }
        testSaved_.clear();
        std::vector<VectorCandidate>().swap(candidates_);
        testPending_ = false;
        effectPending_ = false;
        ineffectiveWrites_ = 0;
        state_ = FeatureState::Active;
        retries_ = 0;
        failReason_ = "";
        log::info("velocity: telemetry reacted (x%.3f) - active with %zu vector(s)",
                  static_cast<double>(ratio), confirmed_.size());
        return;
    }
    if (testFrames_ >= kVerifyFrames) {
        restoreTestVectors();
        fail("Geschwindigkeit reagierte nicht auf den Test");
    }
}

bool VelocityCalibrator::overlapsSaved(std::uintptr_t address, std::size_t bytes) const noexcept {
    for (const SavedVector& saved : testSaved_) {
        const mem::Range range{saved.address, saved.address + vectorBytes(saved.encoding)};
        if (range.overlaps(address, bytes)) {
            return true;
        }
    }
    return false;
}

void VelocityCalibrator::restoreTestVectors() {
    for (const SavedVector& saved : testSaved_) {
        std::uint8_t current[kMaxVectorBytes];
        const std::size_t bytes = vectorBytes(saved.encoding);
        if (mem::safeRead(reinterpret_cast<const void*>(saved.address), current, bytes) &&
            std::memcmp(current, saved.written, bytes) == 0) {
            mem::safeWrite(reinterpret_cast<void*>(saved.address), saved.original, bytes);
        }
    }
    testSaved_.clear();
    testPending_ = false;
}

}  // namespace e2t
