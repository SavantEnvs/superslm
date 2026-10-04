"""C6 7.9/7.9b construction and refusal cells on synthetic pair records."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("run_paged_kv_pairs.py")
spec = importlib.util.spec_from_file_location("paged_kv_pairs", SCRIPT)
pair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair)


class PairCells(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3112-pairs-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def rows(self, count=508, token_mismatch=False):
        path = self.root / "pairs.csv"
        path.write_text("".join(f"{i},{'A' if i % 2 else 'B'},100,100,12,{13 if token_mismatch and i == 0 else 12}\n"
                                for i in range(count)))
        return path

    def test_79_and_79b_must_accept_and_reject_statistic(self):
        healthy = pair.grade([0.0] * 508)
        self.assertEqual((healthy["effect"], healthy["bar"]), ("NO RESULT", "PASS"))
        mutant = pair.grade([.15] * 508)
        self.assertEqual((mutant["effect"], mutant["bar"]), ("RESOLVED", "FAIL"))
        self.assertEqual(mutant["sign_interval"], [.15, .15])
        self.assertEqual(pair.grade([0.0] * 2)["status"], "MECHANISM_ONLY")

    def test_79_6_and_79b_5_corrupt_manifest_or_blob_refused(self):
        model = self.root / "model.sslm"
        model.write_bytes(b"model")
        blob = self.root / "piece.blob"
        blob.write_bytes(b"blob")
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps({"complete": True, "model_sha256": pair.sha(model),
            "stream": {"seed": 79, "width": 530, "prompt_tokens": 18},
            "pieces": [{"file": blob.name, "start": 0, "count": 18, "input_sha256": "-", "sha256": pair.sha(blob)}],
            "final_blob": blob.name, "final_sha256": pair.sha(blob)}))
        self.assertEqual(pair.checked_blob(manifest, model, 530)[0], blob)
        blob.write_bytes(b"flipped")
        with self.assertRaisesRegex(pair.Refusal, "mismatch"):
            pair.checked_blob(manifest, model, 530)
        manifest.write_text('{"complete":')
        with self.assertRaisesRegex(pair.Refusal, "torn"):
            pair.checked_blob(manifest, model, 530)

    def test_79_7_and_79b_6_seam_executable_refused(self):
        seam = self.root / "superslm_test_injection.exe"
        seam.write_bytes(b"binary")
        with self.assertRaisesRegex(pair.Refusal, "production"):
            pair.production_library(seam)

    def test_79_8_and_79b_7_token_mismatch_refused(self):
        with self.assertRaisesRegex(pair.Refusal, "token-stream mismatch"):
            pair.parse_pairs(self.rows(token_mismatch=True), 508)

    def test_79_9_and_79b_8_incomplete_pairs_refused(self):
        with self.assertRaisesRegex(pair.Refusal, "incomplete pair set"):
            pair.parse_pairs(self.rows(507), 508)

    def test_79_10_and_79b_9_live_locks_refused(self):
        for name in ("79", "79b"):
            lock = self.root / f"{name}.lock"
            with pair.PidLock(lock):
                with self.assertRaisesRegex(pair.Refusal, "second runner"):
                    with pair.PidLock(lock):
                        pass

    def test_79_11_and_79b_10_torn_results_are_missing(self):
        for name in ("79", "79b"):
            result = self.root / f"{name}.json"
            result.write_text('{"complete":')
            self.assertIsNone(pair.load_record(result))
            result.write_text(json.dumps({"complete": True, "kind": name}))
            self.assertIsNone(pair.load_record(result))


if __name__ == "__main__":
    unittest.main()
