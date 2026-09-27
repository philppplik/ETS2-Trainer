// OrientationCalibrator: rotation of the truck body next to its confirmed position (see header).
#include "calibration_orientation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "mem_access.h"

namespace e2t {
namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr std::uintptr_t kWindowBytes = 1024;  // searched on each side of a position address
constexpr std::size_t kMaxLayoutBytes = 64;
constexpr std::uintptr_t kPositionBytes = 3 * sizeof(double);
constexpr double kQuatDotMin = 0.9995;  // about 3.6 degrees
constexpr double kMatrixTolerance = 0.03;
constexpr double kUnitTolerance = 0.01;
constexpr float kFilterHeadingDelta = 1.5f / 360.0f;  // turns
constexpr std::size_t kMaxVerifyCandidates = 12;
constexpr std::size_t kMaxIdleVerifyCandidates = 4;
constexpr std::uint32_t kIdleFramesBeforeVerify = 300;
constexpr std::size_t kMaxCandidates = 4096;
constexpr float kTestTurn = 5.0f / 360.0f;
constexpr float kTestTolerance = 2.5f / 360.0f;
constexpr std::uint32_t kVerifyFrames = 3;
constexpr std::uint32_t kMaxRetries = 3;
constexpr std::uint8_t kMaxMismatchFrames = 5;
constexpr OrientationLayout kLayouts[] = {
    OrientationLayout::QuatXYZW, OrientationLayout::QuatXYZWConj, OrientationLayout::QuatWXYZ,
    OrientationLayout::QuatWXYZConj, OrientationLayout::Mat3Rows,  OrientationLayout::Mat3Cols,
    OrientationLayout::Mat4Rows, OrientationLayout::Mat4Cols,
};
constexpr std::int8_t kSigns[2] = {1, -1};

struct Quat {
    double w, x, y, z;
};

struct Mat3 {
    double m[3][3];
};

Quat multiply(const Quat& a, const Quat& b) noexcept {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

// SCS: heading around +Y (counter-clockwise from above), then pitch around X, then roll around Z.
Quat fromEuler(double heading, double pitch, double roll, int pitchSign, int rollSign) noexcept {
    const double h = heading * kTwoPi * 0.5;
    const double p = pitchSign * pitch * kTwoPi * 0.5;
    const double r = rollSign * roll * kTwoPi * 0.5;
    const Quat qy{std::cos(h), 0.0, std::sin(h), 0.0};
    const Quat qx{std::cos(p), std::sin(p), 0.0, 0.0};
    const Quat qz{std::cos(r), 0.0, 0.0, std::sin(r)};
    return multiply(multiply(qy, qx), qz);
}

Mat3 toMatrix(const Quat& q) noexcept {
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {{{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
             {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
             {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}}};
}

bool isQuat(OrientationLayout layout) noexcept {
    return layout <= OrientationLayout::QuatWXYZConj;
}

std::size_t layoutBytes(OrientationLayout layout) noexcept {
    switch (layout) {
        case OrientationLayout::Mat3Rows:
        case OrientationLayout::Mat3Cols: return 9 * sizeof(float);
        case OrientationLayout::Mat4Rows: return 12 * sizeof(float);
        case OrientationLayout::Mat4Cols: return 11 * sizeof(float);
        default: return 4 * sizeof(float);
    }
}

// Float index of matrix element (r, c) for a matrix layout.
std::size_t matrixIndex(OrientationLayout layout, int r, int c) noexcept {
    switch (layout) {
        case OrientationLayout::Mat3Rows: return static_cast<std::size_t>(r * 3 + c);
        case OrientationLayout::Mat3Cols: return static_cast<std::size_t>(c * 3 + r);
        case OrientationLayout::Mat4Rows: return static_cast<std::size_t>(r * 4 + c);
        default: return static_cast<std::size_t>(c * 4 + r);
    }
}

bool loadFloats(const std::uint8_t* raw, std::size_t count, float* out) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, raw + i * sizeof(float), sizeof(bits));
        if (!mem::isFiniteFloatBits(bits)) {
            return false;
        }
        out[i] = mem::floatFromBits(bits);
    }
    return true;
}

bool decodeQuat(const std::uint8_t* raw, OrientationLayout layout, Quat& q) noexcept {
    float f[4];
    if (!loadFloats(raw, 4, f)) {
        return false;
    }
    const bool wFirst = layout == OrientationLayout::QuatWXYZ ||
                        layout == OrientationLayout::QuatWXYZConj;
    q = wFirst ? Quat{f[0], f[1], f[2], f[3]} : Quat{f[3], f[0], f[1], f[2]};
    if (layout == OrientationLayout::QuatXYZWConj || layout == OrientationLayout::QuatWXYZConj) {
        q = {q.w, -q.x, -q.y, -q.z};
    }
    const double norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return std::fabs(norm - 1.0) <= kUnitTolerance;
}

bool matchesAt(const std::uint8_t* raw, OrientationLayout layout, const Quat& expected) noexcept {
    if (isQuat(layout)) {
        Quat q{};
        if (!decodeQuat(raw, layout, q)) {
            return false;
        }
        const double dot = q.w * expected.w + q.x * expected.x + q.y * expected.y + q.z * expected.z;
        return std::fabs(dot) >= kQuatDotMin;
    }
    float f[12];
    if (!loadFloats(raw, layoutBytes(layout) / sizeof(float), f)) {
        return false;
    }
    const Mat3 m = toMatrix(expected);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(f[matrixIndex(layout, r, c)] - m.m[r][c]) > kMatrixTolerance) {
                return false;
            }
        }
    }
    return true;
}

