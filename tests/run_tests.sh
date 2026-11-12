#!/bin/sh
# Testet die Spielstand-Erkennung (Text, BSII, verschlüsselt) und lädt das Plugin.
set -e
cd "$(dirname "$0")/.."
fx=build/fixtures
python3 tests/make_fixtures.py "$fx" >/dev/null
fail=0
check() {  # Datei, erwartetes Ergebnis, weitere Argumente
	f=$1; want=$2; shift 2
	got=$(./savecheck "$fx/$f" "$@" | sed -n 's/^Ergebnis: *\([a-z]*\).*/\1/p')
	if [ "$got" = "$want" ]; then echo "ok    $f $* -> $got"; else echo "FAIL  $f $* -> $got (erwartet $want)"; fail=1; fi
}
for f in text.sii bsii.sii enc_text.sii enc_bsii.sii; do
	check $f round                                   # zugewiesener LKW: DAF mit Bonfire
	check $f blink --plate "TEST 2"                  # Telemetrie meldet anderen LKW -> per Kennzeichen
	check $f round --plate "TEST 1" --truck daf.xf
	check $f unbekannt --plate "XX 999"              # LKW nicht im Spielstand
done
echo "--- Plugin laden (steuert angeschlossene Leuchten) ---"
./build/test_harness ./ets2_giants_beacon.so "TEST 2" | grep -E 'init|ets2-beacon|shutdown'
exit $fail
