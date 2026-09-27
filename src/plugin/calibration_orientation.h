// OrientationCalibrator: finds the truck body's rotation next to its confirmed position.
//
// The physics body keeps position and rotation close together, so instead of a full memory scan
// the calibrator searches a small window around every confirmed position address for a
// quaternion or rotation matrix that equals the telemetry orientation (heading/pitch/roll).
// Hypotheses are filtered while the truck turns and confirmed by a small test rotation that the
// telemetry heading must follow. Used to put the truck upright, spin it and roll it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../shared/bridge_protocol.h"
#include "calibration.h"

namespace e2t {

enum class OrientationLayout : std::uint8_t {
    QuatXYZW,
    QuatXYZWConj,
    QuatWXYZ,
    QuatWXYZConj,
    Mat3Rows,  // 9 floats, m[r][c] = f[r*3+c]
    Mat3Cols,  // 9 floats, m[r][c] = f[c*3+r]
    Mat4Rows,  // 3x4/4x4 row-major, m[r][c] = f[r*4+c]
    Mat4Cols,  // 4x4 column-major, m[r][c] = f[c*4+r]
};

struct OrientationCandidate {
    std::uintptr_t address = 0;
    OrientationLayout layout = OrientationLayout::QuatXYZW;
    std::int8_t pitchSign = 1;
    std::int8_t rollSign = 1;
    std::uint8_t mismatches = 0;
};

struct OrientationInput {
    float heading = 0.0f;  // SCS turns (0..1, counter-clockwise from above)
    float pitch = 0.0f;
    float roll = 0.0f;
    const std::vector<PositionCandidate>* anchors = nullptr;  // confirmed position addresses
    bool available = false;
};

class OrientationCalibrator {
public:
    void setRequested(bool requested);
    void reset();
    void update(const OrientationInput& input, const CalibrationContext& ctx);
    // Writes the rotation given in SCS turns to every confirmed, still valid address.
    bool write(float heading, float pitch, float roll);

    bool requested() const noexcept { return requested_; }
    bool isActive() const noexcept { return state_ == FeatureState::Active; }
    FeatureState state() const noexcept { return requested_ ? state_ : FeatureState::Off; }
    std::uint32_t candidateCount() const noexcept;
    std::uint32_t confirmedCount() const noexcept;
    float progress() const noexcept;
    const char* failReason() const noexcept { return failReason_; }

private:
    void search(const std::vector<PositionCandidate>& anchors);
    void filterCandidates();
    bool readyToVerify() const noexcept;
    void stepVerification(const CalibrationContext& ctx);
    bool startTest(const OrientationCandidate& candidate);
    void finishTest(bool reacted);
    void finishVerification();
    void validateConfirmed();
    bool matchesTelemetry(const OrientationCandidate& candidate) const noexcept;
    void retry(const char* reason);
    void clearSearch();

    FeatureState state_ = FeatureState::Off;
    bool requested_ = false;
    bool available_ = false;
    float heading_ = 0.0f;
    float pitch_ = 0.0f;
    float roll_ = 0.0f;
    float lastFilterHeading_ = 0.0f;
    std::uint32_t filterRounds_ = 0;
    std::uint32_t framesSinceSearch_ = 0;
    std::uint32_t retries_ = 0;
    std::vector<OrientationCandidate> candidates_;
    std::vector<OrientationCandidate> confirmed_;
    std::size_t verifyIndex_ = 0;
    bool testPending_ = false;
    std::uint32_t testFrames_ = 0;
    float testBaseHeading_ = 0.0f;
    std::uint8_t testSaved_[64] = {};
    std::uint8_t testWritten_[64] = {};
    const char* failReason_ = "";
};

}  // namespace e2t