// Replaces the rotation inside raw (other floats, e.g. a translation column, stay untouched).
void encodeInto(std::uint8_t* raw, OrientationLayout layout, const Quat& q) noexcept {
    if (isQuat(layout)) {
        const bool conj = layout == OrientationLayout::QuatXYZWConj ||
                          layout == OrientationLayout::QuatWXYZConj;
        const double s = conj ? -1.0 : 1.0;
        const bool wFirst = layout == OrientationLayout::QuatWXYZ ||
                            layout == OrientationLayout::QuatWXYZConj;
        const float out[4] = {
            static_cast<float>(wFirst ? q.w : s * q.x), static_cast<float>(wFirst ? s * q.x : s * q.y),
            static_cast<float>(wFirst ? s * q.y : s * q.z), static_cast<float>(wFirst ? s * q.z : q.w)};
        std::memcpy(raw, out, sizeof(out));
        return;
    }
    const Mat3 m = toMatrix(q);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const float value = static_cast<float>(m.m[r][c]);
            std::memcpy(raw + matrixIndex(layout, r, c) * sizeof(float), &value, sizeof(value));
        }
    }
}

float wrapTurns(float delta) noexcept { return delta - std::round(delta); }

}  // namespace

void OrientationCalibrator::setRequested(bool requested) {
    if (requested == requested_) {
        return;
    }
    requested_ = requested;
    if (requested && state_ == FeatureState::Off) {
        state_ = FeatureState::WaitingForData;
    } else if (!requested && state_ != FeatureState::Active) {
        clearSearch();
        state_ = FeatureState::Off;
        retries_ = 0;
        failReason_ = "";
    }
}

void OrientationCalibrator::reset() {
    clearSearch();
    confirmed_.clear();
    state_ = FeatureState::Off;
    requested_ = false;
    retries_ = 0;
    failReason_ = "";
}

void OrientationCalibrator::update(const OrientationInput& input, const CalibrationContext& ctx) {
    heading_ = input.heading;
    pitch_ = input.pitch;
    roll_ = input.roll;
    const bool finite = std::isfinite(heading_) && std::isfinite(pitch_) && std::isfinite(roll_);
    const bool anchored = input.anchors != nullptr && !input.anchors->empty();
    available_ = input.available && finite && anchored;
    if (!requested_ || !available_) {
        return;
    }
    switch (state_) {
        case FeatureState::WaitingForData: search(*input.anchors); break;
        case FeatureState::Calibrating:
            ++framesSinceSearch_;
            filterCandidates();
            break;
        case FeatureState::Verifying: stepVerification(ctx); break;
        case FeatureState::Active: validateConfirmed(); break;
        default: break;
    }
}

