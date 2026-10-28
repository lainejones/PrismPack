"""inflate_host.py: test src/inflate.c natively against zlib (raw deflate, levels 0/1/6/9).
Build: gcc -O2 -shared -fPIC -o build/libinflate.so src/inflate.c ; run with python3 in WSL."""
import ctypes, os, random, sys, zlib
lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), '..', 'build', 'libinflate.so'))
lib.pp_inflate.restype = ctypes.c_long
lib.pp_inflate.argtypes = [ctypes.c_char_p, ctypes.c_ulong, ctypes.c_char_p, ctypes.c_ulong]

def raw(data, level, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    return c.compress(data) + c.flush()

def check(name, data):
    for level, strat in ((0, 0), (1, 0), (6, 0), (9, 0), (9, zlib.Z_FIXED), (9, zlib.Z_HUFFMAN_ONLY), (9, zlib.Z_RLE)):
        packed = raw(data, level, strat)
        out = ctypes.create_string_buffer(len(data) + 1)
        rc = lib.pp_inflate(out, len(data), packed, len(packed))
        if rc != 0 or out.raw[:len(data)] != data:
            print('FAIL', name, 'level', level, 'strategy', strat, 'rc', rc); return False
    return True

ok = n = 0
samples = [('empty', b''), ('one', b'x'), ('zeros', bytes(200000)), ('repeat', b'abc' * 50000),
           ('random', bytes(random.Random(1).getrandbits(8) for _ in range(70000)))]
for root in sys.argv[1:]:
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            samples.append((p, open(p, 'rb').read()))
for name, data in samples:
    n += 1; ok += check(name, data)
# stored blocks after compressed ones (sync flushes; incompressible stretches)
def mixed(parts):
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    out = b''
    for p, flush in parts:
        out += c.compress(p) + (c.flush(flush) if flush else b'')
    return out + c.flush()
rnd = random.Random(2)
for k in range(40):
    parts, data = [], b''
    for _ in range(rnd.randint(2, 8)):
        p = (b'text %d ' % rnd.randint(0, 99)) * rnd.randint(1, 3000) if rnd.random() < 0.5 else bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 40000)))
        parts.append((p, rnd.choice([0, zlib.Z_SYNC_FLUSH, zlib.Z_FULL_FLUSH])))
        data += p
    packed = mixed(parts)
    out = ctypes.create_string_buffer(len(data) + 1)
    n += 1
    if lib.pp_inflate(out, len(data), packed, len(packed)) == 0 and out.raw[:len(data)] == data:
        ok += 1
    else:
        print('FAIL mixed stream', k)
# corrupt streams must fail, never crash
bad = 0
for i in range(300):
    data = os.urandom(random.randint(1, 5000))
    packed = bytearray(raw(data, 9)); packed[random.randrange(len(packed))] ^= 1 << random.randrange(8)
    out = ctypes.create_string_buffer(len(data) + 1)
    if lib.pp_inflate(out, len(data), bytes(packed), len(packed)) != 0 or out.raw[:len(data)] != data:
        bad += 1
print('%d/%d samples decode correctly at 7 settings each; %d/300 corrupted streams detected (rest decoded to the same bytes)' % (ok, n, bad))
sys.exit(0 if ok == n else 1)
