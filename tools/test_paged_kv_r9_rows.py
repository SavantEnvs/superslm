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

SCRIPT = Path(__file__).with_name("run_paged_kv_rows.py")
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


class Refusals(unittest.TestCase):
    """79-6, 79-7 and the per-process refusal rule (cell 7.9, rev 16.23)."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3189-r9-refusals-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.model = self.root / "model.sslm"
        self.model.write_bytes(b"model bytes")
        prev, at, pieces = "-", 0, []
        for i, count in enumerate((700, 500)):
            f = self.root / f"piece{i}.ssb"
            f.write_bytes(f"blob after {at + count}".encode())
            pieces.append({"file": f.name, "start": at, "count": count, "input_sha256": prev, "sha256": sha(f)})
            prev, at = sha(f), at + count
        self.manifest = self.root / "manifest.json"
        self.write({"complete": True, "model_sha256": sha(self.model), "stream": {"seed": 79, "restore": 1200},
                    "pieces": pieces, "final_blob": pieces[-1]["file"], "final_sha256": pieces[-1]["sha256"]})

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
