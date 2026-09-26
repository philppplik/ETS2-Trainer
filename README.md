# ETS2 Trainer

Mod-Menü / Trainer für **Euro Truck Simulator 2** (getestet gegen 1.61, DX11, Windows).
Gedacht für **Singleplayer und offizielle SCS-Konvois** – **nicht für TruckersMP** (dort sperrt sich das Plugin selbst).

## Was es kann

| Bereich | Funktion | Wie | Im Konvoi? |
|---|---|---|---|
| **LIVE** | Unendlich Sprit, Volltanken | In-Game-Plugin, Auto-Kalibrierung | ✔ |
| | Kein Schaden, Sofort-Reparatur | In-Game-Plugin | ✔ |
| | Leistungs-Boost ×1–×10, Nitro-Taste, Tempo-Limit, Not-Stopp | In-Game-Plugin (Geschwindigkeitsvektor) | ✔ |
| | Instrumente: Tempo, Gang, Drehzahl, Tank, Schaden, Tempolimit, Auftrag | Offizielle SCS-Telemetrie | ✔ |
| **FIRMA** | Firmenkonto, Erfahrung, ADR-Klassen, alle Skills, Kredite tilgen, Städte besuchen | Spielstand-Editor | vorher |
| **FLOTTE** | Alle Garagen kaufen + auf groß ausbauen, alle Fahrer maximieren, Flotte reparieren + tanken | Spielstand-Editor | vorher |
| **WERKSTATT** | Eigene Motoren mit frei wählbarer PS-Zahl (Sound/Kennlinie vom Original), direkt einbauen | Mod-Generator + Spielstand | vorher* |
| **SETUP** | Plugin installieren, Menü-Taste (F8), Nitro-Taste, Backups | – | – |

\* Im Konvoi müssen Mods ggf. zur Session passen – der Motor-Mod ist eine normale lokale Mod.

## Schnellstart

1. `build.cmd` ausführen (braucht Visual Studio mit C++ und .NET 8 SDK) → `dist\ETS2Trainer\ETS2Trainer.exe`
2. Trainer starten → **SETUP → Installieren / Aktualisieren** (ETS2 dabei geschlossen)
3. ETS2 starten, den Hinweis „SDK-Plugins“ mit **OK** bestätigen
4. Trainer-Fenster über dem Spiel mit **F8** ein-/ausblenden (ETS2 am besten im Modus *Vollbild randlos* oder *Fenster*)

## Bedienung

### LIVE (funktioniert auch im Konvoi)
Funktion einschalten und losfahren. Das Plugin findet die nötigen Speicherstellen **selbst** („Auto-Kalibrierung“):
es vergleicht den Arbeitsspeicher mit den offiziellen Telemetrie-Werten, bis nur noch die echte Stelle übrig ist, und
prüft sie mit einem kleinen Test-Schreibzugriff. Dadurch gibt es **keine festen Adressen**, die nach Spiel-Updates kaputtgehen.

- **Sprit:** kurz fahren, bis sich der Tankinhalt ändert → Chip wird grün „AKTIV“.
- **Schaden:** jeder Verschleißwert wird erkannt, sobald er sich einmal ändert (Werte, die bei 0 stehen, brauchen einen ersten Kratzer).
- **Boost/Nitro/Limit:** ab ca. 20 km/h fahren; der Truck wird bei der Kalibrierung kurz minimal angeschoben.
- Nach Truckwechsel/Laden: **Neu kalibrieren** (passiert meist automatisch).
- Schließt du den Trainer, schaltet das Plugin nach 3 Sekunden alle Funktionen ab.

### FIRMA / FLOTTE (Spielstand-Editor)
1. Im Spiel speichern (oder Autosave nutzen)
2. Profil + Spielstand wählen → **Laden**
3. Werte ändern / Aktionen klicken → **Speichern**
   - *Als neuen Spielstand* (empfohlen): Original bleibt unberührt, der neue Stand heißt **„[Trainer] …“**
   - *Überschreiben*: vorher wird automatisch ein Backup nach `Dokumente\Euro Truck Simulator 2\ets2_trainer_backups` gelegt
4. Im Spiel den „[Trainer]“-Stand laden

Spielstände in der Steam Cloud werden direkt erkannt. Geschrieben wird im Klartext-SII-Format, das ETS2 selbst lesen kann.

### WERKSTATT (PS frei wählen)
1. Truck-Modell + Basis-Motor wählen (der Basis-Motor liefert Sound, Drehmomentkurve und Emblem)
2. PS per Regler wählen (100 – 10.000) → **Erzeugen & in meinen Truck einbauen** → unten **Speichern**
3. Einmalig **Mod im Profil aktivieren** (Spiel geschlossen) oder im Mod-Manager „ETS2 Trainer - Motoren“ einschalten

Alternativ „Nur erzeugen“: der Motor ist dann in der Werkstatt für 1 € kaufbar.

## Sicherheit & Grenzen

- **TruckersMP:** wird erkannt (`core_ets2mp.dll`) → alle Schreibzugriffe gesperrt.
- **World of Trucks:** Aufträge mit aktiven Cheats nicht abgeben.
- Live-Funktionen hängen von der Auto-Kalibrierung ab. Klappt sie nicht, zeigt der Chip „FEHLGESCHLAGEN“ und das Plugin-Log
  (`Dokumente\Euro Truck Simulator 2\ets2_trainer_plugin.log`, SETUP → Plugin-Log öffnen) sagt warum.
- Der Boost wirkt auf den Geschwindigkeitsvektor (physikalisch „mehr Schub“), nicht auf das Motor-Datenblatt. Für echte PS-Werte die Werkstatt nutzen.

## Projektstruktur

```
shared/bridge_protocol.h     Shared-Memory-Vertrag Plugin <-> App (Layout per static_assert fixiert)
src/plugin/                  C++ SCS-Telemetrie-Plugin (Auto-Kalibrierung, Live-Cheats, Selbsttest)
src/Ets2Trainer.Core/        SII (ScsC/BSII/Text), HashFS v2 + CityHash, Save-Editor, Motor-Mod, Bridge-Client
src/Ets2Trainer.App/         WPF-Oberfläche (MVVM, eigenes Theme)
src/Ets2Trainer.Tests/       Test-Runner ohne NuGet-Abhängigkeiten (liest echte Saves nur lesend)
build.cmd                    Plugin + Tests + App -> dist\ETS2Trainer
```

Entwickler-Hilfe: `ETS2Trainer.exe --screenshot <ordner> [--load]` rendert alle Tabs als PNG (liest Saves nur).
