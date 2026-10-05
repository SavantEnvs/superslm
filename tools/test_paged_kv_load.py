"""Schedule and short process smoke for the four C6 load forms."""
import json
from pathlib import Path
import tempfile
import unittest

import paged_kv_load as load

class LoadCells(unittest.TestCase):
    def test_four_forms_and_50_percent_duty(self):
        for form, period in load.FORMS.items():
            events = load.schedule(form, period * 3, 79)
            self.assertEqual(events[:4], [(0.0, True), (period / 2, False),
                                          (period, True), (period * 1.5, False)])
            self.assertGreaterEqual(len(events), 5)
        a = load.schedule("random-dwell", 8.0, 79)
        self.assertEqual(a, load.schedule("random-dwell", 8.0, 79))
        self.assertTrue(all(a[i][0] < a[i+1][0] for i in range(len(a)-1)))
        self.assertTrue(all(a[i][1] != a[i+1][1] for i in range(len(a)-1)))

    def test_short_process_record(self):
        with tempfile.TemporaryDirectory(prefix="pkv-load-") as folder:
            record = Path(folder) / "load.json"
            result = load.run("square-0.1", .15, 1, 79, record)
            self.assertEqual(json.loads(record.read_text())["form"], "square-0.1")
            self.assertEqual(result["worker_exits"], [0])
            self.assertGreaterEqual(result["elapsed_s"], .15)

if __name__ == "__main__":
    unittest.main()
