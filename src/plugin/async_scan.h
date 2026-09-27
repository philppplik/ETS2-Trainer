// Background memory scan: runs mem::scanProcess on its own thread so the game keeps running.
//
// The 14 GiB address space of ETS2 takes several seconds to scan; doing that inside frame_end
// froze the game. Values that change meanwhile (speed, fuel, position) are handled with
// LiveBounds: the owner widens them every frame until done() reports completion, and the
// calibrators filter the hits against the current telemetry right afterwards.
#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "mem_scan.h"

namespace e2t::mem {

class AsyncScan {
public:
    AsyncScan() = default;
    ~AsyncScan();
    AsyncScan(const AsyncScan&) = delete;
    AsyncScan& operator=(const AsyncScan&) = delete;

    // Starts a scan. `bounds` holds one LiveBounds per target and must already be referenced by
    // targets[i].live where wanted; it stays alive (owned here) until take()/cancel().
    // Returns false if a scan is still running or the thread could not be created.
    bool start(std::vector<ScanTarget> targets, std::unique_ptr<LiveBounds[]> bounds,
               unsigned workers);

    bool running() const noexcept { return thread_.joinable(); }
    bool done() const noexcept { return done_.load(std::memory_order_acquire); }
    LiveBounds* bounds() noexcept { return bounds_.get(); }

    // Joins the finished scan and hands out its results (empty when it failed or was cancelled).
    std::vector<ScanResult> take(ScanStats& stats);

    // Requests a stop, joins and discards everything. Safe to call when idle.
    void cancel() noexcept;

private:
    void run() noexcept;
    void reset() noexcept;

    std::vector<ScanTarget> targets_;
    std::unique_ptr<LiveBounds[]> bounds_;
    std::vector<ScanResult> results_;
    ScanStats stats_{};
    unsigned workers_ = 0;
    bool failed_ = false;
    std::atomic<bool> done_{false};
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};

// Worker threads for background scans: half the cores (at least one) so the game keeps its CPU.
unsigned backgroundWorkerCount() noexcept;

}  // namespace e2t::mem
