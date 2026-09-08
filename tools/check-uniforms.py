#!/usr/bin/env python3
"""Verify GPUUniforms (dsvp.h) and the HLSL cbuffers (player.c) agree.
A mismatch silently corrupts every uniform after the divergence point.

Models BYTE OFFSETS on both sides. The C struct packs its 4-byte
scalars contiguously; DXC packs the cbuffer in 16-byte registers (no
element may straddle a register; float4/float4x4 and every array
element align to 16; an array's size is 16*(N-1)+elem, so a scalar
may follow in the last register's remainder — the D3D reflection
rule).

Review 2026-09 (C2): the old parser recognised only `float`/`int` on
the C side and `float*` on the HLSL side and SKIPPED everything else
silently — a `uint32_t`, `bool` or `unsigned` field contributed zero
bytes to the model and the tool printed "IDENTICAL — safe" on a truly
shifted layout (reproduced by both review substrates). Now: every
declaration must match a small whitelist or the run FAILS with exit 2
("fix the checker, do not trust a verdict"); fields are compared by
NAME as well as by index so the first field whose offset differs is
the one blamed; comments are stripped over the whole body, not per
line; multi-declarators are rejected; the overlay's OvParams cbuffer
is checked against its C-side push size too.

  python3 tools/check-uniforms.py [repo]        exit 0 identical, 1 mismatch, 2 parser
  python3 tools/check-uniforms.py --selftest    replay the review mutations
"""
import os, re, shutil, subprocess, sys, tempfile

C_TYPES    = {'float': 4, 'int': 4, 'int32_t': 4, 'uint32_t': 4,
              'unsigned': 4, 'unsigned int': 4}
# HLSL: (component count, bytes). bool/int/uint are 4-byte cbuffer slots.
HLSL_TYPES = {'float': (1, 4), 'float2': (2, 8), 'float3': (3, 12),
              'float4': (4, 16), 'float4x4': (16, 64),
              'int': (1, 4), 'uint': (1, 4), 'bool': (1, 4)}

def fail(msg):
    print("PARSER FAILED: " + msg + " — fix the checker, do not trust a verdict")
    sys.exit(2)

def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)

def decls(body, where):
    """Split a struct/cbuffer body into (type, name, [dims]) tuples."""
    out = []
    for raw in strip_comments(body).split(';'):
        d = ' '.join(raw.split())
        if not d:
            continue
        if ',' in d:
            fail(f"multi-declarator in {where}: '{d}' — one field per line")
        m = re.match(r'^(?:row_major\s+|const\s+)?([A-Za-z_][\w ]*?)\s+(\w+)((?:\s*\[\s*\d+\s*\])*)$', d)
        if not m:
            fail(f"unparsed declaration in {where}: '{d}'")
        ty, name, dims = m.group(1).strip(), m.group(2), re.findall(r'\d+', m.group(3))
        out.append((ty, name, [int(x) for x in dims]))
    return out

def c_layout(hdr):
    m = re.search(r'typedef struct GPUUniforms \{(.*?)\} GPUUniforms;', hdr, re.S)
    if not m:
        fail("GPUUniforms struct not found in dsvp.h")
    fields, off = [], 0
    for ty, name, dims in decls(m.group(1), "dsvp.h GPUUniforms"):
        if ty not in C_TYPES:
            fail(f"unsupported C field '{ty} {name}' in GPUUniforms (whitelist: {sorted(C_TYPES)})")
        n = 1
        for d in dims: n *= d
        fields.append((name, n, off))
        off += C_TYPES[ty] * n
    return fields, off

def hlsl_body(src, cb):
    i = src.find(f'cbuffer {cb}')
    if i < 0:
        fail(f"cbuffer {cb} not found in player.c")
    seg = src[i:src.index('};', i)]
    seg = seg[seg.index('{') + 1:]
    # the shader is a C string literal: drop the quotes and the escaped newlines
    seg = seg.replace('\\n', '\n').replace('"', '')
    return seg

def hlsl_layout(src, cb):
    fields, off = [], 0
    for ty, name, dims in decls(hlsl_body(src, cb), f"player.c cbuffer {cb}"):
        if ty not in HLSL_TYPES:
            fail(f"unsupported HLSL field '{ty} {name}' in {cb} (whitelist: {sorted(HLSL_TYPES)})")
        comps, ebytes = HLSL_TYPES[ty]
        n = 1
        for d in dims: n *= d
        if dims or ty in ('float4', 'float4x4'):
            if off % 16: off += 16 - off % 16          # register-aligned
            fields.append((name, comps * n, off))
            if ty == 'float4x4':
                off += 64 * n
            else:
                off += 16 * (n - 1) + ebytes            # last element not padded (reflection rule)
        else:
            rem = 16 - off % 16
            if ebytes > rem: off += rem                 # no-straddle
            fields.append((name, comps, off))
            off += ebytes
    if not fields:
        fail(f"no fields in cbuffer {cb}")
    return fields, off

