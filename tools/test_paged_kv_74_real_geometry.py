"""Permanent P-5 pin for the real-artifact 7.4 geometry comparator."""

import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get(
    "PKV_C6_SOURCE", ROOT / "tests/paged-kv/c6_dim7_reset_adopt_timing.cpp"))
ARTIFACTS = Path(os.environ.get(
    "PKV_C6_ARTIFACT_DIR", "D:/_artifacts/SuperSLM/t3114"))
VCVARS = Path("C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat")


def config(path):
    # Read CFG1 from the real artifact's section table, without loading weights.
    with path.open("rb") as stream:
        header = stream.read(64)
        if header[:4] != b"SSLM":
            raise AssertionError(f"{path}: bad artifact magic")
        count = struct.unpack_from("<I", header, 12)[0]
        for _ in range(count):
            row = stream.read(40)
            if struct.unpack_from("<I", row)[0] == 0:
                offset = struct.unpack_from("<Q", row, 8)[0]
                position = stream.tell()
                stream.seek(offset)
                cfg = stream.read(84)
                stream.seek(position)
                if cfg[:4] != b"CFG1":
                    raise AssertionError(f"{path}: bad CFG1 magic")
                values = struct.unpack_from("<16I", cfg, 4)
                return (values[2], values[4], values[5], values[1], values[11], values[8])
    raise AssertionError(f"{path}: no CFG1")


class RealGeometryCells(unittest.TestCase):
    def test_p5_hidden_size_and_block_size_from_real_configs(self):
        source = SOURCE.read_text(encoding="utf-8")
        function = re.search(r"bool Same74Geometry\(.*?\n}\n(?=void Run74)", source, re.S)
        self.assertIsNotNone(function, "real-artifact geometry comparator is absent")
        run = source.split("void Run74(const Fixture& low,const Fixture& large){", 1)[1]
        run = run.split("void TimerLive", 1)[0]
        self.assertIn("SslmModel::Load(low.bytes.p", run)
        self.assertIn("SslmModel::Load(large.bytes.p", run)
        self.assertIn("Same74Geometry(small_config,large_config,low.geo,large.geo)", run)

        low = config(ARTIFACTS / "qwen2.5-0.5b-instruct-cap4096-aex.sslm")
        large = config(ARTIFACTS / "qwen2.5-0.5b-instruct-cap32768-aex.sslm")
        self.assertEqual(low[:5], large[:5], "control artifacts have different model geometry")
        self.assertEqual((low[5], large[5]), (4096, 32768))
        cfg = lambda values: "{" + ",".join(map(str, values[:5])) + "}"
        claim = lambda values: "{" + ",".join(map(str, values[:3] + (values[5],))) + "}"
        cpp = ("#include <cstdint>\n#include <iostream>\n"
               "namespace superslm {struct SslmModelConfig {"
               "uint32_t num_hidden_layers,num_key_value_heads,head_dim,hidden_size,kv_block_size;};}\n"
               "struct Geometry {uint32_t layers,kv_heads,head_dim;int64_t context_cap;};\n"
               + function.group() + "\nint main(){\n"
               + f"superslm::SslmModelConfig a={cfg(low)},b={cfg(large)};\n"
               + f"Geometry x={claim(low)},y={claim(large)};\n"
               + "std::cout<<Same74Geometry(a,b,x,y);\n"
               + "++b.hidden_size;std::cout<<Same74Geometry(a,b,x,y);\n"
               + "--b.hidden_size;++b.kv_block_size;std::cout<<Same74Geometry(a,b,x,y);\n}\n")
        with tempfile.TemporaryDirectory(prefix="pkv-p5-") as temp:
            folder = Path(temp)
            (folder / "pin.cpp").write_text(cpp, encoding="utf-8")
            (folder / "build.bat").write_text(
                f'@echo off\ncall "{VCVARS}" >nul 2>&1\n'
                f'cl /nologo /EHsc /Fe"{folder / "pin.exe"}" "{folder / "pin.cpp"}"\n',
                encoding="ascii")
            built = subprocess.run(["cmd", "/c", str(folder / "build.bat")],
                                   cwd=folder, capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            checked = subprocess.run([str(folder / "pin.exe")], capture_output=True,
                                     text=True, timeout=15)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertEqual(checked.stdout, "100", "equal / hidden-only / block-only")


if __name__ == "__main__":
    unittest.main()
