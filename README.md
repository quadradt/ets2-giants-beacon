# ets2-beacon

Natives Linux-Telemetrie-Plugin für Euro Truck Simulator 2 (und ATS): Schaltet man im Spiel die Rundumleuchte des Trucks ein, laufen die GIANTS-USB-Rundumleuchten (340d:1710) mit. Alle angeschlossenen Leuchten werden angesteuert.

- Liest den SDK-Kanal `truck.light.beacon` und schreibt direkt auf `/dev/hidrawN`. Ein Hintergrunddienst ist nicht nötig.
- Schickt den Zustand alle 3 s erneut, weil die Hardware nach 10 s von selbst abschaltet. Beim Beenden des Spiels geht die Leuchte aus.
- Zugriffsrechte: die udev-Regel aus `giants-beacon` (`/etc/udev/rules.d/99-giants-beacon.rules`).
- Modus: `echo blink > ~/.config/ets2-beacon/mode` (Standard `round`) oder die Umgebungsvariable `ETS2_BEACON_MODE`.
- Meldungen erscheinen in `~/.local/share/Euro Truck Simulator 2/game.log.txt` mit dem Präfix `[ets2-beacon]`.

## Bauen und Installieren

    make            # ets2_beacon.so, braucht nur gcc und glibc ≥ 2.17
    make test       # Testlauf ohne Spiel: echte Leuchten AN/AUS
    make install    # kopiert nach .../Euro Truck Simulator 2/bin/linux_x64/plugins/

Für ATS: `make install GAME="$HOME/.local/share/Steam/steamapps/common/American Truck Simulator"`.

Beim Start zeigt ETS2 einen Hinweis auf „Advanced SDK features“. Mit OK bestätigen.

Die SDK-Header unter `sdk/` stammen aus dem SCS Telemetry SDK 1.14 von SCS Software.
