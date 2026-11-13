<div align="center">

# ets2-giants-beacon

**Die GIANTS-USB-Rundumleuchte aus dem Landwirtschafts-Simulator, live gesteuert von Euro Truck Simulator 2 und American Truck Simulator unter Linux.**

[![CI](https://github.com/quadradt/ets2-giants-beacon/actions/workflows/ci.yml/badge.svg)](https://github.com/quadradt/ets2-giants-beacon/actions/workflows/ci.yml)
[![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/license-PolyForm%20Noncommercial-orange.svg)](LICENSE.md)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)

</div>

---

> **English summary:** A native Linux telemetry plugin for ETS2/ATS that drives the GIANTS Software USB beacon (Farming Simulator merch, USB `340d:1710`). The light turns on when you switch on your truck's beacon in-game. It **auto-detects** whether your truck has rotating beacons (→ spin) or LED strobes (→ flash) by reading your save game. Install: download the release, run `./install.sh`. Linux native version of the game only (not Proton).

> **Hinweis zur Entstehung:** Code, Werkzeuge und Dokumentation dieses Projekts hat eine KI (Claude) erstellt. Getestet hat sie ein Mensch, [quadradt](https://github.com/quadradt): im Spiel mit zwei echten GIANTS-Rundumleuchten unter Fedora Linux.
>
> *Note: The code, tools and documentation were created by an AI (Claude) and tested by a human ([quadradt](https://github.com/quadradt)) in-game with two real GIANTS beacons on Fedora Linux.*

## Was es macht

- Du schaltest im Spiel die Rundumleuchte ein, und die echte GIANTS-Leuchte am PC läuft mit. Beim Ausschalten oder Beenden des Spiels geht sie aus.
- **Automatische Erkennung:** Das Plugin schaut nach, welche Leuchten an deinem LKW verbaut sind:
  - drehende Rundumleuchten (Bonfire, Space, Classic, Radiant …) → Leuchte **dreht** (`round`)
  - LED-Blitzer/Strobes (Star- und Nova-Balken, LED-Strobe, Blitzer im Grill, DAF/MAN-Werksblitzer …) → Leuchte **blinkt** (`blink`)
- Mehrere Leuchten gleichzeitig, Ab- und Anstecken während des Spiels, kein Hintergrunddienst: Das Plugin lebt im Spiel.

## Voraussetzungen

- Linux und die **native Linux-Version** von ETS2 oder ATS über Steam (Standard). Unter Proton läuft die Windows-Version, die bräuchte eine Windows-DLL. Die gibt es (noch) nicht.
- Eine oder mehrere [GIANTS-USB-Rundumleuchten](https://www.farming-simulator.com/buy-now.php?platform=hardware) (USB-ID `340d:1710`, `lsusb` zeigt „GIANTS Software Beacon Light“).

## Installation

1. Neuestes Release unter [Releases](https://github.com/quadradt/ets2-giants-beacon/releases/latest) herunterladen und entpacken.
2. Im entpackten Ordner:

   ```bash
   ./install.sh          # ETS2
   ./install.sh --ats    # American Truck Simulator
   ```

   Das Skript kopiert das Plugin nach `…/Euro Truck Simulator 2/bin/linux_x64/plugins/`. Fehlt die udev-Regel für den USB-Zugriff ohne root, installiert es sie mit `sudo`. Außerdem legt es `~/.config/ets2-beacon/config` an.
3. Spiel starten. Den Hinweis **„Advanced SDK features“** mit OK bestätigen. Er erscheint bei jedem Telemetrie-Plugin.

Von Hand: `ets2_giants_beacon.so` in `bin/linux_x64/plugins/` des Spielordners legen (Ordner ggf. anlegen) und `udev/99-giants-beacon.rules` nach `/etc/udev/rules.d/` kopieren.

### Selbst bauen

```bash
make            # braucht nur gcc und make
make test       # Tests (Spielstand-Formate) und Probelauf mit echten Leuchten, falls angesteckt
./install.sh
```

## Konfiguration

`~/.config/ets2-beacon/config`:

```ini
mode=auto       # auto = am LKW erkennen, round = immer drehen, blink = immer blinken
fallback=round  # wenn auto nichts erkennt (z. B. Schnellauftrag mit fremdem LKW)
```

Die Umgebungsvariable `ETS2_BEACON_MODE=blink` (Steam-Startoption: `ETS2_BEACON_MODE=blink %command%`) überschreibt `mode`.

## So funktioniert die Erkennung

Das SDK meldet nur „Rundumleuchte an/aus“, nicht welche Leuchte verbaut ist. Deshalb liest das Plugin den **neuesten Spielstand** (`game.sii`, auch verschlüsselt und im Binärformat) und sucht darin deinen LKW. Grundlage ist das Kennzeichen und das Modell, die das Spiel über die Telemetrie meldet. Das verbaute Zubehör wird dann in einer Tabelle nachgeschlagen.

Die Tabelle (`src/beacon_table.h`, lesbar in `data/beacons_ets2.tsv`) wird mit `tools/gen_beacon_table.py` direkt aus den Spieldateien erzeugt. Drehende Leuchten haben im Modell einen Lichtpunkt `flare.vehicle.beacon`, Blitzer dagegen `…vehicle.beacon.blink_l/_r`.

Grenzen, ehrlich gesagt:

- **Neu gekauftes Zubehör** kennt das Plugin erst nach dem nächsten Speichern (Autosave, Schnellspeichern oder einfach einmal speichern).
- **Schnellaufträge** mit LKW der Firma stehen nicht im Spielstand. Dann gilt `fallback`.
- **Hat der LKW beides** (z. B. drehende Dachleuchte und Blitzer im Grill), entscheidet die Leuchte auf dem Dach.
- **American Truck Simulator:** Die mitgelieferte Tabelle stammt aus ETS2. Für ATS einmal eine eigene Tabelle erzeugen (Befehl unten, `--game` auf den ATS-Ordner, Workshop-ID `270880`), sonst gilt dort `fallback`.
- **Mod-Rundumleuchten** stehen nicht in der mitgelieferten Tabelle. Das Plugin rät dann am Dateinamen (`strobe`, `flash` …) oder nimmt `fallback`. Besser ist eine eigene Tabelle, die auch deine Mods enthält:

  ```bash
  # SCS-Extractor (frei, Linux-Build): https://github.com/sk-zk/Extractor/releases
  python3 tools/gen_beacon_table.py --extractor ./extractor \
      --game ~/.local/share/Steam/steamapps/common/"Euro Truck Simulator 2" \
      --mods ~/.local/share/Steam/steamapps/workshop/content/227300 ~/.local/share/"Euro Truck Simulator 2"/mod \
      --out ~/.config/ets2-beacon/beacons.tsv
  ```

  Einträge in `~/.config/ets2-beacon/beacons.tsv` (`schlüssel<TAB>round|blink`) gehen vor. Schlüssel ist der `data_path` des Zubehörs oder der Name eines Hookups (z. B. `strobe01.addon_hookup`).

### Prüfen, was erkannt wird

```bash
./savecheck                                   # neuester Spielstand, zugewiesener LKW
./savecheck --plate "AB 123" --truck scania   # bestimmter LKW
```

Im Spiel schreibt das Plugin nach `~/.local/share/Euro Truck Simulator 2/game.log.txt`, z. B.:

```
[ets2-beacon] 2 Rundumleuchte(n) gefunden
[ets2-beacon] LKW scania.r_2016: blink (0 drehend, 19 blinkend, 170 ms) -> Modus blink
[ets2-beacon] Rundumleuchte blink (2/2 Geraete)
```

## Fehlersuche

| Problem | Lösung |
|---|---|
| Keine Zeile `[ets2-beacon]` im Log | Plugin liegt nicht in `bin/linux_x64/plugins/`, oder das Spiel läuft unter Proton (Steam → Eigenschaften → Kompatibilität aus). |
| `0 Rundumleuchte(n) gefunden` / `nicht oeffenbar` | udev-Regel fehlt oder Leuchte nach dem Installieren nicht neu angesteckt. `lsusb` prüfen. |
| Falscher Modus | `./savecheck` ausführen. Steht der LKW nicht drin: einmal speichern. Bei Mod-Leuchten eigene Tabelle erzeugen (siehe oben) oder `mode=` fest einstellen. |

## Steam Workshop?

Leider nein. Der Workshop-Uploader von SCS nimmt nur Spiel-Mods (`.scs`) an, keine nativen Programme. Telemetrie-Plugins müssen in den `plugins`-Ordner des Spiels, das kann ein Workshop-Mod nicht leisten. Deshalb gibt es das Plugin hier über GitHub-Releases.

## Danke

- **[cutetux/giants-beacon](https://github.com/cutetux/giants-beacon)**: Von dort kam die Idee. Das Projekt steuert die Leuchte unter Linux für den Landwirtschafts-Simulator, und das USB-Protokoll stammt von dort.
- [steve228uk](https://gist.github.com/steve228uk/873d653f1ecec0456ea3f475b6e54f68) für die erste Analyse des Protokolls.
- [DecryptTruck](https://github.com/CoffeSiberian/DecryptTruck) und [ets2-decrypt](https://github.com/avoinescu/ets2-decrypt) für die Beschreibung des Spielstand-Formats (BSII).
- [sk-zk/Extractor](https://github.com/sk-zk/Extractor) zum Auslesen der Spieldateien für die Leuchten-Tabelle.
- SCS Software für das Telemetrie-SDK (Header unter `sdk/`, MIT-Lizenz, siehe `sdk/LICENSE.txt`).

## Rechtliches

Inoffizielles Community-Projekt, **nicht** verbunden mit SCS Software oder GIANTS Software. „Euro Truck Simulator 2“, „American Truck Simulator“, „Farming Simulator“ und die USB-Rundumleuchte sind Marken bzw. Eigentum ihrer Inhaber. Es sind keine Spieldateien enthalten, nur Pfadnamen von Zubehörteilen in der Leuchten-Tabelle.

## Lizenz

[PolyForm Noncommercial 1.0.0](LICENSE.md): frei nutzen, ändern und weitergeben für nicht-kommerzielle Zwecke, ohne Gewähr. Gilt für den eigenen Code. Die SCS-SDK-Header stehen unter MIT (`sdk/LICENSE.txt`).
