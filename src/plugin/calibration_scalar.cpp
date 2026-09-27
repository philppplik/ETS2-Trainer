// ScalarCalibrator: float telemetry channel -> float or double in memory (fuel, wear values).
#include <algorithm>
#include <cmath>

#include "calibration.h"
#include "log.h"

namespace e2t {
namespace {

constexpr int kMaxTrailingZeroBits = 11;  // "< 12 trailing zero mantissa bits"
constexpr std::uint32_t kMinFilterRounds = 2;
constexpr std::size_t kMaxVerifyCandidates = 8;
constexpr std::uint32_t kStallRounds = 6;  // rounds without reduction: many true copies
constexpr std::size_t kMaxStallVerifyCandidates = 32;
constexpr std::uint32_t kVerifyFrames = 3;
constexpr float kVerifyToleranceFraction = 0.25f;  // of |test - original|
constexpr std::uint32_t kMaxRetries = 3;
constexpr std::uint8_t kMaxMismatchFrames = 3;
constexpr double kDoubleSlackFraction = 1.0e-7;  // > half a float ULP: doubles rounding to it
constexpr double kPredictFrames = 3.0;           // frames of change covered ahead of time
constexpr float kMaxPredictFraction = 0.05f;     // ignore jumps (refuel, repair)

bool readRaw(const ScalarCandidate& candidate, std::uint64_t& raw) noexcept {
    if (candidate.encoding == ScalarEncoding::Float32) {
        std::uint32_t bits = 0;
        if (!mem::readAt(candidate.address, bits)) {
            return false;
        }
        raw = bits;
        return true;
    }
    return mem::readAt(candidate.address, raw);
}

bool writeRaw(const ScalarCandidate& candidate, std::uint64_t raw) noexcept {
    if (candidate.encoding == ScalarEncoding::Float32) {
        return mem::writeAt(candidate.address, static_cast<std::uint32_t>(raw));
    }
    return mem::writeAt(candidate.address, raw);
}

std::uint64_t encode(const ScalarCandidate& candidate, float value) noexcept {
    if (candidate.encoding == ScalarEncoding::Float32) {
        return mem::floatBits(value);
    }
    return mem::doubleBits(static_cast<double>(value));
}

bool matchesValue(const ScalarCandidate& candidate, std::uint32_t bits,
                  const mem::DoubleRange& range) noexcept {
    std::uint64_t raw = 0;
    if (!readRaw(candidate, raw)) {
        return false;
    }
    if (candidate.encoding == ScalarEncoding::Float32) {
        return static_cast<std::uint32_t>(raw) == bits;
    }
    return range.contains(raw);
}

const char* encodingName(ScalarEncoding encoding) noexcept {
    return encoding == ScalarEncoding::Float32 ? "float" : "double";
}

}  // namespace

void ScalarCalibrator::setRequested(bool requested) {
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
    if (state_ != FeatureState::Active) {  // keep confirmed addresses, drop searches
        clearSearch();
        state_ = FeatureState::Off;
        retries_ = 0;
        failReason_ = "";
    }
}

void ScalarCalibrator::reset() {
    clearSearch();
    confirmed_.clear();
    state_ = FeatureState::Off;
    requested_ = false;
    retries_ = 0;
    overflowPending_ = false;
    failReason_ = "";
}

void ScalarCalibrator::update(float value, float limit, bool available,
                              const CalibrationContext& ctx) {
    const std::uint32_t bits = mem::floatBits(value);
    const bool changed = bits != valueBits_;
    if (changed && mem::isFiniteFloatBits(bits) && mem::isFiniteFloatBits(valueBits_)) {
        lastDelta_ = std::min(std::fabs(value - value_), std::fabs(value) * kMaxPredictFraction);
    }
    value_ = value;
    valueBits_ = bits;
    limit_ = limit;
    available_ = available && mem::isFiniteFloatBits(bits);
    if (!requested_ || !available_) {
        return;
    }
    switch (state_) {
        case FeatureState::Calibrating:
            if (changed) {
                filterCandidates();
            }
            break;
        case FeatureState::Verifying: stepVerification(ctx); break;
        case FeatureState::Active: validateConfirmed(); break;
        default: break;
    }
}

bool ScalarCalibrator::write(float value) {
    if (state_ != FeatureState::Active || !mem::isFiniteFloatBits(mem::floatBits(value))) {
        return false;
    }
    const mem::DoubleRange range = mem::doubleRangeRoundingTo(value_);
    bool written = false;
    for (const ScalarCandidate& candidate : confirmed_) {
        if (candidate.mismatches != 0 || !matchesValue(candidate, valueBits_, range)) {
            continue;  // never write memory that does not hold the last known value
        }
        written = writeRaw(candidate, encode(candidate, value)) || written;
    }
    if (written) {
        value_ = value;
        valueBits_ = mem::floatBits(value);
    }
    return written;
}

std::uint32_t ScalarCalibrator::candidateCount() const noexcept {
    switch (state_) {
        case FeatureState::Calibrating: return static_cast<std::uint32_t>(candidates_.size());
        case FeatureState::Verifying:
            return static_cast<std::uint32_t>(candidates_.size() - verifyIndex_);
        default: return 0;
    }
}

std::uint32_t ScalarCalibrator::confirmedCount() const noexcept {
    return static_cast<std::uint32_t>(confirmed_.size());
}

float ScalarCalibrator::progress() const noexcept {
    const float search = static_cast<float>(filterRounds_) / static_cast<float>(kMinFilterRounds);
    const float verify = candidates_.empty() ? 0.0f
                                             : static_cast<float>(verifyIndex_) /
                                                   static_cast<float>(candidates_.size());
    return calibrationProgress(state_, search, verify);
}

bool ScalarCalibrator::wantsScan() const noexcept {
    if (!requested_ || !available_ || state_ != FeatureState::WaitingForData) {
        return false;
    }
    if (overflowPending_ && overflowBits_ == valueBits_) {
        return false;  // wait for the next distinctive value
    }
    return mem::isDistinctiveFloat(value_, config_.minDistinctive, kMaxTrailingZeroBits);
}

// Background scans use value ranges that widenScanTargets() grows with every telemetry value
// seen while the scan runs; onScanResults() then keeps only exact matches of the current value.
void ScalarCalibrator::appendScanTargets(std::vector<mem::ScanTarget>& targets) const {
    const double value = static_cast<double>(value_);
    const double predict = kPredictFrames * static_cast<double>(lastDelta_);
    const double slack = std::fabs(value) * kDoubleSlackFraction + predict;
    targets.push_back(mem::ScanTarget::floatRange(value - predict, value + predict));
    targets.push_back(mem::ScanTarget::doubleValueRange(value - slack, value + slack));
}

void ScalarCalibrator::widenScanTargets(mem::LiveBounds* bounds, std::size_t count) const {
    constexpr std::size_t kTargetsPerScan = 2;
    if (bounds == nullptr || count < kTargetsPerScan || !mem::isFiniteFloatBits(valueBits_)) {
        return;
    }
    const double value = static_cast<double>(value_);
    const double predict = kPredictFrames * static_cast<double>(lastDelta_);
    const double slack = std::fabs(value) * kDoubleSlackFraction + predict;
    bounds[0].widen(0, value - predict, value + predict);
    bounds[1].widen(0, value - slack, value + slack);
}

void ScalarCalibrator::onScanResults(const mem::ScanResult* results, std::size_t count) {
    constexpr std::size_t kTargetsPerScan = 2;
    if (results == nullptr || count < kTargetsPerScan) {
        return;
    }
    if (results[0].overflow || results[1].overflow) {
        overflowPending_ = true;
        overflowBits_ = valueBits_;
        log::warn("%s: too many matches for %.6g - waiting for a more distinctive value",
                  config_.name, static_cast<double>(value_));
        return;
    }
    overflowPending_ = false;
    clearSearch();
    candidates_.reserve(results[0].hits.size() + results[1].hits.size());
    for (const std::uintptr_t address : results[0].hits) {
        candidates_.push_back({address, ScalarEncoding::Float32, 0});
    }
    for (const std::uintptr_t address : results[1].hits) {
        candidates_.push_back({address, ScalarEncoding::Float64, 0});
    }
    log::info("%s: scan for %.6g -> %zu float + %zu double candidates", config_.name,
              static_cast<double>(value_), results[0].hits.size(), results[1].hits.size());
    // The range scan also caught values from earlier frames: keep exact current matches only.
    const std::uint32_t bits = valueBits_;
    const mem::DoubleRange current = mem::doubleRangeRoundingTo(value_);
    mem::parallelFilter(candidates_, [bits, &current](const ScalarCandidate& candidate) {
        return matchesValue(candidate, bits, current);
    });
    log::info("%s: %zu candidates match the current value", config_.name, candidates_.size());
    if (candidates_.empty()) {
        retry("Wert nicht im Speicher gefunden");
        return;
    }
    state_ = FeatureState::Calibrating;
}

void ScalarCalibrator::filterCandidates() {
    const std::size_t before = candidates_.size();
    const std::uint32_t bits = valueBits_;
    const mem::DoubleRange range = mem::doubleRangeRoundingTo(value_);
    mem::parallelFilter(candidates_, [bits, &range](const ScalarCandidate& candidate) {
        return matchesValue(candidate, bits, range);
    });
    ++filterRounds_;
    roundsWithoutReduction_ = candidates_.size() < before ? 0 : roundsWithoutReduction_ + 1;
    if (candidates_.size() != before) {
        log::info("%s: filter round %u -> %zu candidates", config_.name, filterRounds_,
                  candidates_.size());
    }
    if (candidates_.empty()) {
        retry("alle Kandidaten verworfen");
        return;
    }
    if (readyToVerify()) {
        beginVerification();
    }
}

bool ScalarCalibrator::readyToVerify() const noexcept {
    const std::size_t count = candidates_.size();
    if (filterRounds_ >= kMinFilterRounds && count <= kMaxVerifyCandidates) {
        return true;
    }
    return roundsWithoutReduction_ >= kStallRounds && count <= kMaxStallVerifyCandidates;
}

void ScalarCalibrator::beginVerification() {
    state_ = FeatureState::Verifying;
    verifyIndex_ = 0;
    testPending_ = false;
    confirmed_.clear();
    log::info("%s: verifying %zu candidates", config_.name, candidates_.size());
}

void ScalarCalibrator::stepVerification(const CalibrationContext& ctx) {
    if (!ctx.testWritesAllowed) {
        return;  // paused: neither write nor count frames
    }
    if (testPending_) {
        ++testFrames_;
        const float delta = std::fabs(testValue_ - testOriginal_);
        const bool reacted = std::fabs(value_ - testValue_) <= kVerifyToleranceFraction * delta;
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

bool ScalarCalibrator::startTest(const ScalarCandidate& candidate) {
    if (config_.testValue == nullptr ||
        !matchesValue(candidate, valueBits_, mem::doubleRangeRoundingTo(value_))) {
        return false;
    }
    const float test = config_.testValue(value_, limit_);
    const std::uint32_t testBits = mem::floatBits(test);
    std::uint64_t saved = 0;
    if (!mem::isFiniteFloatBits(testBits) || testBits == valueBits_ || !readRaw(candidate, saved)) {
        return false;
    }
    const std::uint64_t written = encode(candidate, test);
    if (!writeRaw(candidate, written)) {
        return false;
    }
    testSavedRaw_ = saved;
    testWrittenRaw_ = written;
    testValue_ = test;
    testOriginal_ = value_;
    testFrames_ = 0;
    testPending_ = true;
    return true;
}

void ScalarCalibrator::finishTest(bool reacted) {
    testPending_ = false;
    if (verifyIndex_ >= candidates_.size()) {
        return;
    }
    const ScalarCandidate& candidate = candidates_[verifyIndex_];
    if (reacted) {
        confirmed_.push_back({candidate.address, candidate.encoding, 0});
        log::info("%s: confirmed %s at 0x%llx", config_.name, encodingName(candidate.encoding),
                  static_cast<unsigned long long>(candidate.address));
    } else {
        std::uint64_t current = 0;
        if (readRaw(candidate, current) && current == testWrittenRaw_) {
            writeRaw(candidate, testSavedRaw_);  // undo the test write on a non-reacting copy
        }
    }
    ++verifyIndex_;
}

void ScalarCalibrator::finishVerification() {
    clearSearch();
    if (confirmed_.empty()) {
        retry("keine Adresse reagierte auf den Test");
        return;
    }
    state_ = FeatureState::Active;
    retries_ = 0;
    failReason_ = "";
    log::info("%s: active with %zu address(es)", config_.name, confirmed_.size());
}

void ScalarCalibrator::validateConfirmed() {
    const mem::DoubleRange range = mem::doubleRangeRoundingTo(value_);
    for (ScalarCandidate& candidate : confirmed_) {
        const bool valid = matchesValue(candidate, valueBits_, range);
        candidate.mismatches = valid ? 0 : static_cast<std::uint8_t>(candidate.mismatches + 1);
    }
    const auto invalid = [this](const ScalarCandidate& candidate) {
        if (candidate.mismatches < kMaxMismatchFrames) {
            return false;
        }
        log::warn("%s: 0x%llx no longer matches telemetry - dropped", config_.name,
                  static_cast<unsigned long long>(candidate.address));
        return true;
    };
    confirmed_.erase(std::remove_if(confirmed_.begin(), confirmed_.end(), invalid),
                     confirmed_.end());
    if (confirmed_.empty()) {
        state_ = FeatureState::WaitingForData;
        log::warn("%s: calibration invalidated - recalibrating", config_.name);
    }
}

void ScalarCalibrator::retry(const char* reason) {
    clearSearch();
    confirmed_.clear();
    ++retries_;
    if (retries_ > kMaxRetries) {
        state_ = FeatureState::Failed;
        failReason_ = reason;
        log::warn("%s: calibration failed after %u attempts (%s)", config_.name, retries_,
                  reason);
        return;
    }
    state_ = FeatureState::WaitingForData;
    log::info("%s: retry %u/%u (%s)", config_.name, retries_, kMaxRetries, reason);
}

void ScalarCalibrator::clearSearch() {
    if (testPending_) {
        finishTest(false);  // restores the pending test write
    }
    std::vector<ScalarCandidate>().swap(candidates_);
    filterRounds_ = 0;
    roundsWithoutReduction_ = 0;
    verifyIndex_ = 0;
    testPending_ = false;
}

}  // namespace e2t
