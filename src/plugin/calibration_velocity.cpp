// VelocityCalibrator: finds the truck body's world-space velocity vector(s) by magnitude.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "calibration.h"
#include "calibration_velocity_tuning.h"
#include "log.h"
#include "vector_io.h"

namespace e2t {

using namespace velocity_tuning;
void VelocityCalibrator::setRequested(bool requested) {
    if (requested == requested_) {
        return;
    }
    requested_ = requested;
    if (requested) {
        if (state_ == FeatureState::Off) {
            state_ = FeatureState::WaitingForData;
        }
        return;
    }
    if (state_ != FeatureState::Active) {
        clearSearch();
        state_ = FeatureState::Off;
        retries_ = 0;
        failReason_ = "";
    }
}

void VelocityCalibrator::reset() {
    clearSearch();
    confirmed_.clear();
    state_ = FeatureState::Off;
    requested_ = false;
    retries_ = 0;
    overflowPending_ = false;
    failReason_ = "";
}

void VelocityCalibrator::update(const VelocityInput& input, const CalibrationContext& ctx) {
    available_ = input.available && std::isfinite(input.speed) && std::isfinite(input.angularSpeed);
    speed_ = available_ ? input.speed : 0.0f;
    angularSpeed_ = available_ ? input.angularSpeed : 0.0f;
    if (!requested_ || !available_) {
        return;
    }
    switch (state_) {
        case FeatureState::Calibrating: filterCandidates(); break;
        case FeatureState::Verifying:
            if (testPending_) {
                if (ctx.velocityWritesAllowed) {
                    checkVerification();
                }
            } else {
                startVerification(ctx);
            }
            break;
        case FeatureState::Active:
            if (checkWriteEffect(ctx)) {
                validateConfirmed();
            }
            break;
        default: break;
    }
}

bool VelocityCalibrator::checkWriteEffect(const CalibrationContext& ctx) {
    if (!effectPending_ || !ctx.velocityWritesAllowed) {
        return true;
    }
    effectPending_ = false;
    const bool effective = std::fabs(speed_ - effectTarget_) < std::fabs(speed_ - effectBefore_);
    ineffectiveWrites_ = effective ? 0 : ineffectiveWrites_ + 1;
    if (ineffectiveWrites_ < kMaxIneffectiveWrites) {
        return true;
    }
    log::warn("velocity: writes no longer change the truck speed - recalibrating");
    confirmed_.clear();
    ineffectiveWrites_ = 0;
    state_ = FeatureState::WaitingForData;
    return false;
}

bool VelocityCalibrator::setMagnitude(float magnitude) {
    if (state_ != FeatureState::Active || !std::isfinite(magnitude) || magnitude < 0.0f) {
        return false;
    }
    const float tolerance = validateTolerance(speed_, angularSpeed_);
    bool written = false;
    for (const VectorCandidate& candidate : confirmed_) {
        double v[3];
        if (candidate.mismatches != 0 || !readVector(candidate.address, candidate.encoding, v)) {
            continue;
        }
        const double length = lengthOf(v);
        if (std::fabs(length - speed_) > tolerance) {
            continue;  // memory no longer looks like the body velocity: do not touch it
        }
        if (magnitude > 0.0f && length < kMinScaleMagnitude) {
            continue;
        }
        const double factor = length > 0.0 ? magnitude / length : 0.0;
        for (double& component : v) {
            component *= factor;
        }
        std::uint8_t raw[kMaxVectorBytes];
        encodeVector(v, candidate.encoding, raw);
        written = mem::safeWrite(reinterpret_cast<void*>(candidate.address), raw,
                                 vectorBytes(candidate.encoding)) ||
                  written;
    }
    if (written) {
        if (std::fabs(magnitude - speed_) >= kMinEffectDelta) {
            effectPending_ = true;
            effectTarget_ = magnitude;
            effectBefore_ = speed_;
        }
        speed_ = magnitude;
    }
    return written;
}

std::uint32_t VelocityCalibrator::candidateCount() const noexcept {
    const bool searching = state_ == FeatureState::Calibrating ||
                           state_ == FeatureState::Verifying;
    return searching ? static_cast<std::uint32_t>(candidates_.size()) : 0;
}

std::uint32_t VelocityCalibrator::confirmedCount() const noexcept {
    return static_cast<std::uint32_t>(confirmed_.size());
}

float VelocityCalibrator::progress() const noexcept {
    const float search = static_cast<float>(filterRounds_) / static_cast<float>(kMinFilterRounds);
    const float verify = testPending_ ? static_cast<float>(testFrames_) / kVerifyFrames : 0.0f;
    return calibrationProgress(state_, search, verify);
}

bool VelocityCalibrator::wantsScan() const noexcept {
    if (!requested_ || !available_ || state_ != FeatureState::WaitingForData) {
        return false;
    }
    if (speed_ < kMinCalibrationSpeed || angularSpeed_ > kMaxCalibrationAngularSpeed) {
        return false;
    }
    return !overflowPending_ || std::fabs(speed_ - overflowSpeed_) >= kOverflowRetrySpeedDelta;
}

void VelocityCalibrator::appendScanTargets(std::vector<mem::ScanTarget>& targets) const {
    const double tolerance = scanTolerance(speed_);
    const double low = std::max(0.0, static_cast<double>(speed_) - tolerance);
    const double high = static_cast<double>(speed_) + tolerance;
    mem::ScanTarget floats = mem::ScanTarget::floatTripleMagnitude(low, high);
    floats.maxHits = kMaxHitsPerEncoding;
    mem::ScanTarget doubles = mem::ScanTarget::doubleTripleMagnitude(low, high);
    doubles.maxHits = kMaxHitsPerEncoding;
    targets.push_back(floats);
    targets.push_back(doubles);
}

void VelocityCalibrator::onScanResults(const mem::ScanResult* results, std::size_t count) {
    constexpr std::size_t kTargetsPerScan = 2;
    if (results == nullptr || count < kTargetsPerScan) {
        return;
    }
    if (results[0].overflow || results[1].overflow) {
        overflowPending_ = true;
        overflowSpeed_ = speed_;
        log::warn("velocity: too many matches at %.2f m/s - retrying at another speed",
                  static_cast<double>(speed_));
        return;
    }
    overflowPending_ = false;
    clearSearch();
    candidates_.reserve(results[0].hits.size() + results[1].hits.size());
    for (const std::uintptr_t address : results[0].hits) {
        candidates_.push_back({address, speed_, VectorEncoding::Float32x3, 0});
    }
    for (const std::uintptr_t address : results[1].hits) {
        candidates_.push_back({address, speed_, VectorEncoding::Float64x3, 0});
    }
    mem::parallelFilter(candidates_, [](VectorCandidate& candidate) {
        return readMagnitude(candidate, candidate.lastMagnitude);  // exact baseline
    });
    lastFilterSpeed_ = speed_;
    log::info("velocity: scan at %.2f m/s -> %zu float3 + %zu double3 candidates (%zu readable)",
              static_cast<double>(speed_), results[0].hits.size(), results[1].hits.size(),
              candidates_.size());
    if (candidates_.empty()) {
        retry("Geschwindigkeit nicht im Speicher gefunden");
        return;
    }
    state_ = FeatureState::Calibrating;
}

void VelocityCalibrator::filterCandidates() {
    const float speed = speed_;
    const float deltaTelemetry = speed - lastFilterSpeed_;
    if (std::fabs(deltaTelemetry) < kFilterSpeedDelta || speed < kMinFilterSpeed ||
        angularSpeed_ > kMaxCalibrationAngularSpeed) {
        return;
    }
    const float tolerance = scanTolerance(speed);
    const float changeTolerance = kChangeMatchFraction * std::fabs(deltaTelemetry) +
                                  kChangeAbsSlack;
    const std::size_t before = candidates_.size();
    mem::parallelFilter(candidates_, [=](VectorCandidate& candidate) {
        float magnitude = 0.0f;
        if (!readMagnitude(candidate, magnitude) || std::fabs(magnitude - speed) > tolerance) {
            return false;
        }
        const float deltaCandidate = magnitude - candidate.lastMagnitude;
        if (std::fabs(deltaCandidate - deltaTelemetry) > changeTolerance) {
            return false;  // constant (or unrelated) while the speed changed
        }
        candidate.lastMagnitude = magnitude;
        return true;
    });
    lastFilterSpeed_ = speed;
    ++filterRounds_;
    roundsWithoutReduction_ = candidates_.size() < before ? 0 : roundsWithoutReduction_ + 1;
    log::info("velocity: filter round %u at %.2f m/s -> %zu candidates", filterRounds_,
              static_cast<double>(speed), candidates_.size());
    if (candidates_.empty()) {
        retry("alle Kandidaten verworfen");
    } else if (readyToVerify()) {
        state_ = FeatureState::Verifying;
        testPending_ = false;
        log::info("velocity: verifying %zu candidate vectors together", candidates_.size());
    }
}

bool VelocityCalibrator::readyToVerify() const noexcept {
    const std::size_t count = candidates_.size();
    if (filterRounds_ >= kMinFilterRounds && count <= kMaxVerifyCandidates) {
        return true;
    }
    return roundsWithoutReduction_ >= kStallRounds && count <= kMaxStallVerifyCandidates;
}

void VelocityCalibrator::validateConfirmed() {
    const float tolerance = validateTolerance(speed_, angularSpeed_);
    for (VectorCandidate& candidate : confirmed_) {
        float magnitude = 0.0f;
        const bool valid = readMagnitude(candidate, magnitude) &&
                           std::fabs(magnitude - speed_) <= tolerance;
        candidate.mismatches = valid ? 0 : static_cast<std::uint8_t>(candidate.mismatches + 1);
        if (valid) {
            candidate.lastMagnitude = magnitude;
        }
    }
    const auto invalid = [](const VectorCandidate& candidate) {
        if (candidate.mismatches < kMaxMismatchFrames) {
            return false;
        }
        log::warn("velocity: 0x%llx no longer matches the truck speed - dropped",
                  static_cast<unsigned long long>(candidate.address));
        return true;
    };
    confirmed_.erase(std::remove_if(confirmed_.begin(), confirmed_.end(), invalid),
                     confirmed_.end());
    if (confirmed_.empty()) {
        state_ = FeatureState::WaitingForData;
        log::warn("velocity: calibration invalidated - recalibrating");
    }
}

void VelocityCalibrator::retry(const char* reason) {
    clearSearch();
    confirmed_.clear();
    ++retries_;
    if (retries_ > kMaxRetries) {
        fail(reason);
        return;
    }
    state_ = FeatureState::WaitingForData;
    log::info("velocity: retry %u/%u (%s)", retries_, kMaxRetries, reason);
}

void VelocityCalibrator::fail(const char* reason) {
    clearSearch();
    confirmed_.clear();
    state_ = FeatureState::Failed;
    failReason_ = reason;
    log::warn("velocity: calibration failed (%s)", reason);
}

void VelocityCalibrator::clearSearch() {
    if (testPending_) {
        restoreTestVectors();
    }
    std::vector<VectorCandidate>().swap(candidates_);
    testSaved_.clear();
    filterRounds_ = 0;
    roundsWithoutReduction_ = 0;
    testPending_ = false;
    effectPending_ = false;
    ineffectiveWrites_ = 0;
}

}  // namespace e2t
