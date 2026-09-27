// Fake-game scenarios for the velocity calibrator (boost, nitro, speed cap, stop) and the
// experimental position calibration + teleport.
#include <cmath>

#include "harness.h"
#include "test_framework.h"

namespace e2t::test {
namespace {

constexpr int kCalibrationFrames = 1500;
constexpr float kCruiseSpeed = 15.0f;  // m/s
constexpr std::uint32_t kShiftKey = 0x10;

double distanceTo(const double (&a)[3], const double (&b)[3]) {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void coast() { harness().game->setInput(DriveInput{true, 0.0f, 0.0f, 0.0f}); }

std::size_t bytesOf(const VectorCandidate& candidate) {
    return candidate.encoding == VectorEncoding::Float32x3 ? 12 : 24;
}

bool noOverlaps(const std::vector<VectorCandidate>& vectors) {
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        const mem::Range range{vectors[i].address, vectors[i].address + bytesOf(vectors[i])};
        for (std::size_t j = i + 1; j < vectors.size(); ++j) {
            if (range.overlaps(vectors[j].address, bytesOf(vectors[j]))) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

E2T_TEST(velocity_calibration_converges_and_boost_increases_speed) {
    constexpr float kPowerFactor = 3.0f;
    constexpr int kMeasureFrames = 60;
    setUp(kDistinctiveFuel, kCruiseSpeed);
    Harness& h = harness();
    h.control.powerBoost = 1;
    h.control.powerFactor = kPowerFactor;
    CHECK(calibrateVelocity(kCalibrationFrames));
    const VelocityCalibrator& velocity = h.trainer->velocityCalibrator();
    CHECK(containsAddress(velocity.confirmed(), h.game->velocityAddress()));
    CHECK(velocity.confirmed().size() <= 16);
    CHECK(noOverlaps(velocity.confirmed()));  // overlapping "combo" triples must be skipped

    h.game->setInput(DriveInput{true, 1.0f, 0.0f, 0.0f});
    frame();
    const float start = h.game->speed();
    float natural = 0.0f;
    for (int i = 0; i < kMeasureFrames; ++i) {
        frame();
        natural += h.game->lastNaturalAccel() * FakeGame::kDt;
    }
    const float gained = h.game->speed() - start;
    CHECK(natural > 0.5f);
    CHECK(gained > 2.5f * natural);  // factor 3: natural + 2x boost
    CHECK((h.status.activeFlags & kActiveBoost) != 0);
    CHECK(h.status.velocity.state == static_cast<std::uint32_t>(FeatureState::Active));
}

E2T_TEST(velocity_speed_cap_and_stop_truck) {
    constexpr float kCapKmh = 36.0f;  // 10 m/s
    constexpr float kCapSlack = 0.1f;
    setUp(kDistinctiveFuel, kCruiseSpeed);
    Harness& h = harness();
    h.control.speedCapEnabled = 1;
    h.control.speedCapKmh = 200.0f;  // no effect while calibrating
    CHECK(calibrateVelocity(kCalibrationFrames));

    h.control.speedCapKmh = kCapKmh;
    h.game->setInput(DriveInput{true, 1.0f, 0.0f, 0.0f});
    runFrames(5);
    CHECK(h.tel.speed <= kCapKmh / 3.6f + kCapSlack);
    CHECK((h.status.activeFlags & kActiveSpeedCap) != 0);

    coast();
    issueCommand(CommandType::StopTruck);
    runFrames(2);
    CHECK(h.game->speed() < 0.5f);
    CHECK(messageContains("gestoppt"));
}

E2T_TEST(nitro_applies_only_while_key_held_and_focused) {
    constexpr float kNitroAccel = 10.0f;
    constexpr int kWindow = 30;  // 0.5 s -> ~5 m/s with nitro
    setUp(kDistinctiveFuel, kCruiseSpeed);
    Harness& h = harness();
    h.control.nitroEnabled = 1;
    h.control.nitroVk = kShiftKey;
    h.control.nitroAccel = kNitroAccel;
    CHECK(calibrateVelocity(kCalibrationFrames));
    coast();
    runFrames(2);

    float before = h.game->speed();
    runFrames(kWindow);
    CHECK(h.game->speed() <= before + 0.01f);

    h.platform.setKey(kShiftKey, true);
    before = h.game->speed();
    runFrames(kWindow);
    CHECK(h.game->speed() - before > 0.8f * kNitroAccel * kWindow * FakeGame::kDt);
    CHECK((h.status.activeFlags & kActiveNitro) != 0);

    h.platform.setFocus(false);
    before = h.game->speed();
    runFrames(kWindow);
    CHECK(h.game->speed() <= before + 0.01f);
}

E2T_TEST(velocity_calibration_invalidates_after_reallocation) {
    constexpr float kInactiveCapKmh = 200.0f;
    constexpr float kCapKmh = 36.0f;
    setUp(kDistinctiveFuel, kCruiseSpeed);
    Harness& h = harness();
    h.control.speedCapEnabled = 1;
    h.control.speedCapKmh = kInactiveCapKmh;
    CHECK(calibrateVelocity(kCalibrationFrames));
    const VelocityCalibrator& velocity = h.trainer->velocityCalibrator();
    const std::uintptr_t oldAddress = h.game->velocityAddress();
    CHECK(containsAddress(velocity.confirmed(), oldAddress));

    // The body vector moves. The mirror/lagging copies still track the speed, so only the
    // write-effect monitor can notice that the cap no longer acts on the truck.
    h.game->reallocateTruck(ReallocMode::ReleaseOld);
    h.control.speedCapKmh = kCapKmh;
    h.game->setInput(DriveInput{true, 0.5f, 0.0f, 0.0f});
    CHECK(runUntil([&velocity] { return !velocity.isActive(); }, 30));

    h.control.speedCapKmh = kInactiveCapKmh;
    CHECK(calibrateVelocity(kCalibrationFrames));
    CHECK(containsAddress(velocity.confirmed(), h.game->velocityAddress()));
    CHECK(!containsAddress(velocity.confirmed(), oldAddress));
}

E2T_TEST(position_calibration_and_teleport) {
    constexpr double kGroundY = 12.5;
    constexpr double kArrivalRadius = 2.0;
    setUp(kDistinctiveFuel, 10.0f);
    Harness& h = harness();
    h.game->setInput(DriveInput{true, 0.2f, 0.0f, 0.0f});
    runFrames(2);
    const double target[3] = {h.tel.pos[0] + 5000.0, kGroundY, h.tel.pos[2] - 3000.0};
    issueCommand(CommandType::Teleport, target[0], target[1], target[2]);
    const bool arrived = runUntil(
        [&] { return distanceTo(h.game->truck().position, target) < kArrivalRadius; }, 900);
    CHECK(arrived);
    const PositionCalibrator& position = h.trainer->positionCalibrator();
    CHECK(position.isActive());
    CHECK(containsAddress(position.confirmed(), h.game->positionAddress()));
    CHECK(!containsAddress(position.confirmed(), addressOf(h.game->decoys().mirrorPosition[0])));
    CHECK(messageContains("Teleport ausgef"));

    // The calibration is kept (request cleared, status still active): a second teleport is
    // executed within a couple of frames, without a new scan.
    runFrames(2);
    CHECK(h.status.position.state == static_cast<std::uint32_t>(FeatureState::Active));
    const double second[3] = {target[0] - 800.0, kGroundY, target[2] + 400.0};
    issueCommand(CommandType::Teleport, second[0], second[1], second[2]);
    CHECK(runUntil(
        [&] { return distanceTo(h.game->truck().position, second) < kArrivalRadius; }, 3));
}

// Teleporting with a trailer is allowed now (the trailer simply stays behind); the command must
// be accepted and start the position calibration.
E2T_TEST(teleport_with_trailer_attached_is_accepted) {
    setUp(kDistinctiveFuel, 5.0f);
    Harness& h = harness();
    h.game->setTrailer(true);
    runFrames(2);
    issueCommand(CommandType::Teleport, 1.0, 2.0, 3.0);
    frame();
    CHECK(h.status.lastCommandAck == h.control.commandSeq);
    CHECK(h.status.position.state != static_cast<std::uint32_t>(FeatureState::Off));
}

}  // namespace e2t::test
