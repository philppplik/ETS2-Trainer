#include "bridge.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstring>

#include "log.h"

namespace e2t {
namespace {

constexpr int kControlReadAttempts = 4;

volatile LONG* asInterlocked(std::uint32_t& field) noexcept {
    return reinterpret_cast<volatile LONG*>(&field);
}

std::uint32_t volatileRead(const std::uint32_t& field) noexcept {
    return *reinterpret_cast<const volatile std::uint32_t*>(&field);
}

}  // namespace

Bridge::~Bridge() { close(); }

bool Bridge::open() noexcept {
    close();
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(sizeof(Shared)), kBridgeMappingName);
    if (mapping == nullptr) {
        log::error("bridge: CreateFileMappingW failed (%lu)", GetLastError());
        return false;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (view == nullptr) {
        log::error("bridge: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(mapping);
        return false;
    }
    mapping_ = mapping;
    view_ = static_cast<Shared*>(view);
    telemetrySeq_ = 0;

    std::memset(view_, 0, sizeof(Shared));
    view_->version = kBridgeVersion;
    view_->size = static_cast<std::uint32_t>(sizeof(Shared));
    std::atomic_thread_fence(std::memory_order_release);
    InterlockedExchange(asInterlocked(view_->magic), static_cast<LONG>(kBridgeMagic));
    log::info("bridge: shared memory ready (%zu bytes, %s)", sizeof(Shared),
              existed ? "opened existing mapping" : "created");
    return true;
}

void Bridge::close() noexcept {
    if (view_ != nullptr) {
        InterlockedExchange(asInterlocked(view_->magic), 0);  // tell the app we are gone
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
}

void Bridge::publishTelemetry(const Telemetry& telemetry) noexcept {
    if (view_ == nullptr) {
        return;
    }
    constexpr std::size_t kBodyBegin = offsetof(Telemetry, frameCounter);
    constexpr std::size_t kBodySize = offsetof(Telemetry, seqEnd) - kBodyBegin;
    Telemetry& shared = view_->telemetry;
    const std::uint32_t seq = ++telemetrySeq_;

    InterlockedExchange(asInterlocked(shared.seqBegin), static_cast<LONG>(seq));  // full fence
    std::memcpy(reinterpret_cast<std::uint8_t*>(&shared) + kBodyBegin,
                reinterpret_cast<const std::uint8_t*>(&telemetry) + kBodyBegin, kBodySize);
    std::atomic_thread_fence(std::memory_order_release);
    InterlockedExchange(asInterlocked(shared.seqEnd), static_cast<LONG>(seq));
}

void Bridge::publishStatus(const Status& status) noexcept {
    if (view_ == nullptr) {
        return;
    }
    constexpr std::size_t kBodyBegin = offsetof(Status, fuel);
    Status& shared = view_->status;
    shared.pluginBuild = status.pluginBuild;
    std::memcpy(reinterpret_cast<std::uint8_t*>(&shared) + kBodyBegin,
                reinterpret_cast<const std::uint8_t*>(&status) + kBodyBegin,
                sizeof(Status) - kBodyBegin);
    std::atomic_thread_fence(std::memory_order_release);
    InterlockedExchange(asInterlocked(shared.heartbeat), static_cast<LONG>(status.heartbeat));
}

Control Bridge::readControl() const noexcept {
    Control copy{};
    if (view_ == nullptr) {
        return copy;
    }
    const Control& shared = view_->control;
    for (int attempt = 0; attempt < kControlReadAttempts; ++attempt) {
        const std::uint32_t seqBefore = volatileRead(shared.commandSeq);
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(&copy, &shared, sizeof(Control));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (volatileRead(shared.commandSeq) == seqBefore && copy.commandSeq == seqBefore) {
            break;
        }
    }
    return copy;
}

}  // namespace e2t
