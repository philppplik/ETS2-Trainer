// "Telemetry as oracle" calibrators: find the memory behind a telemetry value by scanning,
// narrow the candidates while the value changes, confirm them by test-writing and watching the
// telemetry react, and keep validating them before every write. No hardcoded offsets.
//
// All methods run on the game thread inside frame_end (memory consistent with telemetry).
// Calibrator objects must live in static storage (see telemetry_state.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../shared/bridge_protocol.h"
#include "mem_scan.h"
#include "vector_io.h"

namespace e2t {

struct CalibrationContext {
    std::uint32_t frame = 0;
    std::uint64_t nowMs = 0;
    bool testWritesAllowed = false;      // verification writes: simulation running
    bool velocityWritesAllowed = false;  // not paused and 0 < dt <= 0.1 s
};

// A calibrator that can take part in one batched full-memory scan per frame.
class ScanClient {
public:
    virtual ~ScanClient() = default;
    virtual const char* name() const noexcept = 0;
    virtual bool wantsScan() const noexcept = 0;
    virtual void appendScanTargets(std::vector<mem::ScanTarget>& targets) const = 0;
    virtual void onScanResults(const mem::ScanResult* results, std::size_t count) = 0;
    // Called every frame while a background scan for this client runs: widen bounds[0..count)
    // (one per appended target) so they include the current telemetry value(s).
    virtual void widenScanTargets(mem::LiveBounds* /*bounds*/, std::size_t /*count*/) const {}

    // Background-scan bookkeeping (driven by the trainer).
    bool scanning() const noexcept { return scanning_; }
    void markScanStarted() noexcept { scanning_ = true; }
    void abandonScan() noexcept { scanning_ = false; }
    void deliverScanResults(const mem::ScanResult* results, std::size_t count) {
        scanning_ = false;
        onScanResults(results, count);
    }

protected:
    ScanClient() = default;
    ScanClient(const ScanClient&) = default;
    ScanClient& operator=(const ScanClient&) = default;

private:
    bool scanning_ = false;
};

float calibrationProgress(FeatureState state, float searchFraction, float verifyFraction) noexcept;
const char* featureStateName(FeatureState state) noexcept;

// ---- scalar (fuel, wear) ------------------------------------------------------------------

enum class ScalarEncoding : std::uint8_t { Float32, Float64 };

struct ScalarCandidate {
    std::uintptr_t address = 0;
    ScalarEncoding encoding = ScalarEncoding::Float32;
    std::uint8_t mismatches = 0;
};

struct ScalarConfig {
    const char* name = "";
    float minDistinctive = 0.0f;  // telemetry value must exceed this before a scan starts
    float (*testValue)(float current, float limit) = nullptr;  // value written to verify
};

class ScalarCalibrator final : public ScanClient {
public:
    explicit ScalarCalibrator(const ScalarConfig& config) noexcept : config_(config) {}

    void setRequested(bool requested);
    void reset();
    // Every unblocked frame. limit: tank capacity for fuel, 1 for wear. available=false holds
    // the state (e.g. trailer channel without trailer).
    void update(float value, float limit, bool available, const CalibrationContext& ctx);
    // Writes value to every confirmed address whose memory still holds the last known value.
    bool write(float value);

    bool requested() const noexcept { return requested_; }
    bool isActive() const noexcept { return state_ == FeatureState::Active; }
    FeatureState state() const noexcept { return requested_ ? state_ : FeatureState::Off; }
    std::uint32_t candidateCount() const noexcept;
    std::uint32_t confirmedCount() const noexcept;
    float progress() const noexcept;
    float value() const noexcept { return value_; }
    const char* failReason() const noexcept { return failReason_; }
    const std::vector<ScalarCandidate>& confirmed() const noexcept { return confirmed_; }

