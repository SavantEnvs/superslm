#!/usr/bin/env python3
"""Regenerate the committed fuzz seeds that are derived from upstream fixtures.

Not run at build time. Writes:
  mayhem/model_full.sslm            base fixture + SchemaMasks(SCM1: s_ab, s_any) + DampedGreedyConstants
  mayhem/fuzz_artifact/testsuite/   model_base, model_full, mini_tok artifacts
  mayhem/fuzz_model/testsuite/      model_base, model_full
  mayhem/fuzz_tokenizer/testsuite/  text and token-id samples
  mayhem/fuzz_seq_ops/testsuite/    operation scripts for the sequence/prefix state machine; with
                                    `--blobs DIR` (the output of gen_blob_seeds.cpp) also raw-restore scripts
"""
import sys
import hashlib
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, "..", "tests", "fixtures")
HDR, ROW = 64, 40
DGC_FLAG = 0x2


def parse(b):
    n = struct.unpack_from("<I", b, 12)[0]
    flags = struct.unpack_from("<I", b, 16)[0]
    secs = []
    for i in range(n):
        t, d, off, sz, ec, al = struct.unpack_from("<IIQQQI", b, HDR + ROW * i)
        secs.append(dict(type=t, dtype=d, align=al, data=b[off:off + sz], elem=ec))
    return secs, flags


def build(secs, flags):
    n = len(secs)
    cursor = HDR + ROW * n
    offs = []
    for s in secs:
        cursor = (cursor + s["align"] - 1) // s["align"] * s["align"]
        offs.append(cursor)
        cursor += len(s["data"])
    out = bytearray(cursor)
    out[0:4] = b"SSLM"
    struct.pack_into("<IIII", out, 4, 2, HDR, n, flags)
    struct.pack_into("<Q", out, 24, cursor)
    for i, s in enumerate(secs):
        struct.pack_into("<IIQQQII", out, HDR + ROW * i, s["type"], s["dtype"], offs[i], len(s["data"]),
                         s.get("elem", len(s["data"])), s["align"], 0)
        out[offs[i]:offs[i] + len(s["data"])] = s["data"]
    out[32:64] = b"\0" * 32
    out[32:64] = hashlib.sha256(bytes(out)).digest()
    return bytes(out)


def scm1(vocab, schemas):
    page = (vocab + 7) // 8
    blob = b"".join(s[0].encode() for s in schemas)
    name_off, o = [], 0
    for s in schemas:
        name_off.append(o)
        o += len(s[0])
    cursor = 24 + 56 * len(schemas) + len(blob)
    blocks, placed = [], []
    for name, nstates, accepting, rows in schemas:
        mask = bytearray(nstates * page)
        offs, trans, total = b"", b"", 0
        for st in range(nstates):
            offs += struct.pack("<I", total)
            for tok, nxt in rows[st]:
                mask[st * page + (tok >> 3)] |= 1 << (tok & 7)
                trans += struct.pack("<II", tok, nxt)
                total += 1
        offs += struct.pack("<I", total)
        acc = b"".join(struct.pack("<I", a) for a in accepting)
        p = []
        for part in (bytes(mask), offs, acc, trans):
            p.append(cursor)
            cursor += len(part)
        placed.append(p)
        blocks.append((bytes(mask), offs, acc, trans))
    out = bytearray(b"SCM1")
    out += struct.pack("<IIIII", 1, len(schemas), vocab, len(blob), 0)
    for i, (name, nstates, accepting, rows) in enumerate(schemas):
        tc = sum(len(r) for r in rows)
        out += struct.pack("<IIIIII", name_off[i], len(name), nstates, len(accepting), tc, 0)
        out += struct.pack("<QQQQ", *placed[i])
    out += blob
    for b in blocks:
        for part in b:
            out += part
    return bytes(out)


