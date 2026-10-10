"""Paged-KV plan rev 16.21 §23.8 step T4: red cells for the five-process 7.9 driver (tools/run_paged_kv_rows.py).

Cells (cell 7.9's text; the 79-series rows of the instrument table):
  79-12   the per-process row rule's exact bar: five process medians at exactly 0.05 PASS, at
          nextafter(0.05, +inf) FAIL; 0.04 PASS and 0.06 FAIL; the mixed cases NO RESULT and FAIL
  79-9    a process with fewer (or more) than 104 pairs is refused
  79-8    a token mismatch in any pair stops the process
  79-10   a second runner is refused while the lock owner lives, with a different result path
  79-11   a torn record reads as missing
  boot    a process on another boot is discarded
  guard   the bound table (n, P, M99) at B = 64 and at 7.9-MR-OPT's B = 16; M's definition; the
          vitality constructions at both page sizes (the T-3146 shuffle passes; ascending ids, the pair
          churn, interleave-5 and the set-concentrating order are rejected); the five id lists agree
  79-6    (rev 16.23, T-3189 G-4) a blob or manifest mismatch in any process: the restored blob's digest, the
          manifest chain, the model binding; refused before restore, its own exit code, no record, row ungraded
  79-7    a seam-library build, checked once per process before timing
  per-process refusals: each kind its own non-zero exit code; a row is never graded on four processes
  T6      (T-3202 remedies, SuperSLM 1478be7; class T6Remedies) F-1 the row record's status and exe_sha256, NO RESULT
          standing, a missing pair executable; F-12 a refused run supersedes an existing record, the rerun limit too;
          F-2 a crash after a seam identity is rerun, a transient crash is rerun; F-11 the RESERVE line recorded; F-9 an
          OS-lock holder mid-recovery refuses a second runner
  loop    (T-3196 F-6) 79-6 to 79-9 through the driver's process loop, as a process: five processes against a
          stand-in pair executable, each refusal injected into process 3 (a blob changed after process 2, a seam
          identity line, a token mismatch at pair 57, 103 pairs); the exit status is EXIT_CODES[kind], the refused
          process writes no record, no row record or verdict exists, processes 1 and 2 keep their records, and the
          blob refusal fires before process 3 runs;
          a healthy row exits 0 with five process records and a graded row record

PKV_ROWS_DRIVER, when set, names the driver file under test instead of tools/run_paged_kv_rows.py (the test author's
scratch reference and its mutants run through it); unset, the shipped driver is what every cell grades.
"""
import collections
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(os.environ.get("PKV_ROWS_DRIVER") or Path(__file__).with_name("run_paged_kv_rows.py"))
spec = importlib.util.spec_from_file_location("paged_kv_rows", SCRIPT)
rows = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rows)

# Cell 7.9's tables, transcribed (rev 16.21): slot -> (n, P, M99).
PRODUCTION = {"R2a": (21, 64, 3), "R2c": (34, 64, 4), "R2b": (64, 64, 6),
              "R4c": (130, 512, 4), "R4a": (250, 512, 5), "R4b": (294, 512, 5)}
MR_OPT = {"R2b": (256, 256, 6), "R4a": (999, 2048, 5), "R4b": (1175, 2048, 6)}
# Each row's geometry for the set-concentrating order: (runs per page, bytes per position per run).
GEOMETRY = {"R2a": (96, 64), "R2c": (96, 64), "R2b": (96, 64), "R4c": (112, 128), "R4a": (112, 128), "R4b": (112, 128)}
WAY = 1 << 20


# The plan's vitality constructions (Claude/Plans/superslm-paged-kv/probes/scatter_guard.py), as test data.
def proto_shuffle(pool):
    res = list(range(pool))
    x = 3146
    for i in range(pool, 1, -1):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        k = (x >> 8) % i
        res[i - 1], res[k] = res[k], res[i - 1]
    return list(reversed(res))


