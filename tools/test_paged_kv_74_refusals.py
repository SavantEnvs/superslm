"""Executed subprocess constructions for 7.4 infrastructure refusals."""
import os
from pathlib import Path
import subprocess
import unittest

BUILD = Path(os.environ.get("PKV_C6_BUILD", "D:/_scratch/T-3112/build-c6"))
FIXTURES = Path(os.environ.get("PKV_C6_FIXTURES", "D:/_scratch/T-3112/fixtures"))

class RefusalCells(unittest.TestCase):
    def call(self, binary="superslm_pkv_c6.exe", cell="7.4/C6:fixtures", **settings):
        env = os.environ.copy()
        env["SUPERSLM_PAGED_KV_FIXTURE_DIR"] = str(FIXTURES)
        for key, value in settings.items():
            env["SUPERSLM_PAGED_KV_74_" + key] = value
        return subprocess.run([str(BUILD / binary), "=" + cell], env=env,
                              capture_output=True, text=True, timeout=90)

    def check_refusal(self, code, message, **settings):
        for attempt in range(5 if code in (74, 75) else 1):
            result = self.call(**settings)
            if code in (74, 75) and result.returncode == 76:
                continue  # timer vitality precedes these deliberate refusals
            self.assertEqual(result.returncode, code, result.stdout + result.stderr)
            self.assertIn(message, result.stderr)
            return
        self.fail(f"timer vitality refused every attempt before infrastructure {code}")

    def test_74_5_geometry_refuses_before_cap(self):
        self.check_refusal(71, "geometry mismatch", FIXTURE_LARGE="pkv_qk")

    def test_74_6_wrong_cap(self):
        self.check_refusal(72, "wrong cap", FIXTURE_LARGE="pkv_odd")

    def test_74_7_cpuid_and_pin_refusals(self):
        result = self.call(binary="superslm_pkv_74_no_tsc.exe")
        self.assertEqual(result.returncode, 73, result.stderr)
        self.assertIn("invariant TSC absent", result.stderr)
        self.check_refusal(73, "cannot pin timing thread", FORCE_PIN_FAILURE="1")

    def test_74_8_corrected_median_floor(self):
        self.check_refusal(74, "corrected median under 100 ticks", FORCE_FLOOR="1")

    def test_timer_vitality_and_unresolved_refusals(self):
        self.check_refusal(76, "fixed-L timer vitality failed", FORCE_VITALITY_FAILURE="1")
        self.check_refusal(75, "sign interval did not resolve", FORCE_UNRESOLVED="1")

    def test_4t_live_no_subtraction_mutant(self):
        healthy = [self.call(cell="7.4t/C6:live") for _ in range(20)]
        passed = sum(result.returncode == 0 for result in healthy)
        print(f"7.4t fixed-L live mechanism: {passed}/20 healthy accepted", flush=True)
        self.assertGreaterEqual(passed, 19, f"healthy fixed-L clock accepted only {passed}/20")
        mutant = self.call(binary="superslm_pkv_74_no_subtraction.exe", cell="7.4t/C6:live")
        self.assertEqual(mutant.returncode, 1, mutant.stdout + mutant.stderr)
        self.assertIn("clock.okay", mutant.stderr)

    def test_74_2_and_74_3_per_call_reset_and_adopt_mutants(self):
        for verb in ("ADOPT", "RESET"):
            with self.subTest(verb=verb):
                for attempt in range(3):
                    result = self.call(**{verb + "_MUTANT": "1"})
                    if result.returncode == 76:  # independent timer vitality refusal
                        continue
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    for holder in ("budget", "whole_reserve"):
                        self.assertRegex(result.stdout, rf"7\.4 {holder} {verb.lower()} r\(86\)=[0-9.]+ r\(120\)=[0-9.]+ bound=2\.0 FAIL")
                    break
                else:
                    self.fail(f"{verb} hook never reached a graded fixture call in three attempts")

if __name__ == "__main__":
    unittest.main()