    const char* name() const noexcept override { return config_.name; }
    bool wantsScan() const noexcept override;
    void appendScanTargets(std::vector<mem::ScanTarget>& targets) const override;
    void onScanResults(const mem::ScanResult* results, std::size_t count) override;
    void widenScanTargets(mem::LiveBounds* bounds, std::size_t count) const override;

private:
    void filterCandidates();
    bool readyToVerify() const noexcept;
    void beginVerification();
    void stepVerification(const CalibrationContext& ctx);
    bool startTest(const ScalarCandidate& candidate);
    void finishTest(bool reacted);
    void finishVerification();
    void validateConfirmed();
    void retry(const char* reason);
    void clearSearch();

    ScalarConfig config_;
    FeatureState state_ = FeatureState::Off;
    bool requested_ = false;
    bool available_ = false;
    float value_ = 0.0f;
    std::uint32_t valueBits_ = 0;
    float lastDelta_ = 0.0f;  // |change| of the last frame: predicts values during a scan
    float limit_ = 0.0f;
    std::vector<ScalarCandidate> candidates_;
    std::vector<ScalarCandidate> confirmed_;
    std::uint32_t filterRounds_ = 0;
    std::uint32_t roundsWithoutReduction_ = 0;
    std::uint32_t retries_ = 0;
    bool overflowPending_ = false;
    std::uint32_t overflowBits_ = 0;
    std::size_t verifyIndex_ = 0;
    bool testPending_ = false;
    std::uint32_t testFrames_ = 0;
    float testValue_ = 0.0f;
    float testOriginal_ = 0.0f;
    std::uint64_t testSavedRaw_ = 0;
    std::uint64_t testWrittenRaw_ = 0;
    const char* failReason_ = "";
};

// ---- velocity vector (boost, nitro, stop, speed cap) --------------------------------------

struct VectorCandidate {
    std::uintptr_t address = 0;
    float lastMagnitude = 0.0f;
    VectorEncoding encoding = VectorEncoding::Float32x3;
    std::uint8_t mismatches = 0;
};

struct VelocityInput {
    float speed = 0.0f;         // |truck body velocity| (m/s)
    float angularSpeed = 0.0f;  // rad/s
    bool available = false;
};

class VelocityCalibrator final : public ScanClient {
public:
    void setRequested(bool requested);
    void reset();
    void update(const VelocityInput& input, const CalibrationContext& ctx);
    // Scales every validated confirmed vector v to magnitude m (v * m / |v|); m = 0 stops.
    bool setMagnitude(float magnitude);
    // Adds delta (m/s, in the vectors' own space) to every validated confirmed vector.
    bool addDelta(const double (&delta)[3]);
    // First validated confirmed vector (for direction/space detection).
    bool readFirst(double (&out)[3]) const;

    bool requested() const noexcept { return requested_; }
    bool isActive() const noexcept { return state_ == FeatureState::Active; }
    FeatureState state() const noexcept { return requested_ ? state_ : FeatureState::Off; }
    std::uint32_t candidateCount() const noexcept;
    std::uint32_t confirmedCount() const noexcept;
    float progress() const noexcept;
    const char* failReason() const noexcept { return failReason_; }
    const std::vector<VectorCandidate>& confirmed() const noexcept { return confirmed_; }

    const char* name() const noexcept override { return "velocity"; }
    bool wantsScan() const noexcept override;
    void appendScanTargets(std::vector<mem::ScanTarget>& targets) const override;
    void onScanResults(const mem::ScanResult* results, std::size_t count) override;
    void widenScanTargets(mem::LiveBounds* bounds, std::size_t count) const override;

private:
    bool isValidNow(const VectorCandidate& candidate, double (&v)[3]) const noexcept;
    void removeOverlapping();
    void dropSelfReferences();
    struct SavedVector {
        std::uintptr_t address;
        VectorEncoding encoding;
        std::uint8_t original[24];
        std::uint8_t written[24];
    };

    void filterCandidates();
    bool readyToVerify() const noexcept;
    void startVerification(const CalibrationContext& ctx);
    void checkVerification();
    bool overlapsSaved(std::uintptr_t address, std::size_t bytes) const noexcept;
    void restoreTestVectors();
    bool checkWriteEffect(const CalibrationContext& ctx);
    void validateConfirmed();
    void retry(const char* reason);
    void fail(const char* reason);
    void clearSearch();

