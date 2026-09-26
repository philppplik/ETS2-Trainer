#include <windows.h>

#include "platform.h"

namespace e2t {
namespace {

constexpr SHORT kKeyDownMask = static_cast<SHORT>(0x8000);
constexpr std::uint32_t kMinVirtualKey = 0x01;
constexpr std::uint32_t kMaxVirtualKey = 0xFE;
constexpr const wchar_t* kTruckersMpModules[] = {L"core_ets2mp.dll", L"core_atsmp.dll"};

}  // namespace

std::uint64_t WinPlatform::nowMs() const noexcept { return GetTickCount64(); }

bool WinPlatform::gameHasFocus() const noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

bool WinPlatform::isKeyDown(std::uint32_t vk) const noexcept {
    if (vk < kMinVirtualKey || vk > kMaxVirtualKey) {
        return false;
    }
    return (GetAsyncKeyState(static_cast<int>(vk)) & kKeyDownMask) != 0;
}

bool WinPlatform::truckersMpLoaded() const noexcept {
    for (const wchar_t* module : kTruckersMpModules) {
        if (GetModuleHandleW(module) != nullptr) {
            return true;
        }
    }
    return false;
}

}  // namespace e2t
