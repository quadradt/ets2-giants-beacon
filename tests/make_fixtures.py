#!/usr/bin/env python3
"""Erzeugt Test-Spielstände (Text, BSII, verschlüsselt) im Verzeichnis argv[1].

Inhalt: Spieler fährt LKW "TEST 1" (DAF XF, Dach-Bonfire = drehend) und besitzt
einen zweiten LKW "TEST 2" (Scania R 2016, Star-Balken + LED-Strobes = blinkend).
"""
import os, struct, subprocess, sys, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

TRUCKS = [
    ('_nameless.1.a', 'TEST 1|_germany', [('/def/vehicle/truck/daf.xf/accessory/beacon/shape1.sii', []),
                                          ('/def/vehicle/truck/daf.xf/accessory/f_grill/stock.sii', [])]),
    ('_nameless.1.b', 'TEST 2|_germany', [('/def/vehicle/truck/scania.r_2016/accessory/beacon/beacon_bar_01.sii', []),
                                          ('/def/vehicle/truck/scania.r_2016/accessory/b_grill/shape6_low.sii',
                                           ['strobe01.addon_hookup', 'strobe01.addon_hookup'])]),
]

# ---------- Text (SiiN) ----------
def text_save():
    L = ['SiiNunit', '{', 'player : _nameless.9.1 {', ' money_account: 1234',
         ' assigned_vehicles: _nameless.9.2', ' trucks: 2', ' trucks[0]: _nameless.1.a', ' trucks[1]: _nameless.1.b', '}', '',
         'player_vehicles : _nameless.9.2 {', ' vehicle: _nameless.1.a', ' trailer: null', '}', '']
    acc_n = 0
    for vid, plate, accs in TRUCKS:
        ids = []
        body = []
        for dp, hooks in accs:
            acc_n += 1
            aid = '_nameless.3.%x' % acc_n
            ids.append(aid)
            body += ['vehicle_addon_accessory : %s {' % aid, ' slot_name: %d' % len(hooks)]
            body += [' slot_name[%d]: slot_%d' % (i, i) for i in range(len(hooks))]
            body += [' slot_hookup: %d' % len(hooks)]
            body += [' slot_hookup[%d]: %s' % (i, h) for i, h in enumerate(hooks)]
            body += [' data_path: "%s"' % dp, '}', '']
        L += ['vehicle : %s {' % vid, ' engine_wear: 0', ' license_plate: "%s"' % plate, ' accessories: %d' % len(ids)]
        L += [' accessories[%d]: %s' % (i, a) for i, a in enumerate(ids)]
        L += ['}', ''] + body
    L += ['}']
    return '\n'.join(L).encode()

# ---------- BSII ----------
def s(x): x = x.encode(); return struct.pack('<I', len(x)) + x
def nameless(n): return b'\xff' + struct.pack('<Q', n)

def bsii_save():
    # Typen: 1 string, 2 string[], 5 float, 25 vec8, 39 u32, 40 u32[], 57 id, 58 id[], 59 id, 53 bool
    structs = {
        1: ('player', [(39, 'money_account'), (57, 'assigned_vehicles'), (58, 'trucks'), (25, 'pos'), (40, 'misc')]),
        2: ('player_vehicles', [(59, 'vehicle'), (59, 'trailer'), (53, 'flag')]),
        3: ('vehicle', [(5, 'engine_wear'), (1, 'license_plate'), (58, 'accessories')]),
        4: ('vehicle_addon_accessory', [(2, 'slot_hookup'), (1, 'data_path')]),
    }
    b = bytearray(b'BSII' + struct.pack('<I', 3))
    for sid, (name, fields) in structs.items():
        b += struct.pack('<I', 0) + b'\x01' + struct.pack('<I', sid) + s(name)
        for t, fn in fields:
            b += struct.pack('<I', t) + s(fn)
        b += struct.pack('<I', 0)
    vids = {'_nameless.1.a': 0xa1, '_nameless.1.b': 0xb1}
    b += struct.pack('<I', 1) + nameless(0x91) + struct.pack('<I', 1234) + nameless(0x92)
    b += struct.pack('<I', 2) + nameless(0xa1) + nameless(0xb1) + struct.pack('<8f', *[1.0] * 8) + struct.pack('<I2I', 2, 7, 8)
    b += struct.pack('<I', 2) + nameless(0x92) + nameless(0xa1) + b'\x00' + b'\x01'
    acc = 0x300
    for vid, plate, accs in TRUCKS:
        ids = []
        for dp, hooks in accs:
            acc += 1
            ids.append(acc)
            b += struct.pack('<I', 4) + nameless(acc) + struct.pack('<I', len(hooks)) + b''.join(s(h) for h in hooks) + s(dp)
        b += struct.pack('<I', 3) + nameless(vids[vid]) + struct.pack('<f', 0.5) + s(plate)
        b += struct.pack('<I', len(ids)) + b''.join(nameless(i) for i in ids)
    b += struct.pack('<I', 0) + b'\x00'
    return bytes(b)

# ---------- ScsC (AES-256-CBC + zlib), mit openssl ----------
KEY = '2a5fcb1791d22fb60245b3d8369ed0b2c27371563fbf1f3c9edf6b11825a5d0a'
def scsc(data):
    iv = os.urandom(16)
    comp = zlib.compress(data)
    ct = subprocess.run(['openssl', 'enc', '-aes-256-cbc', '-K', KEY, '-iv', iv.hex()],
                        input=comp, capture_output=True, check=True).stdout
    return b'ScsC' + b'\0' * 32 + iv + struct.pack('<I', len(data)) + ct

for name, data in [('text.sii', text_save()), ('bsii.sii', bsii_save())]:
    open(os.path.join(out, name), 'wb').write(data)
    open(os.path.join(out, 'enc_' + name), 'wb').write(scsc(data))
print('Fixtures in', out)
