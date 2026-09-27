// Status block for the app and the German status line.
#include <algorithm>
#include <cstdio>
#include <string>

#include "cloud_storage.h"
#include "input_device.h"
#include "text_util.h"
#include "trainer.h"

namespace e2t {
namespace {

constexpr std::size_t kHintBytes = 256;
constexpr std::size_t kListBytes = 192;

constexpr char kFuelWaiting[] =
    u8"Sprit: Kalibrierung – fahr kurz los, bis sich der Tankinhalt ändert";
constexpr char kWearWaiting[] =
    u8"Kein Schaden: warte auf Verschleiß > 0 – etwas fahren (leichter Schaden hilft)";
constexpr char kVelocityWaiting[] =
    u8"Boost: zum Kalibrieren geradeaus schneller als 20 km/h fahren";

int stateRank(FeatureState state) noexcept {
    switch (state) {
        case FeatureState::Active: return 5;
        case FeatureState::Verifying: return 4;
        case FeatureState::Calibrating: return 3;
        case FeatureState::WaitingForData: return 2;
        case FeatureState::Failed: return 1;
        default: return 0;
    }
}

bool motionWanted(const Control& control) noexcept {
    constexpr std::uint32_t kPhysicsFun = kFunMoonGravity | kFunAnchor | kFunSpin | kFunAutoUpright;
    return control.prepareMotion != 0 || (control.funFlags & kPhysicsFun) != 0;
}

bool velocityWanted(const Control& control) noexcept {
    return control.powerBoost != 0 || control.nitroEnabled != 0 || control.speedCapEnabled != 0 ||
           motionWanted(control);
}

template <class Calibrator>
FeatureStatus featureStatus(const Calibrator& calibrator, bool wanted, bool blocked) noexcept {
    FeatureStatus status{};
    if (!wanted) {
        status.state = static_cast<std::uint32_t>(FeatureState::Off);
        return status;
    }
    if (blocked) {
        status.state = static_cast<std::uint32_t>(FeatureState::Blocked);
        return status;
    }
    const FeatureState state = calibrator.isActive() ? FeatureState::Active : calibrator.state();
    status.state = static_cast<std::uint32_t>(state == FeatureState::Off
                                                  ? FeatureState::WaitingForData
                                                  : state);
    status.candidates = calibrator.candidateCount();
    status.confirmed = calibrator.isActive() ? calibrator.confirmedCount() : 0;
    status.progress = calibrator.progress();
    return status;
}

void scalarHint(const char* label, const ScalarCalibrator& calibrator, const char* waitingText,
                char* out, std::size_t size) {
    switch (calibrator.state()) {
        case FeatureState::WaitingForData: std::snprintf(out, size, "%s", waitingText); break;
        case FeatureState::Calibrating:
            std::snprintf(out, size, u8"%s: Kalibrierung läuft (%u Kandidaten) – weiterfahren",
                          label, calibrator.candidateCount());
            break;
        case FeatureState::Verifying:
            std::snprintf(out, size, u8"%s: prüfe Speicheradressen (%u übrig)", label,
                          calibrator.candidateCount());
            break;
        case FeatureState::Failed:
            std::snprintf(out, size,
                          u8"%s: Kalibrierung fehlgeschlagen (%s) – bitte neu kalibrieren",
                          label, calibrator.failReason());
            break;
        default: out[0] = '\0'; break;
    }
}

void velocityHint(const VelocityCalibrator& calibrator, char* out, std::size_t size) {
    switch (calibrator.state()) {
        case FeatureState::WaitingForData: std::snprintf(out, size, "%s", kVelocityWaiting); break;
        case FeatureState::Calibrating:
            std::snprintf(out, size,
                          u8"Boost: Kalibrierung – leicht beschleunigen und bremsen "
                          u8"(%u Kandidaten)",
                          calibrator.candidateCount());
            break;
        case FeatureState::Verifying:
            std::snprintf(out, size, "%s", u8"Boost: teste Geschwindigkeitsadresse …");
            break;
        case FeatureState::Failed:
            std::snprintf(out, size, u8"Boost: Kalibrierung fehlgeschlagen (%s) – neu kalibrieren",
                          calibrator.failReason());
            break;
        default: out[0] = '\0'; break;
    }
}

void positionHint(const PositionCalibrator& calibrator, char* out, std::size_t size) {
    switch (calibrator.state()) {
        case FeatureState::WaitingForData:
            std::snprintf(out, size, "%s", u8"Teleport: Positions-Kalibrierung startet …");
            break;
        case FeatureState::Calibrating:
            std::snprintf(out, size,
                          u8"Teleport: ein Stück fahren, um Kandidaten einzugrenzen (%u)",
                          calibrator.candidateCount());
            break;
        case FeatureState::Verifying:
            std::snprintf(out, size, u8"Teleport: prüfe Positionsadressen (%u übrig)",
                          calibrator.candidateCount());
            break;
        case FeatureState::Failed:
            std::snprintf(out, size, u8"Teleport: Positions-Kalibrierung fehlgeschlagen (%s)",
                          calibrator.failReason());
            break;
        default: out[0] = '\0'; break;
    }
}

void appendItem(char* out, std::size_t size, bool& first, const char* item) {
    const std::size_t used = std::char_traits<char>::length(out);
    if (used + 1 >= size) {
        return;
    }
    std::snprintf(out + used, size - used, "%s%s", first ? "" : ", ", item);
    first = false;
}

}  // namespace

void Trainer::fillStatus(const Control& control, Gate gate, std::uint64_t now,
                         Status& status) const {
    const bool blocked = gate != Gate::Open;
    status.pluginBuild = kPluginBuild;
    status.heartbeat = heartbeat_;
    status.fuel = featureStatus(fuel_, control.infiniteFuel != 0 || pendingRefuel_, blocked);
    status.wear = wearStatus(control, gate);
    status.velocity = featureStatus(velocity_, velocityWanted(control), blocked);
    status.position = featureStatus(position_, positionRequested_ || motionWanted(control) ||
                                                   position_.isActive(),
                                    blocked);
    status.lastCommandAck = lastCommandSeq_;
    status.activeFlags = activeFlags_;
    status.menuToggleCount = menuToggleCount_;
    status.reserved0 = 0;
    composeMessage(control, gate, now, status.message, sizeof(status.message));
}

FeatureStatus Trainer::wearStatus(const Control& control, Gate gate) const noexcept {
    FeatureStatus status{};
    if (control.noDamage == 0) {
        status.state = static_cast<std::uint32_t>(FeatureState::Off);
        return status;
    }
    if (gate != Gate::Open) {
        status.state = static_cast<std::uint32_t>(FeatureState::Blocked);
        return status;
    }
    FeatureState best = FeatureState::Off;
    float bestProgress = 0.0f;
    std::uint32_t active = 0;
    for (const ScalarCalibrator& wear : wear_) {
        const FeatureState state = wear.state();
        best = stateRank(state) > stateRank(best) ? state : best;
        bestProgress = std::max(bestProgress, wear.progress());
        status.candidates += wear.candidateCount();
        active += wear.isActive() ? 1u : 0u;
    }
    status.state = static_cast<std::uint32_t>(best == FeatureState::Off
                                                  ? FeatureState::WaitingForData
                                                  : best);
    status.confirmed = active;
    status.progress = active > 0 ? static_cast<float>(active) / kWearChannelCount : bestProgress;
    return status;
}

void Trainer::composeMessage(const Control& control, Gate gate, std::uint64_t now, char* out,
                             std::size_t size) const {
    switch (gate) {
        case Gate::TruckersMp:
            copyUtf8(out, size, u8"Blockiert: TruckersMP erkannt – Trainer deaktiviert");
            return;
        case Gate::AppStale:
            copyUtf8(out, size, u8"Blockiert: keine Verbindung zur Trainer-App");
            return;
        case Gate::NoTruck:
            copyUtf8(out, size, u8"Blockiert: kein LKW geladen – Profil laden und einsteigen");
            return;
        case Gate::Open: break;
    }
    if (commandMessage_[0] != '\0' && now < commandMessageUntilMs_) {
        copyUtf8(out, size, commandMessage_);
        return;
    }
    char hint[kHintBytes] = {};
    if (composeHint(control, hint, sizeof(hint))) {
        copyUtf8(out, size, hint);
        return;
    }
    composeActiveList(out, size);
}

bool Trainer::composeHint(const Control& control, char* out, std::size_t size) const {
    out[0] = '\0';
    if ((control.infiniteFuel != 0 || pendingRefuel_) && !fuel_.isActive()) {
        scalarHint(u8"Sprit", fuel_, kFuelWaiting, out, size);
    }
    if (out[0] == '\0' && control.noDamage != 0) {
        const ScalarCalibrator* best = &wear_[0];
        for (const ScalarCalibrator& wear : wear_) {
            best = stateRank(wear.state()) > stateRank(best->state()) ? &wear : best;
        }
        if (!best->isActive()) {
            scalarHint(u8"Kein Schaden", *best, kWearWaiting, out, size);
        }
    }
    if (out[0] == '\0' && velocityWanted(control) && !velocity_.isActive()) {
        velocityHint(velocity_, out, size);
    }
    if (out[0] == '\0' && (positionRequested_ || pendingTeleport_ || motionWanted(control)) &&
        !position_.isActive()) {
        positionHint(position_, out, size);
    }
    if (out[0] == '\0' && motionWanted(control) && position_.isActive() &&
        !orientation_.isActive()) {
        switch (orientation_.state()) {
            case FeatureState::Failed:
                std::snprintf(out, size, u8"Rotation: Kalibrierung fehlgeschlagen (%s)",
                              orientation_.failReason());
                break;
            case FeatureState::Verifying:
                std::snprintf(out, size, u8"Rotation: prüfe Kandidaten (%u)",
                              orientation_.candidateCount());
                break;
            default:
                std::snprintf(out, size, "%s",
                              u8"Rotation: fahr eine Kurve, damit der Trainer die Drehung findet");
                break;
        }
    }
    return out[0] != '\0';
}

void Trainer::composeActiveList(char* out, std::size_t size) const {
    if (activeFlags_ == 0) {
        copyUtf8(out, size, "Bereit");
        return;
    }
    char list[kListBytes] = u8"Aktiv: ";
    bool first = true;
    if ((activeFlags_ & kActiveFuel) != 0) appendItem(list, sizeof(list), first, u8"Unendlich Sprit");
    if ((activeFlags_ & kActiveNoDamage) != 0) appendItem(list, sizeof(list), first, u8"Kein Schaden");
    if ((activeFlags_ & kActiveBoost) != 0) appendItem(list, sizeof(list), first, u8"Boost");
    if ((activeFlags_ & kActiveNitro) != 0) appendItem(list, sizeof(list), first, u8"Nitro");
    if ((activeFlags_ & kActiveSpeedCap) != 0) appendItem(list, sizeof(list), first, u8"Tempolimit");
    copyUtf8(out, size, list);
}

}  // namespace e2t

namespace e2t {

// Fields added with bridge protocol v2 (orientation, capabilities, counters, cloud saves).
void Trainer::fillExtendedStatus(const TelemetryState& tel, const Control& control, Gate gate,
                                 Status& status) {
    constexpr std::uint64_t kCloudCheckIntervalMs = 5'000;
    const std::uint64_t now = platform_.nowMs();
    if (!cloudChecked_ || now - lastCloudCheckMs_ >= kCloudCheckIntervalMs) {
        cloudAvailable_ = cloud::available();
        cloudChecked_ = true;
        lastCloudCheckMs_ = now;
    }
    status.orientation = featureStatus(orientation_, motionWanted(control) || orientation_.isActive(),
                                       gate != Gate::Open);
    status.capabilities = (cloudAvailable_ ? kCapCloudSaves : 0u) |
                          (input::registered() ? kCapInputDevice : 0u);
    status.breadcrumbCount = static_cast<std::uint32_t>(crumbCount_);
    status.jobStartedCount = tel.jobStartedCount;
    status.jobDeliveredCount = tel.jobDeliveredCount;
    status.saveWriteAck = saveWriteAck_;
    status.saveWriteResult = saveWriteResult_;
}

}  // namespace e2t
