// Shared-memory bridge to the trainer app (layout: shared/bridge_protocol.h).
#pragma once

#include <cstdint>

#include "../../shared/bridge_protocol.h"

namespace e2t {

class Bridge {
public:
    Bridge() = default;
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Creates (or opens) the named mapping, zeroes it, fills the header and publishes the magic
    // last. Returns false (and logs) on failure; the plugin keeps running without the app link.
    bool open() noexcept;
    void close() noexcept;
    bool isOpen() const noexcept { return view_ != nullptr; }

    // Seqlock write: seqBegin, fence, body, fence, seqEnd = seqBegin.
    void publishTelemetry(const Telemetry& telemetry) noexcept;
    // Body first, heartbeat last.
    void publishStatus(const Status& status) noexcept;
    // Consistent copy of the app-owned Control block (retries while commandSeq moves).
    Control readControl() const noexcept;

private:
    void* mapping_ = nullptr;
    Shared* view_ = nullptr;
    std::uint32_t telemetrySeq_ = 0;
};

}  // namespace e2t