    FeatureState state_ = FeatureState::Off;
    bool requested_ = false;
    bool available_ = false;
    float speed_ = 0.0f;
    float angularSpeed_ = 0.0f;
    // Write effectiveness: the confirmed group may contain pure copies; if the real body
    // vector disappears, significant writes stop changing the telemetry speed.
    bool effectPending_ = false;
    float effectTarget_ = 0.0f;
    float effectBefore_ = 0.0f;
    std::uint32_t ineffectiveWrites_ = 0;
    std::vector<VectorCandidate> candidates_;
    std::vector<VectorCandidate> confirmed_;
    float lastFilterSpeed_ = 0.0f;
    std::uint32_t filterRounds_ = 0;
    std::uint32_t roundsWithoutReduction_ = 0;
    std::uint32_t retries_ = 0;
    bool overflowPending_ = false;
    float overflowSpeed_ = 0.0f;
    bool testPending_ = false;
    std::uint32_t testFrames_ = 0;
    float testBaseSpeed_ = 0.0f;
    std::vector<SavedVector> testSaved_;
    const char* failReason_ = "";
};

// ---- world position (experimental teleport) -----------------------------------------------

struct PositionCandidate {
    std::uintptr_t address = 0;
    std::uint8_t mismatches = 0;
};

struct PositionInput {
    double pos[3] = {0.0, 0.0, 0.0};
    float speed = 0.0f;  // m/s, widens the match tolerance while moving
    bool available = false;
};

class PositionCalibrator final : public ScanClient {
public:
    void setRequested(bool requested);
    void reset();
    void update(const PositionInput& input, const CalibrationContext& ctx);
    // Writes the target to every confirmed triple that still matches the telemetry position.
    bool teleport(const double (&target)[3]);
    // Moves every confirmed triple by delta (e.g. lift when putting the truck upright).
    bool nudge(const double (&delta)[3]);

    bool requested() const noexcept { return requested_; }
    bool isActive() const noexcept { return state_ == FeatureState::Active; }
    FeatureState state() const noexcept { return requested_ ? state_ : FeatureState::Off; }
    std::uint32_t candidateCount() const noexcept;
    std::uint32_t confirmedCount() const noexcept;
    float progress() const noexcept;
    const char* failReason() const noexcept { return failReason_; }
    const std::vector<PositionCandidate>& confirmed() const noexcept { return confirmed_; }

    const char* name() const noexcept override { return "position"; }
    bool wantsScan() const noexcept override;
    void appendScanTargets(std::vector<mem::ScanTarget>& targets) const override;
    void onScanResults(const mem::ScanResult* results, std::size_t count) override;
    void widenScanTargets(mem::LiveBounds* bounds, std::size_t count) const override;

private:
    double tolerance() const noexcept;
    bool matchesTelemetry(std::uintptr_t address) const noexcept;
    void filterCandidates();
    void stepVerification(const CalibrationContext& ctx);
    bool startTest(const PositionCandidate& candidate);
    void finishTest(bool reacted);
    void finishVerification();
    void validateConfirmed();
    void retry(const char* reason);
    void clearSearch();

    FeatureState state_ = FeatureState::Off;
    bool requested_ = false;
    bool available_ = false;
    double pos_[3] = {0.0, 0.0, 0.0};
    float speed_ = 0.0f;
    std::vector<PositionCandidate> candidates_;
    std::vector<PositionCandidate> confirmed_;
    std::uint32_t retries_ = 0;
    std::size_t verifyIndex_ = 0;
    bool testPending_ = false;
    std::uint32_t testFrames_ = 0;
    double testBaseY_ = 0.0;
    double testOriginalY_ = 0.0;
    double testWrittenY_ = 0.0;
    const char* failReason_ = "";
};

}  // namespace e2t