void OrientationCalibrator::search(const std::vector<PositionCandidate>& anchors) {
    clearSearch();
    std::uint8_t window[2 * kWindowBytes + kMaxLayoutBytes];
    for (const PositionCandidate& anchor : anchors) {
        const std::uintptr_t base = anchor.address - kWindowBytes;
        if (!mem::safeRead(reinterpret_cast<const void*>(base), window, sizeof(window))) {
            continue;
        }
        for (std::uintptr_t offset = 0; offset < 2 * kWindowBytes; offset += sizeof(float)) {
            const std::uintptr_t address = base + offset;
            for (const OrientationLayout layout : kLayouts) {
                const std::uintptr_t end = address + layoutBytes(layout);
                if (address < anchor.address + kPositionBytes && end > anchor.address) {
                    continue;  // that is the position itself
                }
                for (const std::int8_t ps : kSigns) {
                    bool added = false;
                    for (const std::int8_t rs : kSigns) {
                        if (matchesAt(window + offset, layout, fromEuler(heading_, pitch_, roll_, ps, rs))) {
                            candidates_.push_back({address, layout, ps, rs, 0});
                            added = true;
                            break;
                        }
                    }
                    if (added) {
                        break;
                    }
                }
            }
            if (candidates_.size() >= kMaxCandidates) {
                break;
            }
        }
    }
    log::info("orientation: %zu candidates near %zu position address(es)", candidates_.size(),
              anchors.size());
    if (candidates_.empty()) {
        retry("keine Rotation neben der Position gefunden");
        return;
    }
    lastFilterHeading_ = heading_;
    framesSinceSearch_ = 0;
    filterRounds_ = 0;
    state_ = FeatureState::Calibrating;
}

bool OrientationCalibrator::matchesTelemetry(const OrientationCandidate& candidate) const noexcept {
    std::uint8_t raw[kMaxLayoutBytes];
    if (!mem::safeRead(reinterpret_cast<const void*>(candidate.address), raw,
                       layoutBytes(candidate.layout))) {
        return false;
    }
    return matchesAt(raw, candidate.layout,
                     fromEuler(heading_, pitch_, roll_, candidate.pitchSign, candidate.rollSign));
}

void OrientationCalibrator::filterCandidates() {
    if (std::fabs(wrapTurns(heading_ - lastFilterHeading_)) >= kFilterHeadingDelta) {
        const std::size_t before = candidates_.size();
        candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(),
                                         [this](const OrientationCandidate& c) {
                                             return !matchesTelemetry(c);
                                         }),
                          candidates_.end());
        lastFilterHeading_ = heading_;
        ++filterRounds_;
        if (candidates_.size() != before) {
            log::info("orientation: filter round %u -> %zu candidates", filterRounds_,
                      candidates_.size());
        }
    }
    if (candidates_.empty()) {
        retry("alle Rotations-Kandidaten verworfen");
    } else if (readyToVerify()) {
        state_ = FeatureState::Verifying;
        verifyIndex_ = 0;
        testPending_ = false;
        confirmed_.clear();
        log::info("orientation: verifying %zu candidates", candidates_.size());
    }
}

bool OrientationCalibrator::readyToVerify() const noexcept {
    const std::size_t count = candidates_.size();
    return (filterRounds_ >= 1 && count <= kMaxVerifyCandidates) ||
           (framesSinceSearch_ >= kIdleFramesBeforeVerify && count <= kMaxIdleVerifyCandidates);
}

void OrientationCalibrator::stepVerification(const CalibrationContext& ctx) {
    if (!ctx.testWritesAllowed) {
        return;
    }
    if (testPending_) {
        ++testFrames_;
        const float turned = wrapTurns(heading_ - testBaseHeading_);
        const bool reacted = std::fabs(turned - kTestTurn) <= kTestTolerance;
        if (!reacted && testFrames_ < kVerifyFrames) {
            return;
        }
        finishTest(reacted);
    }
    while (verifyIndex_ < candidates_.size()) {
        if (startTest(candidates_[verifyIndex_])) {
            return;
        }
        ++verifyIndex_;
    }
    finishVerification();
}

bool OrientationCalibrator::startTest(const OrientationCandidate& candidate) {
    const std::size_t bytes = layoutBytes(candidate.layout);
    if (!matchesTelemetry(candidate) ||
        !mem::safeRead(reinterpret_cast<const void*>(candidate.address), testSaved_, bytes)) {
        return false;
    }
    std::memcpy(testWritten_, testSaved_, bytes);
    encodeInto(testWritten_, candidate.layout,
               fromEuler(heading_ + kTestTurn, pitch_, roll_, candidate.pitchSign, candidate.rollSign));
    if (!mem::safeWrite(reinterpret_cast<void*>(candidate.address), testWritten_, bytes)) {
        return false;
    }
    testBaseHeading_ = heading_;
    testFrames_ = 0;
    testPending_ = true;
    return true;
}

