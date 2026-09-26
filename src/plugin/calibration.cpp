#include "calibration.h"

#include <algorithm>

namespace e2t {
namespace {

constexpr float kCalibratingBase = 0.1f;
constexpr float kCalibratingSpan = 0.6f;
constexpr float kVerifyingBase = 0.7f;
constexpr float kVerifyingSpan = 0.3f;

float clamp01(float value) noexcept { return std::min(1.0f, std::max(0.0f, value)); }

}  // namespace

float calibrationProgress(FeatureState state, float searchFraction, float verifyFraction) noexcept {
    switch (state) {
        case FeatureState::Active: return 1.0f;
        case FeatureState::Calibrating:
            return kCalibratingBase + kCalibratingSpan * clamp01(searchFraction);
        case FeatureState::Verifying:
            return kVerifyingBase + kVerifyingSpan * clamp01(verifyFraction);
        default: return 0.0f;
    }
}

const char* featureStateName(FeatureState state) noexcept {
    switch (state) {
        case FeatureState::Off: return "off";
        case FeatureState::WaitingForData: return "waiting";
        case FeatureState::Calibrating: return "calibrating";
        case FeatureState::Verifying: return "verifying";
        case FeatureState::Active: return "active";
        case FeatureState::Failed: return "failed";
        case FeatureState::Blocked: return "blocked";
    }
    return "?";
}

}  // namespace e2t
