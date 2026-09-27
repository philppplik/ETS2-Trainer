#include "async_scan.h"

#include <windows.h>

#include <algorithm>
#include <utility>

namespace e2t::mem {

AsyncScan::~AsyncScan() { cancel(); }

bool AsyncScan::start(std::vector<ScanTarget> targets, std::unique_ptr<LiveBounds[]> bounds,
                      unsigned workers) {
    if (running() || targets.empty()) {
        return false;
    }
    targets_ = std::move(targets);
    bounds_ = std::move(bounds);
    workers_ = std::max(1u, workers);
    failed_ = false;
    done_.store(false, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        reset();
        return false;
    }
    return true;
}

void AsyncScan::run() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        results_ = scanProcess(targets_, &stats_, workers_, &cancel_);
    } catch (...) {
        failed_ = true;
        results_.clear();
    }
    done_.store(true, std::memory_order_release);
}

std::vector<ScanResult> AsyncScan::take(ScanStats& stats) {
    if (thread_.joinable()) {
        thread_.join();
    }
    stats = stats_;
    std::vector<ScanResult> results = failed_ ? std::vector<ScanResult>() : std::move(results_);
    reset();
    return results;
}

void AsyncScan::cancel() noexcept {
    cancel_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    reset();
}

void AsyncScan::reset() noexcept {
    targets_.clear();
    results_.clear();
    bounds_.reset();
    stats_ = ScanStats{};
    done_.store(false, std::memory_order_relaxed);
}

unsigned backgroundWorkerCount() noexcept { return std::max(1u, workerCount() / 2); }

}  // namespace e2t::mem
