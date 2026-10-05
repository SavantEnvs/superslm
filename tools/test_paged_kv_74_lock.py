"""C-4: the direct 7.4 lock recovers a dead owner and refuses a live one."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


BUILD = Path(os.environ.get("PKV_C6_BUILD", "D:/_scratch/T-3114/build-c6-opt"))
FIXTURES = Path(os.environ.get("PKV_C6_FIXTURES", "D:/_scratch/T-3112/fixtures"))
EXE = BUILD / "superslm_pkv_c6.exe"


class Lock74Cells(unittest.TestCase):
    def run_lock(self, path: Path):
        env = os.environ.copy()
        env["PKV_DEBUNK_74_TEST_LOCK_PATH"] = str(path)
        return subprocess.run([str(EXE), "=7.4t/C6:lock"], env=env,
                              capture_output=True, text=True, timeout=15)

    def test_dead_owner_reclaimed_and_live_owner_refused_separately_from_geometry(self):
        with tempfile.TemporaryDirectory(prefix="pkv-74-lock-") as folder:
            lock = Path(folder) / "timing.lock"
            lock.write_text(str(os.getpid()), encoding="ascii")
            live = self.run_lock(lock)
            self.assertEqual(live.returncode, 77, live.stdout + live.stderr)
            self.assertIn("already held", live.stderr)
            self.assertEqual(lock.read_text(encoding="ascii"), str(os.getpid()))

            dead = subprocess.Popen([sys.executable, "-c", "pass"])
            dead.wait(timeout=10)
            lock.write_text(str(dead.pid), encoding="ascii")
            reclaimed = self.run_lock(lock)
            self.assertEqual(reclaimed.returncode, 0, reclaimed.stdout + reclaimed.stderr)
            self.assertFalse(lock.exists(), "successful lock owner did not release at exit")

            lock.write_text("", encoding="ascii")
            torn = self.run_lock(lock)
            self.assertEqual(torn.returncode, 77, torn.stdout + torn.stderr)
            self.assertIn("incomplete", torn.stderr)

        env = os.environ.copy()
        env["SUPERSLM_PAGED_KV_FIXTURE_DIR"] = str(FIXTURES)
        env["SUPERSLM_PAGED_KV_74_FIXTURE_LARGE"] = "pkv_qk"
        geometry = subprocess.run([str(EXE), "=7.4/C6:fixtures"], env=env,
                                  capture_output=True, text=True, timeout=15)
        self.assertEqual(geometry.returncode, 71, geometry.stdout + geometry.stderr)
        self.assertIn("geometry mismatch", geometry.stderr)


if __name__ == "__main__":
    unittest.main()
