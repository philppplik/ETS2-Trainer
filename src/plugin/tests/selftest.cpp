// Offline self-test runner (no game needed): unit tests + fake-game scenarios.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "../log.h"
#include "harness.h"
#include "test_framework.h"

namespace e2t::test {
namespace {

int g_failedChecks = 0;
Harness g_harness;

std::wstring logPathNextToExecutable() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::wstring();
    }
    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L"\\/");
    return result.substr(0, slash + 1) + L"selftest.log";
}

}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void reportCheck(bool ok, const char* expression, const char* file, int line) {
    if (ok) {
        return;
    }
    ++g_failedChecks;
    const char* name = std::strrchr(file, '\\');
    std::printf("    check failed: %s (%s:%d)\n", expression, name != nullptr ? name + 1 : file,
                line);
}

int failedChecksInCurrentTest() { return g_failedChecks; }

Harness& harness() { return g_harness; }

void setUp(float fuel, float speed, float wear) {
    Harness& h = g_harness;
    h.trainer.reset();
    h.game.reset();
    h.platform.reset();
    h.game.emplace();
    h.game->reset(fuel, speed, wear);
    h.trainer.emplace(h.platform);
    h.tel = TelemetryState{};
    h.control = Control{};
    h.status = Status{};
    h.heartbeatRunning = true;
}

void tearDown() {
    g_harness.trainer.reset();
    g_harness.game.reset();
}

void frame() {
    Harness& h = g_harness;
    h.game->step(h.tel);
    if (h.heartbeatRunning) {
        ++h.control.appHeartbeat;
    }
    h.platform.advance(kFrameMs);
    h.trainer->onFrameEnd(h.tel, h.control, h.status);
}

void runFrames(int count) {
    for (int i = 0; i < count; ++i) {
        frame();
    }
}

void issueCommand(CommandType type, double a0, double a1, double a2) {
    Control& control = g_harness.control;
    control.commandType = static_cast<std::uint32_t>(type);
    control.commandArgs[0] = a0;
    control.commandArgs[1] = a1;
    control.commandArgs[2] = a2;
    ++control.commandSeq;
}

bool messageContains(const char* text) {
    return std::strstr(g_harness.status.message, text) != nullptr;
}

bool calibrateFuel(int maxFrames) {
    constexpr int kEngineOffFrames = 5;
    Harness& h = g_harness;
    DriveInput input = h.game->input();
    input.engineOn = false;
    h.game->setInput(input);
    runFrames(kEngineOffFrames);
    input.engineOn = true;
    h.game->setInput(input);
    return runUntil([&h] { return h.trainer->fuelCalibrator().isActive(); }, maxFrames);
}

bool calibrateVelocity(int maxFrames) {
    constexpr int kPhaseFrames = 60;
    constexpr float kBrake = 0.3f;
    Harness& h = g_harness;
    for (int i = 0; i < maxFrames; ++i) {
        const bool accelerate = (i / kPhaseFrames) % 2 == 0;
        DriveInput input;
        input.engineOn = true;
        input.throttle = accelerate ? 1.0f : 0.0f;
        input.brake = accelerate ? 0.0f : kBrake;
        h.game->setInput(input);
        frame();
        if (h.trainer->velocityCalibrator().isActive()) {
            return true;
        }
    }
    return false;
}

}  // namespace e2t::test

int main() {
    using namespace e2t::test;
    e2t::log::init(nullptr, logPathNextToExecutable());
    int failedTests = 0;
    for (const TestCase& test : registry()) {
        const int before = g_failedChecks;
        try {
            test.function();
        } catch (const std::exception& ex) {
            std::printf("    exception: %s\n", ex.what());
            ++g_failedChecks;
        } catch (...) {
            std::printf("    unknown exception\n");
            ++g_failedChecks;
        }
        tearDown();
        const bool passed = g_failedChecks == before;
        failedTests += passed ? 0 : 1;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", test.name);
        std::fflush(stdout);
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failedTests);
    e2t::log::shutdown();
    return failedTests == 0 ? 0 : 1;
}