void OrientationCalibrator::finishTest(bool reacted) {
    testPending_ = false;
    if (verifyIndex_ >= candidates_.size()) {
        return;
    }
    const OrientationCandidate& candidate = candidates_[verifyIndex_];
    if (reacted) {
        confirmed_.push_back(candidate);
        log::info("orientation: confirmed 0x%llx (layout %u)",
                  static_cast<unsigned long long>(candidate.address),
                  static_cast<unsigned>(candidate.layout));
    } else {
        const std::size_t bytes = layoutBytes(candidate.layout);
        std::uint8_t now[kMaxLayoutBytes];
        if (mem::safeRead(reinterpret_cast<const void*>(candidate.address), now, bytes) &&
            std::memcmp(now, testWritten_, bytes) == 0) {
            mem::safeWrite(reinterpret_cast<void*>(candidate.address), testSaved_, bytes);
        }
    }
    ++verifyIndex_;
}

void OrientationCalibrator::finishVerification() {
    clearSearch();
    if (confirmed_.empty()) {
        retry("keine Rotation reagierte auf die Testdrehung");
        return;
    }
    state_ = FeatureState::Active;
    retries_ = 0;
    failReason_ = "";
    log::info("orientation: active with %zu address(es)", confirmed_.size());
}

void OrientationCalibrator::validateConfirmed() {
    for (OrientationCandidate& candidate : confirmed_) {
        const bool valid = matchesTelemetry(candidate);
        candidate.mismatches = valid ? 0 : static_cast<std::uint8_t>(candidate.mismatches + 1);
    }
    confirmed_.erase(std::remove_if(confirmed_.begin(), confirmed_.end(),
                                    [](const OrientationCandidate& c) {
                                        return c.mismatches >= kMaxMismatchFrames;
                                    }),
                     confirmed_.end());
    if (confirmed_.empty()) {
        state_ = FeatureState::WaitingForData;
        log::warn("orientation: calibration invalidated - searching again");
    }
}

bool OrientationCalibrator::write(float heading, float pitch, float roll) {
    if (state_ != FeatureState::Active || !std::isfinite(heading) || !std::isfinite(pitch) ||
        !std::isfinite(roll)) {
        return false;
    }
    bool written = false;
    for (OrientationCandidate& candidate : confirmed_) {
        const std::size_t bytes = layoutBytes(candidate.layout);
        std::uint8_t raw[kMaxLayoutBytes];
        if (candidate.mismatches != 0 ||
            !mem::safeRead(reinterpret_cast<const void*>(candidate.address), raw, bytes)) {
            continue;
        }
        encodeInto(raw, candidate.layout,
                   fromEuler(heading, pitch, roll, candidate.pitchSign, candidate.rollSign));
        written = mem::safeWrite(reinterpret_cast<void*>(candidate.address), raw, bytes) || written;
    }
    if (written) {
        heading_ = heading;  // validation of this frame compares against the written rotation
        pitch_ = pitch;
        roll_ = roll;
    }
    return written;
}

std::uint32_t OrientationCalibrator::candidateCount() const noexcept {
    const bool searching = state_ == FeatureState::Calibrating || state_ == FeatureState::Verifying;
    return searching ? static_cast<std::uint32_t>(candidates_.size()) : 0;
}

std::uint32_t OrientationCalibrator::confirmedCount() const noexcept {
    return static_cast<std::uint32_t>(confirmed_.size());
}

float OrientationCalibrator::progress() const noexcept {
    const float verify = candidates_.empty() ? 0.0f
                                             : static_cast<float>(verifyIndex_) /
                                                   static_cast<float>(candidates_.size());
    return calibrationProgress(state_, filterRounds_ > 0 ? 1.0f : 0.5f, verify);
}

void OrientationCalibrator::retry(const char* reason) {
    clearSearch();
    confirmed_.clear();
    ++retries_;
    if (retries_ > kMaxRetries) {
        state_ = FeatureState::Failed;
        failReason_ = reason;
        log::warn("orientation: calibration failed (%s)", reason);
        return;
    }
    state_ = FeatureState::WaitingForData;
    log::info("orientation: retry %u/%u (%s)", retries_, kMaxRetries, reason);
}

void OrientationCalibrator::clearSearch() {
    if (testPending_) {
        finishTest(false);
    }
    candidates_.clear();
    verifyIndex_ = 0;
    testPending_ = false;
    filterRounds_ = 0;
    framesSinceSearch_ = 0;
}

}  // namespace e2t
