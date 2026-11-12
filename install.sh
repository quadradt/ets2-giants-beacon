#!/bin/sh
# Installiert ets2-giants-beacon: Plugin ins Spiel, udev-Regel (fragt nach sudo), Beispiel-Konfiguration.
# Aufruf: ./install.sh [--ats] [--game "PFAD/zum/Spiel"]
set -e
cd "$(dirname "$0")"
name="Euro Truck Simulator 2"
game=""
while [ $# -gt 0 ]; do
	case "$1" in
		--ats) name="American Truck Simulator" ;;
		--game) game="$2"; shift ;;
		*) echo "unbekannte Option: $1" >&2; exit 2 ;;
	esac
	shift
done
so=ets2_giants_beacon.so
[ -f "$so" ] || { echo "$so fehlt – erst 'make' ausführen oder das Release-Archiv entpacken." >&2; exit 1; }

if [ -z "$game" ]; then
	for lib in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
		[ -f "$lib/steamapps/libraryfolders.vdf" ] || continue
		for p in "$lib" $(sed -n 's/.*"path"[[:space:]]*"\([^"]*\)".*/\1/p' "$lib/steamapps/libraryfolders.vdf"); do
			[ -d "$p/steamapps/common/$name" ] && { game="$p/steamapps/common/$name"; break 2; }
		done
	done
fi
[ -n "$game" ] && [ -d "$game" ] || { echo "Spielordner von '$name' nicht gefunden – mit --game angeben." >&2; exit 1; }

mkdir -p "$game/bin/linux_x64/plugins"
install -m 755 "$so" "$game/bin/linux_x64/plugins/"
echo "Plugin installiert: $game/bin/linux_x64/plugins/$so"

if [ ! -f /etc/udev/rules.d/99-giants-beacon.rules ]; then
	echo "udev-Regel fehlt – installiere sie (sudo):"
	sudo -S -p '' install -m 644 udev/99-giants-beacon.rules /etc/udev/rules.d/
	sudo -S -p '' udevadm control --reload-rules
	sudo -S -p '' udevadm trigger
	echo "Rundumleuchten einmal ab- und wieder anstecken."
fi

cfg="${XDG_CONFIG_HOME:-$HOME/.config}/ets2-beacon"
mkdir -p "$cfg"
[ -f "$cfg/config" ] || printf '# auto = am LKW erkennen, round = immer drehen, blink = immer blinken\nmode=auto\n# wenn auto nichts erkennt\nfallback=round\n' > "$cfg/config"
echo "Konfiguration: $cfg/config"
echo "Fertig. Beim Spielstart den Hinweis \"Advanced SDK features\" mit OK bestätigen."
