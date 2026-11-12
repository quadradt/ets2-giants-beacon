#!/usr/bin/env python3
"""Erzeugt die Tabelle "Zubehör -> Rundumleuchten-Art" aus den Spieldateien.

Liest alle Truck-, Trailer- und Hookup-Zubehördefinitionen aus den .scs-Archiven
(Spiel, DLCs, optional Mods). Dann prüft es in jedem Modell (.pmg), welche
Lichtpunkte (Flares) es hat:

  flare.vehicle.beacon                   -> round  (drehende Rundumleuchte)
  *.vehicle.beacon.blink_l / .blink_r    -> blink  (LED-Blitzer / Strobe)

Benötigt den freien SCS-Extractor von sk-zk (https://github.com/sk-zk/Extractor).

Aufruf:
  tools/gen_beacon_table.py --extractor PFAD/extractor --game "PFAD/Euro Truck Simulator 2" \
      [--mods MODORDNER ...] --out beacons.tsv [--header src/beacon_table.h]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

UNIT_RX = re.compile(r'(accessory_\w+)\s*:\s*([\w.]+)\s*\{(.*?)\n\}', re.S)
MODEL_RX = re.compile(r'\b(?:exterior_model|model)\s*:\s*"([^"]+\.pmd)"')
VARIANT_RX = re.compile(r'\bvariant\s*:\s*"?(\w+)')
# Modelle mit Varianten (z. B. Grill "led" / "strobe") haben die Blitzer nur in
# bestimmten Varianten. Solche Variantennamen gelten als Rundumleuchte.
BEACON_VARIANT_RX = re.compile(r'strobe|beacon|blink|flash|warn', re.I)
DEF_DIRS = ['/def/vehicle/truck', '/def/vehicle/trailer_owned', '/def/vehicle/addon_hookups']


def run_extractor(exe, archive, dest, partial=None, paths_file=None):
    cmd = [exe, archive, '-q', '-d', dest]
    if partial:
        cmd += ['-p=' + ','.join(partial)]
    if paths_file:
        cmd += ['-P', paths_file]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def archives(game, mods):
    out = [os.path.join(game, n) for n in ('base.scs', 'base_share.scs', 'base_vehicle.scs', 'def.scs')]
    out += sorted(os.path.join(game, f) for f in os.listdir(game)
                  if f.startswith('dlc_') and f.endswith('.scs'))
    for m in mods:
        for root, _, files in os.walk(m):
            out += [os.path.join(root, f) for f in files if f.endswith(('.scs', '.zip'))]
    return [a for a in out if os.path.isfile(a)]


def classify(pmg_bytes):
    names = set(re.findall(rb'[a-z_]+\.vehicle\.beacon[a-z0-9_.]*', pmg_bytes))
    if not names:
        return None
    if any(b'blink' in n or b'strobe' in n for n in names):
        return 'blink'
    return 'round'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extractor', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--mods', nargs='*', default=[])
    ap.add_argument('--out', required=True)
    ap.add_argument('--header')
    a = ap.parse_args()

    arcs = archives(a.game, a.mods)
    with tempfile.TemporaryDirectory() as tmp:
        # 1. Definitionen aus allen Archiven (spätere überschreiben frühere wie im Spiel)
        for i, arc in enumerate(arcs):
            run_extractor(a.extractor, arc, os.path.join(tmp, 'd%03d' % i), DEF_DIRS)

        key_model = {}
        for i in range(len(arcs)):
            base = os.path.join(tmp, 'd%03d' % i)
            for root, _, files in os.walk(base):
              for f in files:
                if not f.endswith(('.sii', '.sui')):
                    continue
                p = os.path.join(root, f)
                rel = '/' + os.path.relpath(p, base)
                text = open(p, errors='ignore').read()
                for cls, uid, body in UNIT_RX.findall(text):
                    m = MODEL_RX.search(body)
                    if not m:
                        continue
                    v = VARIANT_RX.search(body)
                    entry = (m.group(1), v.group(1) if v else '')
                    if cls == 'accessory_hookup_data':
                        key_model[uid] = entry               # Hookup: Unit-Name
                    elif f.endswith('.sii'):
                        key_model[rel] = entry               # Zubehör: data_path

        # 2. Nur die referenzierten Modelle (.pmg) holen
        pmgs = sorted({mp[:-4] + '.pmg' for mp, _ in key_model.values()})
        lst = os.path.join(tmp, 'paths.txt')
        open(lst, 'w').write('\n'.join(pmgs) + '\n')
        mdir = os.path.join(tmp, 'models')
        for arc in arcs:
            run_extractor(a.extractor, arc, mdir, paths_file=lst)

        mode_of_model = {}
        for pmg in pmgs:
            p = mdir + pmg
            if os.path.isfile(p):
                c = classify(open(p, 'rb').read())
                if c:
                    mode_of_model[pmg[:-4] + '.pmd'] = c

    # Modelle, bei denen nur bestimmte Varianten Blitzer haben
    variant_models = {m for m, v in key_model.values() if v and BEACON_VARIANT_RX.search(v)}
    rows = []
    for k, (m, v) in key_model.items():
        if m not in mode_of_model:
            continue
        if m in variant_models and not BEACON_VARIANT_RX.search(v or ''):
            continue    # z. B. Grill-Variante "led" ohne Blitzer
        rows.append((k, mode_of_model[m]))
    rows.sort()
    with open(a.out, 'w') as f:
        f.write('# erzeugt von tools/gen_beacon_table.py – Schlüssel<TAB>round|blink\n')
        for k, v in rows:
            f.write('%s\t%s\n' % (k, v))
    if a.header:
        with open(a.header, 'w') as f:
            f.write('/* Automatisch erzeugt von tools/gen_beacon_table.py – nicht von Hand ändern. */\n')
            f.write('static const struct { const char *key; char blink; } BEACON_TABLE[] = {\n')
            for k, v in rows:
                f.write('\t{"%s", %d},\n' % (k, 1 if v == 'blink' else 0))
            f.write('};\n')
    nb = sum(1 for _, v in rows if v == 'blink')
    print('%d Archive, %d Zubehoer-Eintraege, %d Rundumleuchten (%d blink, %d round)'
          % (len(arcs), len(key_model), len(rows), nb, len(rows) - nb))


if __name__ == '__main__':
    sys.exit(main())
