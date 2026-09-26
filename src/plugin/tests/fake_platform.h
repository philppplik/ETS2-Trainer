// Deterministic Platform for the self-test.
#pragma once

#include <array>
#include <cstdint>

#include "../platform.h"

namespace e2t::test {

class FakePlatform final : public Platform {
public:
    static constexpr std::uint64_t kStartMs = 100'000;

    void reset() noexcept {
        nowMs_ = kStartMs;
        focus_ = true;
        truckersMp_ = false;
        keys_.fill(false);
    }
    void advance(std::uint64_t ms) noexcept { nowMs_ += ms; }
    void setFocus(bool focus) noexcept { focus_ = focus; }
    void setKey(std::uint32_t vk, bool down) noexcept {
        if (vk < keys_.size()) {
            keys_[vk] = down;
        }
    }
    void setTruckersMp(bool loaded) noexcept { truckersMp_ = loaded; }

    std::uint64_t nowMs() const noexcept override { return nowMs_; }
    bool gameHasFocus() const noexcept override { return focus_; }
    bool isKeyDown(std::uint32_t vk) const noexcept override {
        return vk < keys_.size() && keys_[vk];
    }
    bool truckersMpLoaded() const noexcept override { return truckersMp_; }

private:
    std::uint64_t nowMs_ = kStartMs;
    bool focus_ = true;
    bool truckersMp_ = false;
    std::array<bool, 256> keys_{};
};

}  // namespace e2t::test