def pair_churn(pool, seed=7):
    pairs = [(2 * i, 2 * i + 1) for i in range(pool // 2)]
    order = list(range(len(pairs)))
    x = seed
    for i in range(len(order), 1, -1):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        k = (x >> 8) % i
        order[i - 1], order[k] = order[k], order[i - 1]
    stack = []
    for h in order:
        stack.extend(pairs[h])
    return [stack.pop() for _ in range(pool)]


def interleave(pool, k=5):
    return [p for r in range(k) for p in range(r, pool, k)]


def set_concentrating(n, runs, run_bytes, page_bytes, pool):
    mods = collections.defaultdict(list)
    for pid in range(pool):
        mods[(pid * page_bytes) % WAY].append(pid)
    out, used = [], set()
    for j in range(n):
        want = (-(j % runs) * run_bytes) % WAY
        best = min(mods, key=lambda m: (m - want) % WAY)
        cand = [p for p in mods[best] if p not in used] or [p for m in mods for p in mods[m] if p not in used]
        out.append(cand[0])
        used.add(cand[0])
    return out


def own_mode_step(ids):
    steps = collections.Counter(ids[j + 1] - ids[j] for j in range(len(ids) - 1))
    return steps.most_common(1)[0][1] if steps else 0


def pairs_at(s, count=104):
    """count pairs whose slowdown is exactly s (contiguous 1,000,000 ns, paged (1 + s) of it)."""
    return [(1_000_000.0 * (1.0 + s), 1_000_000.0, 7, 7) for _ in range(count)]


class RowRule(unittest.TestCase):
    def test_79_12_exact_bar(self):
        self.assertEqual(rows.row_verdict([0.05] * 5), "PASS")
        self.assertEqual(rows.row_verdict([math.nextafter(0.05, math.inf)] * 5), "FAIL")
        self.assertEqual(rows.row_verdict([0.04] * 5), "PASS")
        self.assertEqual(rows.row_verdict([0.06] * 5), "FAIL")

    def test_79_12_mixed(self):
        # largest > 0.05 but the median <= 0.05: neither PASS nor FAIL
        self.assertEqual(rows.row_verdict([0.04, 0.04, 0.04, 0.06, 0.06]), "NO RESULT")
        # the median > 0.05: FAIL, whatever the smallest
        self.assertEqual(rows.row_verdict([0.06, 0.06, 0.06, 0.00, 0.00]), "FAIL")
        # the median exactly at the bar with a larger maximum: neither PASS (the max is above) nor FAIL (the median is
        # not above). Kills "FAIL uses median >= 0.05" (T-3198 §15's survivor; T-3189 §11)
        self.assertEqual(rows.row_verdict([0.05, 0.05, 0.05, 0.06, 0.06]), "NO RESULT")
        with self.assertRaises(rows.Refusal):
            rows.row_verdict([0.0] * 4)

    def test_slowdown_is_the_time_ratio(self):
        self.assertAlmostEqual(rows.slowdown(105.0, 100.0), 0.05)
        self.assertAlmostEqual(rows.slowdown(100.0, 100.0), 0.0)

    def test_process_statistic_is_the_median(self):
        rng = random.Random(79)
        values = [rng.uniform(-0.03, 0.08) for _ in range(104)]
        pairs = [(1e6 * (1 + v), 1e6, 3, 3) for v in values]
        self.assertAlmostEqual(rows.process_statistic(pairs), sorted(values)[51] / 2 + sorted(values)[52] / 2, places=9)
        self.assertAlmostEqual(rows.process_statistic(pairs_at(0.02)), 0.02, places=9)

    def test_79_9_incomplete_process(self):
        for count in (103, 105, 0):
            with self.assertRaises(rows.Refusal):
                rows.process_statistic(pairs_at(0.0, count))

    def test_79_8_token_mismatch(self):
        bad = pairs_at(0.0)
        bad[57] = (1e6, 1e6, 7, 8)
        with self.assertRaises(rows.Refusal):
            rows.process_statistic(bad)

    def test_nonfinite_or_nonpositive_duration(self):
        for broken in ((math.nan, 1e6, 1, 1), (1e6, 0.0, 1, 1), (-1.0, 1e6, 1, 1)):
            bad = pairs_at(0.0)
            bad[3] = broken
            with self.assertRaises(rows.Refusal):
                rows.process_statistic(bad)

    def test_constants(self):
        self.assertEqual((rows.PAIRS_PER_PROCESS, rows.PROCESSES_PER_ROW, rows.WARMUP_TOKENS, rows.BAR), (104, 5, 4, 0.05))
        self.assertEqual(str(rows.LOCK_PATH), r"D:\_scratch\SuperSLM\pkv-c6-timing.lock")


class Guard(unittest.TestCase):
    def test_bound_table_both_page_sizes(self):
        self.assertEqual({k: (v["n"], v["P"], v["m99"]) for k, v in rows.ROWS.items()}, PRODUCTION)
        self.assertEqual({k: (v["n"], v["P"], v["m99"]) for k, v in rows.MR_OPT_ROWS.items()}, MR_OPT)
        for slot, (n, pool, m99) in PRODUCTION.items():
            self.assertEqual(rows.guard_bound(slot, 64, n), m99)
        for slot, (n, pool, m99) in MR_OPT.items():
            self.assertEqual(rows.guard_bound(slot, 16, n), m99)

    def test_mode_step_definition(self):
        for ids in ([0, 1, 2, 3], [5, 3, 9, 7, 13], [4, 2, 0, 9, 7, 5], list(range(40, 0, -3)), [1]):
            self.assertEqual(rows.mode_step(ids), own_mode_step(ids))

    def vitality(self, table, page_positions):
        for slot, (n, pool, m99) in table.items():
            runs, dpe = GEOMETRY[slot]
            run_bytes = page_positions * dpe
            orders = {"shuffle": proto_shuffle(pool)[:n], "ascending": list(range(n)), "pair churn": pair_churn(pool)[:n],
                      "interleave-5": interleave(pool)[:n],
                      "set-concentrating": set_concentrating(n, runs, run_bytes, runs * run_bytes, 4 * pool)}
            for name, ids in orders.items():
                with self.subTest(slot=slot, B=page_positions, order=name):
                    self.assertEqual(rows.guard_admits(slot, page_positions, ids), name == "shuffle",
                                     f"M = {own_mode_step(ids)}, M99 = {m99}")

    def test_vitality_at_b64(self):
        self.vitality(PRODUCTION, 64)

    def test_vitality_at_b16(self):
        self.vitality(MR_OPT, 16)

    def test_bound_is_inclusive(self):
        # n ids whose most common step occurs exactly k times: k steps of +1, the rest distinct and larger
        def with_mode(n, k):
            steps = [1] * k + list(range(2, 2 + n - 1 - k))
            ids = [0]
            for s in steps:
                ids.append(ids[-1] + s)
            return ids
        for table, b in ((PRODUCTION, 64), (MR_OPT, 16)):
            for slot, (n, pool, m99) in table.items():
                with self.subTest(slot=slot, B=b):
                    self.assertEqual(own_mode_step(with_mode(n, m99)), m99)
                    self.assertTrue(rows.guard_admits(slot, b, with_mode(n, m99)))
                    self.assertFalse(rows.guard_admits(slot, b, with_mode(n, m99 + 1)))

    def test_five_lists_agree(self):
        ids = proto_shuffle(64)[:64]
        self.assertTrue(rows.id_lists_agree([ids] * 5))
        other = list(ids)
        other[3], other[4] = other[4], other[3]
        self.assertFalse(rows.id_lists_agree([ids] * 4 + [other]))


def sha(path):
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def row_inputs(root):
    """A model file and a two-piece blob chain with its manifest (restore 1,200), healthy. Returns (model, manifest,
    manifest data)."""
    model = root / "model.sslm"
    model.write_bytes(b"model bytes")
    prev, at, pieces = "-", 0, []
    for i, count in enumerate((700, 500)):
        f = root / f"piece{i}.ssb"
        f.write_bytes(f"blob after {at + count}".encode())
        pieces.append({"file": f.name, "start": at, "count": count, "input_sha256": prev, "sha256": sha(f)})
        prev, at = sha(f), at + count
    data = {"complete": True, "model_sha256": sha(model), "stream": {"seed": 79, "restore": 1200},
            "pieces": pieces, "final_blob": pieces[-1]["file"], "final_sha256": pieces[-1]["sha256"]}
    manifest = root / "manifest.json"
    manifest.write_text(json.dumps(data))
    return model, manifest, data


class Refusals(unittest.TestCase):
    """79-6, 79-7 and the per-process refusal rule (cell 7.9, rev 16.23)."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3189-r9-refusals-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.model, self.manifest, self.data = row_inputs(self.root)

    def write(self, data):
        self.data = data
        self.manifest.write_text(json.dumps(data))

    def test_79_6_healthy_blob_accepted(self):
        blob, digest = rows.check_row_blob(self.manifest, self.model, 1200)
        self.assertEqual(Path(blob).name, "piece1.ssb")
        self.assertEqual(digest, sha(self.manifest))

    def test_79_6_each_mismatch_refused(self):
        cases = {
            "blob digest": lambda: (self.root / "piece1.ssb").write_bytes(b"changed blob"),
            "chain": lambda: self.write({**self.data, "pieces": [self.data["pieces"][0],
                                                                  {**self.data["pieces"][1], "input_sha256": "0" * 64}]}),
            "model binding": lambda: self.model.write_bytes(b"another model"),
            "stream": lambda: self.write({**self.data, "stream": {"seed": 79, "restore": 1204}}),
            "torn manifest": lambda: self.manifest.write_text('{"complete": tr'),
            "final digest alone": lambda: self.write({**self.data, "final_sha256": "0" * 64}),
        }
        for name, damage in cases.items():
            with self.subTest(case=name):
                self.setUp()
                damage()
                with self.assertRaises(rows.Refusal):
                    rows.check_row_blob(self.manifest, self.model, 1200)

    def test_79_7_seam_build_refused(self):
        self.assertIsNone(rows.check_build_identity("PKV_PAIR_PRODUCTION_V2 defs=none"))
        for line in ("PKV_PAIR_PRODUCTION_V2 defs=PKV_C6_IDENTITY_PROBE", "PKV_PAIR_PRODUCTION_V2 defs=SUPERSLM_ENABLE_KV_PAGES_TEST_SEAMS", ""):
            with self.subTest(line=line):
                with self.assertRaises(rows.Refusal):
                    rows.check_build_identity(line)

    def test_exit_codes_distinct(self):
        self.assertEqual(set(rows.EXIT_CODES), {"blob", "seam", "token", "pairs"})
        self.assertEqual(len(set(rows.EXIT_CODES.values())), 4)
        self.assertNotIn(0, rows.EXIT_CODES.values())

    def test_row_graded_only_on_five_complete_processes(self):
        ok = [{"status": "ok", "median": 0.01}] * 5
        graded = rows.grade_row(ok)
        self.assertTrue(graded["graded"])
        self.assertEqual(graded["verdict"], "PASS")
        self.assertIsNotNone(graded["record"])
        for kind in ("blob", "seam", "token", "pairs"):
            with self.subTest(kind=kind):
                bad = list(ok)
                bad[2] = {"status": "refused", "kind": kind}
                g = rows.grade_row(bad)
                self.assertEqual((g["graded"], g["verdict"], g["record"]), (False, None, None))
                self.assertEqual(g["exit_code"], rows.EXIT_CODES[kind])
        g = rows.grade_row(ok[:4])  # never graded on four
        self.assertEqual((g["graded"], g["verdict"]), (False, None))


# The stand-in pair executable: prints the identity line, 104 timed pairs (slowdown 1%, tokens equal) and the
# measured holder's ids; PKV_STANDIN_INJECT="<kind>@<k>" injects one refusal into process k. A "blob" injection
# changes the blob's bytes at the end of process k - 1, so process k's pre-restore check is what must see it.
STANDIN = r"""
import argparse, json, os, sys
from pathlib import Path
ap = argparse.ArgumentParser()
for flag in ("--blob", "--model", "--warmup"):
    ap.add_argument(flag)
ap.add_argument("--process", type=int)
ap.add_argument("--pairs", type=int)
a = ap.parse_args()
kind, _, at = os.environ.get("PKV_STANDIN_INJECT", "").partition("@")
at = int(at) if at else 0
with open(os.environ["PKV_STANDIN_LOG"], "a") as log:
    log.write(f"{a.process}\n")
mine = a.process == at
print("PKV_PAIR_PRODUCTION_V2 defs=PKV_C6_IDENTITY_PROBE" if kind in ("seam", "crash-seam") and mine else "PKV_PAIR_PRODUCTION_V2 defs=none")
if os.environ.get("PKV_STANDIN_RESERVE"):
    print(os.environ["PKV_STANDIN_RESERVE"])
for i in range(a.pairs - 1 if kind == "pairs" and mine else a.pairs):
    print(f"PAIR 1010000 1000000 7 {8 if kind == 'token' and mine and i == 57 else 7}")
print("IDS " + " ".join(str(i) for i in json.loads(os.environ["PKV_STANDIN_IDS"])))
if kind == "blob" and a.process == at - 1:
    Path(a.blob).write_bytes(Path(a.blob).read_bytes() + b"changed")
if mine and kind in ("crash", "crash-seam"):
    sys.exit(5)
once = Path(os.environ["PKV_STANDIN_LOG"] + ".once")
if mine and kind == "crash-once" and not once.exists():
    once.write_text("crashed")
    sys.exit(5)
"""


class Loop(unittest.TestCase):
    """79-6 to 79-9 through the driver's process loop, as a process (T-3196 F-6)."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3189-r9-loop-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.model, self.manifest, _ = row_inputs(self.root)
        self.standin = self.root / "standin_pair.py"
        self.standin.write_text(STANDIN)
        self.log = self.root / "standin.log"
        self.out = self.root / "out"
        self.out.mkdir()
        self.result = self.out / "R2a-run1.json"

    def run_row(self, inject="", ids=None, reserve="", pair_cmd=None):
        env = dict(os.environ, PKV_STANDIN_INJECT=inject, PKV_STANDIN_LOG=str(self.log),
                   PKV_STANDIN_IDS=json.dumps(ids if ids is not None else proto_shuffle(64)[:21]),
                   PKV_STANDIN_RESERVE=reserve)
        cmd = [sys.executable, "-I", str(SCRIPT), "--pair-cmd", json.dumps(pair_cmd or [sys.executable, "-I", str(self.standin)]),
               "--manifest", str(self.manifest), "--model", str(self.model), "--restore", "1200", "--slot", "R2a",
               "--result", str(self.result), "--lock", str(self.root / "pkv-c6-timing.lock")]
        done = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=300)
        ran = [int(x) for x in self.log.read_text().split()] if self.log.exists() else []
        return done, ran

    def verdicts_written(self):
        found = []
        for f in self.out.rglob("*"):
            if f.is_file():
                try:
                    data = json.loads(f.read_text())
                except ValueError:
                    continue
                if isinstance(data, dict) and data.get("verdict") is not None:
                    found.append(f.name)
        return found

    def test_loop_healthy_row_is_graded(self):
        done, ran = self.run_row()
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(ran, [1, 2, 3, 4, 5])
        self.assertEqual((rows.read_record(self.result) or {}).get("verdict"), "PASS")
        for k in range(1, 6):
            self.assertIsNotNone(rows.read_record(self.out / f"R2a-run1.p{k}.json"), f"process {k}'s record")

    def test_loop_each_refusal_in_process_3(self):
        for kind in ("blob", "seam", "token", "pairs"):
            with self.subTest(kind=kind):
                self.setUp()
                done, ran = self.run_row(f"{kind}@3")
                self.assertEqual(done.returncode, rows.EXIT_CODES.get(kind), f"{kind}: exit {done.returncode}")
                self.assertNotEqual(done.returncode, 0)
                self.assertFalse(self.result.exists(), f"{kind}: a row record was written")
                self.assertFalse((self.out / "R2a-run1.p3.json").exists(), f"{kind}: the refused process wrote a record")
                self.assertEqual(self.verdicts_written(), [], f"{kind}: a verdict was written")
                # processes 1 and 2 completed on a blob checked before each restore: their records stand
                for k in (1, 2):
                    self.assertIsNotNone(rows.read_record(self.out / f"R2a-run1.p{k}.json"),
                                         f"{kind}: process {k}'s record (a check after the restore refuses process 2)")
                if kind == "blob":
                    # checked before process 3 restores: process 3 never ran
                    self.assertEqual(ran, [1, 2], "the blob check ran after the restore, or not per process")
                else:
                    self.assertIn(3, ran)


# ---- T-3202 (code review) remedies, SuperSLM 1478be7: one pin cell per landed remedy. Each fails with that remedy
# reverted (the test author's record, T-3189 §12, runs each revert) and passes on the built driver.
RESERVE_LINE = "RESERVE seed=1 singles=63 source=tests/paged-kv/pkv_scatter_reserve.h"


class T6Remedies(unittest.TestCase):
    setUp = Loop.setUp
    run_row = Loop.run_row

    def record(self, path=None):
        return json.loads((path or self.result).read_text(encoding="utf-8"))

    # F-1 (D-SLM8461): the row record carries status STANDING and the driver's own hash of the pair executable,
    # the verdict in its own field; every process record carries the hash too.
    def test_f1_row_record_status_and_exe_sha256(self):
        done, _ = self.run_row()
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        rec = self.record()
        exe = Path(sys.executable).resolve()
        self.assertEqual(rec.get("status"), "STANDING")
        self.assertEqual(rec.get("verdict"), "PASS")
        self.assertEqual(rec.get("exe_sha256"), sha(exe))
        self.assertEqual(Path(rec.get("exe", "")).resolve(), exe)
        for k in range(1, 6):
            self.assertEqual(self.record(self.out / f"R2a-run1.p{k}.json").get("exe_sha256"), sha(exe), f"process {k}")

    # F-1 (D-SLM8461): a NO RESULT row stands as a reading of no result.
    def test_f1_no_result_row_stands(self):
        done, _ = self.run_row(ids=list(range(21)))  # ascending: the guard rejects, so the row reads NO RESULT
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        rec = self.record()
        self.assertEqual((rec.get("status"), rec.get("verdict")), ("STANDING", "NO RESULT"))

    # F-1: a pair executable that does not exist refuses (exit 70) before any run, writing nothing.
    def test_f1_missing_pair_executable_refuses(self):
        done, ran = self.run_row(pair_cmd=[str(self.root / "no-such-pair.exe")])
        self.assertEqual(done.returncode, 70, done.stderr[-2000:])
        self.assertEqual(ran, [])
        self.assertFalse(self.result.exists())

    # F-12 (with D-SLM8461): a refused run replaces an earlier row record at its path with a REFUSED record.
    def test_f12_refused_run_supersedes_an_existing_record(self):
        self.assertEqual(self.run_row()[0].returncode, 0)
        self.assertEqual(self.record().get("status"), "STANDING")
        self.log.unlink()
        done, _ = self.run_row("seam@3")
        self.assertEqual(done.returncode, 62, done.stderr[-2000:])
        rec = self.record()
        self.assertEqual((rec.get("status"), rec.get("exit_code"), rec.get("verdict")), ("REFUSED", 62, None))
        self.assertIn("superseded", rec)
        self.assertEqual(rec.get("exe_sha256"), sha(Path(sys.executable).resolve()))

    # F-12: the rerun limit is a refusal, so it supersedes too.
    def test_f12_rerun_limit_supersedes(self):
        self.assertEqual(self.run_row()[0].returncode, 0)
        self.log.unlink()
        done, ran = self.run_row("crash@3")
        self.assertEqual(done.returncode, 70, done.stderr[-2000:])
        self.assertEqual(ran.count(3), 3)
        rec = self.record()
        self.assertEqual((rec.get("status"), rec.get("exit_code"), rec.get("verdict")), ("REFUSED", 70, None))

    # F-2: the exit status is read before the identity. A process that prints a seam identity and then crashes is
    # rerun whole (three attempts, then 70), never graded 62; a transient crash is rerun and the row is graded.
    def test_f2_crash_after_a_seam_identity_is_rerun_not_graded(self):
        done, ran = self.run_row("crash-seam@3")
        self.assertEqual(done.returncode, 70, done.stderr[-2000:])
        self.assertEqual(ran.count(3), 3)

    def test_f2_transient_crash_is_rerun(self):
        done, ran = self.run_row("crash-once@3")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(ran.count(3), 2)
        self.assertEqual(self.record().get("status"), "STANDING")

    # F-11: the RESERVE line is recorded in every process record and in the row record; absent, it records null.
    def test_f11_reserve_recorded(self):
        done, _ = self.run_row(reserve=RESERVE_LINE)
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        want = {"seed": 1, "singles": 63, "source": "tests/paged-kv/pkv_scatter_reserve.h"}
        for k in range(1, 6):
            self.assertEqual(self.record(self.out / f"R2a-run1.p{k}.json").get("reserve"), want, f"process {k}")
        self.assertEqual(self.record().get("reserve"), want)
        self.setUp()
        self.assertEqual(self.run_row()[0].returncode, 0)
        rec = self.record()
        self.assertIn("reserve", rec)
        self.assertIsNone(rec["reserve"])

    # F-9: the lock is taken by an OS byte lock before the owner is read. A runner that holds the byte lock while the
    # content still names a dead owner (mid-recovery) refuses a second runner, which must not also recover.
    def test_f9_os_lock_holder_refuses_a_second_recovery(self):
        lock = self.root / "pkv-c6-timing.lock"
        dead = subprocess.Popen([sys.executable, "-I", "-c", "pass"])
        dead.wait()
        holder_src = (
            "import msvcrt, os, sys, time\n"
            "fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT | os.O_BINARY, 0o600)\n"
            "os.write(fd, ('{\"pid\": %s}' % sys.argv[2]).encode())\n"
            "os.lseek(fd, 1 << 20, os.SEEK_SET)\n"
            "msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)\n"
            "print('held', flush=True)\n"
            "time.sleep(60)\n") if os.name == "nt" else (
            "import fcntl, os, sys, time\n"
            "fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o600)\n"
            "os.write(fd, ('{\"pid\": %s}' % sys.argv[2]).encode())\n"
            "fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, 1 << 20)\n"
            "print('held', flush=True)\n"
            "time.sleep(60)\n")
        holder = subprocess.Popen([sys.executable, "-I", "-c", holder_src, str(lock), str(dead.pid)],
                                  stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (holder.kill(), holder.wait(), holder.stdout.close()))
        self.assertEqual(holder.stdout.readline().strip(), "held")
        with self.assertRaises(rows.Refusal):
            with rows.timing_lock(self.root / "b.json", lock_path=lock):
                pass


class Process(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3189-rows-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_boot(self):
        self.assertTrue(rows.same_boot("20261009081500.000000+000", "20261009081500.000000+000"))
        self.assertFalse(rows.same_boot("20261009081500.000000+000", "20261009093000.000000+000"))

    def test_79_10_second_runner_refused_with_another_result_path(self):
        lock = self.root / "pkv-c6-timing.lock"
        holder = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(30)"])
        self.addCleanup(holder.kill)
        lock.write_text(json.dumps({"pid": holder.pid}))
        with self.assertRaises(rows.Refusal):
            with rows.timing_lock(self.root / "b.json", lock_path=lock):
                pass

    def test_79_10_dead_owner_recovered(self):
        lock = self.root / "pkv-c6-timing.lock"
        dead = subprocess.Popen([sys.executable, "-I", "-c", "pass"])
        dead.wait()
        lock.write_text(json.dumps({"pid": dead.pid}))
        with rows.timing_lock(self.root / "a.json", lock_path=lock):
            self.assertTrue(lock.exists())

    def test_79_11_torn_record_is_missing(self):
        path = self.root / "r.json"
        rows.write_record(path, {"row": "R4a", "medians": [0.01] * 5})
        self.assertEqual(rows.read_record(path)["row"], "R4a")
        path.write_text('{"row": "R4a", "medi')
        self.assertIsNone(rows.read_record(path))
        self.assertIsNone(rows.read_record(self.root / "missing.json"))


if __name__ == "__main__":
    unittest.main()
