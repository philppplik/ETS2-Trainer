// OS services the trainer depends on (real implementation: platform_win, fake: self-test).
#pragma once

#include <cstdint>

namespace e2t {

class Platform {
public:
    Platform() = default;
    virtual ~Platform() = default;
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    virtual std::uint64_t nowMs() const noexcept = 0;        // monotonic wall clock
    virtual bool gameHasFocus() const noexcept = 0;          // our process owns the foreground
    virtual bool isKeyDown(std::uint32_t vk) const noexcept = 0;
    virtual bool truckersMpLoaded() const noexcept = 0;      // TruckersMP client module present
};

class WinPlatform final : public Platform {
public:
    std::uint64_t nowMs() const noexcept override;
    bool gameHasFocus() const noexcept override;
    bool isKeyDown(std::uint32_t vk) const noexcept override;
    bool truckersMpLoaded() const noexcept override;
};

}  // namespace e2t
