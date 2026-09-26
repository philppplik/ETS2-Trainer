// PositionCalibrator (experimental): exact double[3] copies of truck.world.placement.
#include <algorithm>
#include <cmath>

#include "calibration.h"
#include "log.h"

namespace e2t {
namespace {

constexpr std::size_t kMaxVerifyCandidates = 64;
constexpr double kTestLift = 0.25;           // m written onto y
constexpr double kTestLiftTolerance = 0.1;   // m
constexpr std::uint32_t kVerifyFrames = 3;
constexpr std::uint32_t kMaxRetries = 3;
constexpr std::uint8_t kMaxMismatchFrames = 3;
constexpr std::size_t kMaxHits = 200'000;
constexpr std::uintptr_t kYOffset = sizeof(double);

}  // namespace

void PositionCalibrator::setRequested(bool requested) {
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

void PositionCalibrator::reset() {
    clearSearch();
    confirmed_.clear();
    state_ = FeatureState::Off;
    requested_ = false;
    retries_ = 0;
    failReason_ = "";
}

void PositionCalibrator::update(const PositionInput& input, const CalibrationContext& ctx) {
    bool finite = true;
    for (int i = 0; i < 3; ++i) {
        pos_[i] = input.pos[i];
        finite = finite && std::isfinite(input.pos[i]);
    }
    available_ = input.available && finite;
    if (!requested_ || !available_) {
        return;
    }
    switch (state_) {
        case FeatureState::Calibrating: filterCandidates(); break;
        case FeatureState::Verifying: stepVerification(ctx); break;
        case FeatureState::Active: validateConfirmed(); break;
        default: break;
    }
}

bool PositionCalibrator::teleport(const double (&target)[3]) {
    if (state_ != FeatureState::Active) {
        return false;
    }
    for (const double component : target) {
        if (!std::isfinite(component)) {
            return false;
        }
    }
    bool written = false;
    for (const PositionCandidate& candidate : confirmed_) {
        if (candidate.mismatches != 0 || !matchesTelemetry(candidate.address)) {
            continue;
        }
        written = mem::writeAt(candidate.address, target) || written;
    }
    if (written) {
        std::copy(std::begin(target), std::end(target), std::begin(pos_));
        log::info("position: teleported to %.1f %.1f %.1f", target[0], target[1], target[2]);
    }
    return written;
}

std::uint32_t PositionCalibrator::candidateCount() const noexcept {
    switch (state_) {
        case FeatureState::Calibrating: return static_cast<std::uint32_t>(candidates_.size());
        case FeatureState::Verifying:
            return static_cast<std::uint32_t>(candidates_.size() - verifyIndex_);
        default: return 0;
    }
}

std::uint32_t PositionCalibrator::confirmedCount() const noexcept {
    return static_cast<std::uint32_t>(confirmed_.size());
}

float PositionCalibrator::progress() const noexcept {
    const float verify = candidates_.empty() ? 0.0f
                                             : static_cast<float>(verifyIndex_) /
                                                   static_cast<float>(candidates_.size());
    return calibrationProgress(state_, 1.0f, verify);
}

bool PositionCalibrator::wantsScan() const noexcept {
    const bool atOrigin = pos_[0] == 0.0 && pos_[1] == 0.0 && pos_[2] == 0.0;
    return requested_ && available_ && !atOrigin && state_ == FeatureState::WaitingForData;
}

void PositionCalibrator::appendScanTargets(std::vector<mem::ScanTarget>& targets) const {
    mem::ScanTarget target = mem::ScanTarget::exactDoubleTriple(pos_);
    target.maxHits = kMaxHits;
    targets.push_back(target);
}

void PositionCalibrator::onScanResults(const mem::ScanResult* results, std::size_t count) {
    if (results == nullptr || count < 1) {
        return;
    }
    clearSearch();
    if (results[0].overflow) {
        retry("zu viele Treffer");
        return;
    }
    for (const std::uintptr_t address : results[0].hits) {
        candidates_.push_back({address, 0});
    }
    log::info("position: scan -> %zu candidates", candidates_.size());
    if (candidates_.empty()) {
        retry("Position nicht im Speicher gefunden");
        return;
    }
    state_ = FeatureState::Calibrating;
}

bool PositionCalibrator::matchesTelemetry(std::uintptr_t address) const noexcept {
    std::uint64_t bits[3];
    if (!mem::readAt(address, bits)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (bits[i] != mem::doubleBits(pos_[i])) {
            return false;
        }
    }
    return true;
}

void PositionCalibrator::filterCandidates() {
    const std::size_t before = candidates_.size();
    mem::parallelFilter(candidates_, [this](const PositionCandidate& candidate) {
        return matchesTelemetry(candidate.address);
    });
    if (candidates_.size() != before) {
        log::info("position: %zu candidates left", candidates_.size());
    }
    if (candidates_.empty()) {
        retry("alle Kandidaten verworfen");
    } else if (candidates_.size() <= kMaxVerifyCandidates) {
        state_ = FeatureState::Verifying;
        verifyIndex_ = 0;
        testPending_ = false;
        confirmed_.clear();
    }
}

void PositionCalibrator::stepVerification(const CalibrationContext& ctx) {
    if (!ctx.testWritesAllowed) {
        return;
    }
    if (testPending_) {
        ++testFrames_;
        const double lift = pos_[1] - testBaseY_;
        const bool reacted = std::fabs(lift - kTestLift) <= kTestLiftTolerance;
        if (reacted || testFrames_ >= kVerifyFrames) {
            finishTest(reacted);
        } else {
            return;
        }
    }
    while (verifyIndex_ < candidates_.size()) {
        if (startTest(candidates_[verifyIndex_])) {
            return;
        }
        ++verifyIndex_;
    }
    finishVerification();
}

bool PositionCalibrator::startTest(const PositionCandidate& candidate) {
    if (!matchesTelemetry(candidate.address)) {
        return false;
    }
    const double lifted = pos_[1] + kTestLift;
    if (!mem::writeAt(candidate.address + kYOffset, lifted)) {
        return false;
    }
    testBaseY_ = pos_[1];
    testOriginalY_ = pos_[1];
    testWrittenY_ = lifted;
    testFrames_ = 0;
    testPending_ = true;
    return true;
}

void PositionCalibrator::finishTest(bool reacted) {
    testPending_ = false;
    if (verifyIndex_ >= candidates_.size()) {
        return;
    }
    const PositionCandidate& candidate = candidates_[verifyIndex_];
    if (reacted) {
        confirmed_.push_back({candidate.address, 0});
        log::info("position: confirmed 0x%llx",
                  static_cast<unsigned long long>(candidate.address));
    } else {
        double currentY = 0.0;
        if (mem::readAt(candidate.address + kYOffset, currentY) &&
            mem::doubleBits(currentY) == mem::doubleBits(testWrittenY_)) {
            mem::writeAt(candidate.address + kYOffset, testOriginalY_);
        }
    }
    ++verifyIndex_;
}

void PositionCalibrator::finishVerification() {
    clearSearch();
    if (confirmed_.empty()) {
        retry("keine Position reagierte auf den Test");
        return;
    }
    state_ = FeatureState::Active;
    retries_ = 0;
    failReason_ = "";
    log::info("position: active with %zu address(es)", confirmed_.size());
}

void PositionCalibrator::validateConfirmed() {
    for (PositionCandidate& candidate : confirmed_) {
        const bool valid = matchesTelemetry(candidate.address);
        candidate.mismatches = valid ? 0 : static_cast<std::uint8_t>(candidate.mismatches + 1);
    }
    const auto invalid = [](const PositionCandidate& candidate) {
        return candidate.mismatches >= kMaxMismatchFrames;
    };
    confirmed_.erase(std::remove_if(confirmed_.begin(), confirmed_.end(), invalid),
                     confirmed_.end());
    if (confirmed_.empty()) {
        state_ = FeatureState::WaitingForData;
        log::warn("position: calibration invalidated - recalibrating");
    }
}

void PositionCalibrator::retry(const char* reason) {
    clearSearch();
    confirmed_.clear();
    ++retries_;
    if (retries_ > kMaxRetries) {
        state_ = FeatureState::Failed;
        failReason_ = reason;
        log::warn("position: calibration failed (%s)", reason);
        return;
    }
    state_ = FeatureState::WaitingForData;
    log::info("position: retry %u/%u (%s)", retries_, kMaxRetries, reason);
}

void PositionCalibrator::clearSearch() {
    if (testPending_) {
        finishTest(false);
    }
    std::vector<PositionCandidate>().swap(candidates_);
    verifyIndex_ = 0;
    testPending_ = false;
}

}  // namespace e2t