def check(repo):
    hdr = open(f'{repo}/src/dsvp.h', encoding='utf-8').read()
    src = open(f'{repo}/src/player.c', encoding='utf-8').read()
    cf, c_size = c_layout(hdr)
    sf, s_size = hlsl_layout(src, 'Params')

    bad = []
    cmap, smap = {f[0]: f for f in cf}, {f[0]: f for f in sf}
    for name, n, off in cf:                       # by name, in C order
        s = smap.get(name)
        if s is None:
            bad.append((name, (name, n, off), None))
        elif (s[1], s[2]) != (n, off):
            bad.append((name, (name, n, off), s))
    for name, n, off in sf:
        if name not in cmap:
            bad.append((name, None, (name, n, off)))
    if [f[0] for f in cf] != [f[0] for f in sf] and not bad:
        bad.append(("<order>", tuple(f[0] for f in cf), tuple(f[0] for f in sf)))

    print(f"C struct : {len(cf):2d} fields, {c_size} bytes (packed)")
    print(f"HLSL cbuf: {len(sf):2d} fields, {s_size} bytes (D3D-packed)")
    for name, c, s in bad:
        print(f"  MISMATCH {name}: C={c} HLSL={s}   (name, floats, byte offset)")
    if not bad and c_size != s_size:
        print(f"  NOTE: total sizes differ only by tail padding ({c_size} vs {s_size}) — offsets all agree, safe")

    # Second cbuffer: OvParams is pushed from a raw float array, not a struct.
    of, o_size = hlsl_layout(src, 'OvParams')
    m = re.search(r'float\s+ovp\s*\[\s*(\d+)\s*\]', src)
    if not m:
        fail("OvParams C-side push array 'float ovp[N]' not found in player.c")
    push = int(m.group(1)) * 4
    ov_ok = (push == o_size)
    print(f"OvParams : {len(of)} fields, {o_size} bytes; C push {push} bytes — {'OK' if ov_ok else 'MISMATCH'}")

    print("checked: Params<->GPUUniforms (%d fields), OvParams<->ovp[] (%d fields)" % (len(cf), len(of)))
    print("layout:", "IDENTICAL — safe (offsets modeled)" if not bad and ov_ok else "*** MISMATCH ***")
    return 1 if (bad or not ov_ok) else 0

# ── self-test: the review's mutations must produce the review's verdicts ──
def selftest(repo):
    hdr = open(f'{repo}/src/dsvp.h', encoding='utf-8').read()
    src = open(f'{repo}/src/player.c', encoding='utf-8').read()
    C_ANCHOR = re.search(r'^\s*float is_hlg;.*$', hdr, re.M)
    S_ANCHOR = re.search(r'^\s*"\s*float is_hlg;\\n"\s*$', src, re.M)
    if not C_ANCHOR or not S_ANCHOR:
        fail("selftest anchors (is_hlg) not found")
    def c_after(extra):   return hdr[:C_ANCHOR.end()] + '\n    ' + extra + hdr[C_ANCHOR.end():]
    def s_after(extra):   return src[:S_ANCHOR.end()] + '\n    "    ' + extra + '\\n"' + src[S_ANCHOR.end():]
    def c_tail(extra):    # before the closing of the struct
        i = hdr.index('} GPUUniforms;'); return hdr[:i] + '    ' + extra + '\n' + hdr[i:]
    def s_tail(extra):
        i = src.index('cbuffer Params'); j = src.index('};', i)
        k = src.rfind('\n', 0, j); return src[:k] + '\n    "    ' + extra + '\\n"' + src[k:]
    cases = [
        ("unmodified tree",                                   hdr, src, 0),
        ("M1 uint32_t on both sides (real shift)",            c_after("uint32_t hdr_flags;"), s_after("uint hdr_flags;"), 1),
        ("M2 uint32_t on C side only",                        c_after("uint32_t hdr_flags;"), src, 1),
        ("M3 bool on C side (1-byte type)",                   c_after("bool use_lift;"), s_after("bool use_lift;"), 2),
        ("M4 int on both sides (blames the right field)",     c_after("int hdr_flags;"), s_after("int hdr_flags;"), 1),
        ("M5 multi-declarator",                               c_after("float a, b;"), src, 2),
        ("M6 block comment with a ghost field (no false fail)", c_after("/*\n    float ghost;\n    */"), src, 0),
        ("M10 scalar array then scalar (reflection rule)",   c_tail("float arr2[2];\n    float after;\n    float _p2[2];"),
                                                              s_tail("float arr2[2];\\n\"\n    \"    float after;\\n\"\n    \"    float2 _p2;"), 1),
    ]
    tmp = tempfile.mkdtemp(prefix='chkuni-')
    ok = True
    for label, h, s, want in cases:
        d = os.path.join(tmp, re.sub(r'\W+', '_', label)[:24]); os.makedirs(os.path.join(d, 'src'))
        open(os.path.join(d, 'src/dsvp.h'), 'w').write(h)
        open(os.path.join(d, 'src/player.c'), 'w').write(s)
        r = subprocess.run([sys.executable, os.path.abspath(__file__), d], capture_output=True, text=True)
        verdict = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[-120:]
        good = (r.returncode == want)
        ok &= good
        print(f"  {'PASS' if good else 'FAIL'}  exit {r.returncode} (want {want})  {label}  — {verdict}")
        if label.startswith("M4") and good:
            first = [l for l in r.stdout.splitlines() if 'MISMATCH' in l][0]
            blamed_right = 'dovi_num_pieces' in first
            ok &= blamed_right
            print(f"        {'PASS' if blamed_right else 'FAIL'}  first blame is dovi_num_pieces: {first.strip()[:90]}")
    shutil.rmtree(tmp)
    print("SELFTEST:", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(selftest(next((a for a in sys.argv[1:] if a != '--selftest'), '.')))
    sys.exit(check(sys.argv[1] if len(sys.argv) > 1 else '.'))
