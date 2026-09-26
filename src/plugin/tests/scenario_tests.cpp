// Fake-game scenarios for fuel / wear calibration, validation and safety gates.
#include <cstring>

#include "harness.h"
#include "test_framework.h"

namespace e2t::test {
namespace {

constexpr int kCalibrationFrames = 900;
constexpr float kFullTankSlack = 0.2f;   // litres
constexpr std::uint32_t kInsertKey = 0x2D;

std::uint32_t stateOf(FeatureState state) { return static_cast<std::uint32_t>(state); }

}  // namespace

E2T_TEST(fuel_calibration_finds_real_address_and_keeps_tank_full) {
    setUp(kDistinctiveFuel, 0.0f);
    Harness& h = harness();
    h.control.infiniteFuel = 1;
    CHECK(calibrateFuel(kCalibrationFrames));
    const auto& confirmed = h.trainer->fuelCalibrator().confirmed();
    const FakeDecoys& decoys = h.game->decoys();
    CHECK(containsAddress(confirmed, h.game->fuelAddress()));
    CHECK(!containsAddress(confirmed, addressOf(decoys.laggingFuel)));
    CHECK(!containsAddress(confirmed, addressOf(decoys.mirrorFuel)));
    CHECK(!containsAddress(confirmed, addressOf(decoys.mirrorFuelDouble)));
    CHECK(confirmed.size() == 1);

    runFrames(300);
    CHECK(h.tel.fuel >= FakeGame::kCapacity - kFullTankSlack);
    CHECK((h.status.activeFlags & kActiveFuel) != 0);
    CHECK(h.status.fuel.state == stateOf(FeatureState::Active));
    CHECK(h.status.fuel.confirmed == 1);

    issueCommand(CommandType::ResetAll);
    frame();
    CHECK(!h.trainer->fuelCalibrator().isActive());
    CHECK(h.status.lastCommandAck == h.control.commandSeq);
}

E2T_TEST(fuel_refuel_command_fills_tank) {
    setUp(kDistinctiveFuel, 0.0f);
    Harness& h = harness();
    frame();  // app connects (commands issued before the first contact are never replayed)
    issueCommand(CommandType::Refuel);  // requests the fuel calibration until done
    CHECK(calibrateFuel(kCalibrationFrames));
    frame();
    CHECK(h.tel.fuel >= FakeGame::kCapacity - kFullTankSlack);
    CHECK(messageContains("Tank aufgef"));
    CHECK(h.status.lastCommandAck == h.control.commandSeq);
}

E2T_TEST(fuel_calibration_invalidates_after_reallocation_and_recalibrates) {
    setUp(kDistinctiveFuel, 0.0f);
    Harness& h = harness();
    h.control.infiniteFuel = 1;
    CHECK(calibrateFuel(kCalibrationFrames));
    const std::uintptr_t oldAddress = h.game->fuelAddress();

    h.game->reallocateTruck(ReallocMode::ReleaseOld);  // old pages become inaccessible
    const ScalarCalibrator& fuel = h.trainer->fuelCalibrator();
    CHECK(runUntil([&fuel] { return !fuel.isActive(); }, 5));
    const bool recalibrated = runUntil(
        [&] { return fuel.isActive() && containsAddress(fuel.confirmed(), h.game->fuelAddress()); },
        kCalibrationFrames);
    CHECK(recalibrated);
    CHECK(!containsAddress(fuel.confirmed(), oldAddress));
}

E2T_TEST(fuel_stale_copy_is_invalidated_and_never_written) {
    setUp(kDistinctiveFuel, 0.0f);
    Harness& h = harness();
    h.control.infiniteFuel = 1;
    CHECK(calibrateFuel(kCalibrationFrames));
    runFrames(10);

    h.game->reallocateTruck(ReallocMode::KeepStaleCopy);  // old object stays readable
    const FakeTruckMemory* stale = h.game->staleCopy();
    CHECK(stale != nullptr);
    const float staleFuel = stale->fuel;
    const ScalarCalibrator& fuel = h.trainer->fuelCalibrator();
    CHECK(runUntil([&fuel] { return !fuel.isActive(); }, 5));
    h.game->truck().fuel = 450.4321f;  // the live object drains; a refill must hit only it
    runFrames(10);
    CHECK(stale->fuel == staleFuel);
}

E2T_TEST(no_damage_holds_wear_and_repair_resets_it) {
    constexpr float kInitialWear = 0.0312345f;
    constexpr float kWearSlack = 1.0e-6f;
    setUp(kDistinctiveFuel, 0.0f, kInitialWear);
    Harness& h = harness();
    h.control.noDamage = 1;
    h.game->setInput(DriveInput{true, 0.3f, 0.0f, 0.0f});  // wear grows while driving
    const ScalarCalibrator& wear = h.trainer->wearCalibrator(WearChannel::Engine);
    CHECK(runUntil([&wear] { return wear.isActive(); }, kCalibrationFrames));
    CHECK(containsAddress(wear.confirmed(), h.game->wearAddress()));
    const float ceiling = wear.value();

    runFrames(120);
    CHECK(h.tel.wearEngine <= ceiling + kWearSlack);
    CHECK((h.status.activeFlags & kActiveNoDamage) != 0);
    CHECK(h.status.wear.state == stateOf(FeatureState::Active) && h.status.wear.confirmed == 1);

    issueCommand(CommandType::Repair);
    runFrames(3);
    CHECK(h.tel.wearEngine < 1.0e-5f);
    CHECK(messageContains("Reparatur"));
}

E2T_TEST(truckersmp_blocks_all_features) {
    setUp(kDistinctiveFuel, 0.0f);
    Harness& h = harness();
    h.platform.setTruckersMp(true);
    h.control.infiniteFuel = 1;
    h.control.powerBoost = 1;
    h.game->setInput(DriveInput{true, 0.5f, 0.0f, 0.0f});
    runFrames(300);
    CHECK(h.trainer->truckersMpDetected());
    CHECK(!h.trainer->fuelCalibrator().isActive());
    CHECK(h.status.fuel.state == stateOf(FeatureState::Blocked));
    CHECK(h.status.velocity.state == stateOf(FeatureState::Blocked));
    CHECK(h.status.activeFlags == 0);
    CHECK(messageContains("TruckersMP"));
    CHECK(h.tel.fuel < kDistinctiveFuel);  // never refilled
}

E2T_TEST(stale_app_heartbeat_blocks_writes) {
    constexpr int kStaleFrames = 200;  // 3.2 s > 3 s timeout
    setUp(kDistinctiveFuel, 0.0f);
    Harness& h = harness();
    h.control.infiniteFuel = 1;
    CHECK(calibrateFuel(kCalibrationFrames));
    runFrames(5);

    h.heartbeatRunning = false;
    runFrames(kStaleFrames);
    CHECK(h.status.fuel.state == stateOf(FeatureState::Blocked));
    CHECK(messageContains("Trainer-App"));
    h.game->truck().fuel = 400.1234f;  // game event while blocked
    runFrames(10);
    CHECK(h.tel.fuel < 401.0f);

    h.heartbeatRunning = true;
    runFrames(5);
    CHECK(h.tel.fuel >= FakeGame::kCapacity - kFullTankSlack);
}

E2T_TEST(stale_command_is_not_replayed_on_connect) {
    setUp(kDistinctiveFuel, 10.0f);
    Harness& h = harness();
    h.control.commandSeq = 7;  // left over from a previous game session
    h.control.commandType = static_cast<std::uint32_t>(CommandType::Teleport);
    runFrames(5);
    CHECK(h.status.lastCommandAck == 7);
    CHECK(h.status.position.state == stateOf(FeatureState::Off));
}

E2T_TEST(menu_hotkey_counts_focused_presses) {
    setUp(kDistinctiveFuel, 0.0f);
    Harness& h = harness();
    h.control.menuHotkeyVk = kInsertKey;
    frame();
    h.platform.setKey(kInsertKey, true);
    runFrames(3);
    h.platform.setKey(kInsertKey, false);
    frame();
    CHECK(h.status.menuToggleCount == 1);
    h.platform.setFocus(false);
    h.platform.setKey(kInsertKey, true);
    runFrames(2);
    CHECK(h.status.menuToggleCount == 1);
}

}  // namespace e2t::test