def tok1_mini(real, nbyte=256, nmerge=300, nspecial=2):
    v, m, s = struct.unpack_from("<III", real, 8)
    p = 24
    b2i = real[p:p + 1024]
    p += 1024
    offs = struct.unpack_from("<%dI" % (v + 1), real, p)
    p += 4 * (v + 1)
    bl = struct.unpack_from("<I", real, p)[0]
    p += 4
    blob = real[p:p + bl]
    p += bl
    merges = real[p:p + 12 * m]
    p += 12 * m
    sids = struct.unpack_from("<%dI" % s, real, p)
    p += 4 * s
    soffs = struct.unpack_from("<%dI" % (s + 1), real, p)
    p += 4 * (s + 1)
    sbl = struct.unpack_from("<I", real, p)[0]
    p += 4
    sblob = real[p:p + sbl]
    nv = nbyte + nmerge
    new_offs = offs[:nv + 1]
    new_blob = blob[:new_offs[-1]]
    new_merges = merges[:12 * nmerge]
    new_sids = [nv + j for j in range(nspecial)]
    new_soffs = soffs[:nspecial + 1]
    new_sblob = sblob[:new_soffs[-1]]
    out = b"TOK1" + struct.pack("<IIIII", 1, nv + nspecial, nmerge, nspecial, 0)
    out += b2i
    # vocab must also hold the special-token entries, in id order
    out_offs = list(new_offs)
    extra = b""
    for j in range(nspecial):
        sp = sblob[soffs[j]:soffs[j + 1]]
        extra += sp
        out_offs.append(out_offs[-1] + len(sp))
    out += struct.pack("<%dI" % len(out_offs), *out_offs)
    out += struct.pack("<I", len(new_blob) + len(extra)) + new_blob + extra
    out += new_merges
    out += struct.pack("<%dI" % nspecial, *new_sids)
    out += struct.pack("<%dI" % (nspecial + 1), *new_soffs)
    out += struct.pack("<I", len(new_sblob)) + new_sblob
    return out


def op(o, si=0, pi=0, bi=0, ws=1, *args):
    return bytes([o, si, pi, bi, ws, *args])


def tk(*toks):
    return bytes(t + 2 for t in toks)


def seq_ops_seeds():
    dec_g = lambda si=0: op(5, si, 0, 0, 1, 2, 0, 0)
    dec_d = lambda si=0: op(5, si, 0, 0, 1, 2, 1, 2, 2, 6, 0)
    pre = lambda si, *t: op(4, si, 0, 0, 1, len(t), *tk(*t), 4, 0)
    return {
        "basic_decode": op(0) + pre(0, 1, 2, 3, 4) + dec_g() + dec_g() + dec_d() + op(20) + op(2),
        "save_restore": op(0) + pre(0, 1, 2, 3) + dec_g() + op(6, 0, 0, 0) + op(7, 1, 0, 0) + dec_g(1)
        + op(23, 1) + op(20, 1) + op(3, 0) + op(2, 0) + op(2, 1),
        "schema_ab": op(0) + op(17, 0, 0, 0, 1, 0, 0, 1) + op(18, 0, 0, 0, 1, 4)
        + op(4, 0, 0, 0, 1, 1, *tk(5), 4, 1) + op(19, 0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 0, *tk(7, 0, 0, 0, 0, 0), 4)
        + dec_g() + op(18, 0, 0, 0, 1, 4) + op(20),
        "schema_any_damped": op(0) + op(17, 0, 0, 0, 1, 1, 0, 1) + pre(0, 1) + dec_d() + dec_d() + dec_d()
        + op(19, 0, 0, 0, 1, 2, 1, 2, 2, 6, 0, 3, *tk(2, 3, 1, 0, 0, 0), 5) + op(20),
        "exact_jump": op(0) + op(17, 0, 0, 0, 1, 0, 0, 1) + pre(0, 0) + dec_g()
        + op(19, 0, 0, 0, 1, 2, 0, 1 | (2 << 1), *tk(7, 0, 0, 0, 0, 0), 6) + dec_g(),
        "prefix_share": op(9, 0, 0, 0, 1, 1) + op(10, 0, 0, 0, 1, 4, *tk(1, 2, 3, 4), 4, 0) + op(11) + op(14, 0, 0, 0)
        + op(15, 0, 0, 0) + op(16, 0, 0, 0, 1, 9) + op(1, 0, 0, 0, 1, 9) + op(13, 0, 0) + dec_g() + op(6, 0, 0, 1)
        + op(21, 1, 0, 1) + op(20, 1) + op(2, 0) + op(2, 1) + op(12, 0) + op(12, 1),
        "pool_pressure": op(0, 0) + op(0, 1) + op(0, 2) + op(1, 0, 0, 0, 1, 16) + op(9, 0, 0, 0, 1, 1)
        + op(9, 0, 1, 0, 1, 1) + op(9, 0, 2, 0, 1, 1) + op(20) + op(2, 0) + op(2, 1) + op(2, 2),
    }


def main():
    base_bytes = open(os.path.join(FIX, "t2572_arm_c_non_qknorm_fixture.sslm"), "rb").read()
    secs, flags = parse(base_bytes)
    schemas = [
        ("s_ab", 3, [2], [[(5, 1)], [(7, 2)], []]),
        ("s_any", 2, [0, 1], [[(1, 1), (2, 1), (3, 0)], [(1, 0), (4, 0)]]),
    ]
    raw = secs[0]["dtype"]
    full_secs = list(secs) + [
        dict(type=30, dtype=raw, align=8, data=scm1(32, schemas)),
        dict(type=42, dtype=raw, align=8, data=struct.pack("<qi", 2883584, -36)),
    ]
    full = build(full_secs, flags | DGC_FLAG)

    tok = open(os.path.join(FIX, "qwen2.5-1.5b.tok.sslm"), "rb").read()
    tsecs, tflags = parse(tok)
    mini = []
    for s in tsecs:
        s = dict(s)
        if s["type"] == 20:
            s["data"] = tok1_mini(s["data"])
            s["elem"] = len(s["data"])
        mini.append(s)
    mini_tok = build(mini, tflags)

    files = {
        "model_full.sslm": full,
        "fuzz_artifact/testsuite/model_base.sslm": base_bytes,
        "fuzz_artifact/testsuite/model_full.sslm": full,
        "fuzz_artifact/testsuite/mini_tok.sslm": mini_tok,
        "fuzz_model/testsuite/model_base.sslm": base_bytes,
        "fuzz_model/testsuite/model_full.sslm": full,
    }
    for name, data in seq_ops_seeds().items():
        files["fuzz_seq_ops/testsuite/" + name] = data
    if "--blobs" in sys.argv:
        d = sys.argv[sys.argv.index("--blobs") + 1]
        for fn, o in (("seq_ssb5.blob", 8), ("seq_ssb6.blob", 8), ("prefix_spx1.blob", 22)):
            blob = open(os.path.join(d, fn), "rb").read()
            files["fuzz_seq_ops/testsuite/raw_" + fn[:-5]] = op(o) + blob
    texts = {
        "ascii": "The quick brown fox jumps over the lazy dog. 12345 + 67890 = 80235\n",
        "code": "def f(x):\n    return [i*i for i in range(x) if i % 2]  # comment\n\tprint('hi')\n",
        "multilingual": "Hello, w\u00f6rld! \u65e5\u672c\u8a9e\u306e\u30c6\u30ad\u30b9\u30c8 \u0645\u0631\u062d\u0628\u0627 \U0001f642 e\u0301",
        "special": "<|im_start|>user\nhi<|im_end|>\n<|endoftext|>",
    }
    for name, t in texts.items():
        files["fuzz_tokenizer/testsuite/enc_" + name] = b"\x00" + t.encode()
    files["fuzz_tokenizer/testsuite/dec_ids"] = b"\x02" + b"".join(struct.pack("<I", v)[:3] for v in
                                                                  (9707, 11, 1879, 0, 151643, 30, 100, 2000))
    for rel, data in files.items():
        path = os.path.join(HERE, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(data)
        print(rel, len(data))


if __name__ == "__main__":
    main()
