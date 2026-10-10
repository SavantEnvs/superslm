"""Paged-KV plan rev 16.21 cell 7.9 (§23.8 step T4): the five-process 7.9 driver.

One model, one blob per row restored into both arms (a legacy holder in a page pool at the derived B, rotated,
and a legacy holder in the same build's contiguous pool option), 104 timed pairs per process after 4 warm-up
tokens, five processes on one boot. The process statistic is the median of s_i = tau_paged / tau_contiguous - 1;
the row rule: PASS when the largest of the five process medians is <= 0.05, FAIL when their median is > 0.05,
NO RESULT otherwise. A row is graded only on five complete processes, only when their five measured-holder id
lists agree, and only when the scatter guard admits those ids (M <= M99(n, P) at the n read after the run).

Every refusal under the per-process rule has its own exit code, writes no record for the refused process and no
row record, and leaves the row ungraded: 79-6 a blob or manifest mismatch (checked before each process restores
the blob), 79-7 a seam-library pair executable (its identity line, checked once per process before any pair is
read), 79-8 a token mismatch between the arms, 79-9 a process with other than 104 timed pairs. A process whose
boot changed, or whose pair executable crashed, is discarded and rerun whole. The box-global C6 timing lock is
held from the first process until the row record is published.

Command line (main):
    run_paged_kv_rows.py --pair-cmd JSON --manifest M --model F --restore N --slot S --result R
                         [--lock L] [--page-positions 64|16]
--page-positions selects the guard's bound table: 64 for the production rows, 16 for 7.9-MR-OPT's (slot R79MR,
whose rows are R2b, R4a and R4b; pass the row as --slot and --page-positions 16).
"""

from __future__ import annotations

import argparse
import collections
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time


class Refusal(RuntimeError):
    """A construction or input the driver refuses (cell 7.9's refusal vitality, 79-6 to 79-12). `kind` names the
    per-process refusal (EXIT_CODES' keys) where there is one."""

    def __init__(self, message: str, kind: str | None = None):
        super().__init__(message)
        self.kind = kind


PAIRS_PER_PROCESS = 104
PROCESSES_PER_ROW = 5
WARMUP_TOKENS = 4
BAR = 0.05
LOCK_PATH = Path(r"D:\_scratch\SuperSLM\pkv-c6-timing.lock")

# Rev 16.23 (T-3189 G-4): each refusal under the per-process rule has its own exit code, writes no timing record and
# leaves the row ungraded: 79-6 a blob or manifest mismatch, 79-7 a seam-library build, 79-8 a token mismatch, 79-9 a
# process with other than 104 timed pairs.
EXIT_CODES = {"blob": 61, "seam": 62, "token": 63, "pairs": 64}
# Not a per-process refusal: the row cannot run at all (bad arguments, the lock held by a live owner, a process that
# crashed or changed boot more than RERUN_LIMIT times, a guard input outside the table). No record is written.
EXIT_INFRASTRUCTURE = 70
RERUN_LIMIT = 3
PROCESS_CAP_SECONDS = 3600

# Cell 7.9's tables (rev 16.21; probes/scatter_guard.py): slot -> {"n": mapped pages after the run, "P": pool pages,
# "m99": the guard bound}, at B = 64.
ROWS = {
    "R2a": {"n": 21, "P": 64, "m99": 3},
    "R2c": {"n": 34, "P": 64, "m99": 4},
    "R2b": {"n": 64, "P": 64, "m99": 6},
    "R4c": {"n": 130, "P": 512, "m99": 4},
    "R4a": {"n": 250, "P": 512, "m99": 5},
    "R4b": {"n": 294, "P": 512, "m99": 5},
}
# slot -> the same at B = 16, for 7.9-MR-OPT (slot R79MR)
MR_OPT_ROWS = {
    "R2b": {"n": 256, "P": 256, "m99": 6},
    "R4a": {"n": 999, "P": 2048, "m99": 5},
    "R4b": {"n": 1175, "P": 2048, "m99": 6},
}

IDENTITY = "PKV_PAIR_PRODUCTION_V2 defs=none"


# ---- the statistic and the row rule -------------------------------------------------------------------------

def slowdown(paged_ns, contiguous_ns):
    """s_i = tau_paged / tau_contiguous - 1."""
    return float(paged_ns) / float(contiguous_ns) - 1.0


def process_statistic(pairs):
    """pairs: [(paged_ns, contiguous_ns, paged_token, contiguous_token)], exactly 104; the median s_i."""
    pairs = list(pairs)
    if len(pairs) != PAIRS_PER_PROCESS:
        raise Refusal(f"79-9: {len(pairs)} timed pairs, {PAIRS_PER_PROCESS} owed", "pairs")
    values = []
    for i, (paged, contiguous, paged_token, contiguous_token) in enumerate(pairs):
        if paged_token != contiguous_token:
            raise Refusal(f"79-8: token mismatch at pair {i}: paged {paged_token}, contiguous {contiguous_token}",
                          "token")
        if not (math.isfinite(paged) and math.isfinite(contiguous) and paged > 0 and contiguous > 0):
            raise Refusal(f"pair {i}: duration not finite and positive ({paged}, {contiguous})", "pairs")
        values.append(slowdown(paged, contiguous))
    return statistics.median(values)


def row_verdict(process_medians):
    """PASS when the largest of the five process medians is <= 0.05; FAIL when their median is > 0.05;
    NO RESULT otherwise."""
    medians = [float(m) for m in process_medians]
    if len(medians) != PROCESSES_PER_ROW or not all(math.isfinite(m) for m in medians):
        raise Refusal(f"a row is graded on {PROCESSES_PER_ROW} finite process medians, not {medians}")
    if max(medians) <= BAR:
        return "PASS"
    if statistics.median(medians) > BAR:
        return "FAIL"
    return "NO RESULT"


# ---- the scatter guard ----------------------------------------------------------------------------------------

def mode_step(ids):
    """M: the number of neighbour pairs whose step equals the most common step."""
    steps = collections.Counter(ids[j + 1] - ids[j] for j in range(len(ids) - 1))
    return steps.most_common(1)[0][1] if steps else 0


def _table(page_positions):
    if page_positions == 64:
        return ROWS
    if page_positions == 16:
        return MR_OPT_ROWS
    raise Refusal(f"no guard table at B = {page_positions} (64: production, 16: 7.9-MR-OPT)")


def guard_bound(slot, page_positions, n):
    """M99(n, P) for the row at page size 64 (production) or 16 (7.9-MR-OPT). The bound is tabulated at the n each
    row reads after its run; another n has no bound, and is refused rather than guessed."""
    table = _table(page_positions)
    if slot not in table:
        raise Refusal(f"slot {slot} has no guard row at B = {page_positions}")
    row = table[slot]
    if n != row["n"]:
        raise Refusal(f"slot {slot} at B = {page_positions}: the run read n = {n}; the bound is tabulated at "
                      f"n = {row['n']}")
    return row["m99"]


def guard_admits(slot, page_positions, ids):
    """True when the row is graded: M <= M99(n, P) at the n of `ids`."""
    return mode_step(list(ids)) <= guard_bound(slot, page_positions, len(ids))


def id_lists_agree(lists):
    """The five processes' measured-holder id lists must be equal; else the row is NO RESULT."""
    lists = [list(x) for x in lists]
    return bool(lists) and bool(lists[0]) and all(x == lists[0] for x in lists)


# ---- the box -----------------------------------------------------------------------------------------------------

def same_boot(before, after):
    """A process whose LastBootUpTime changed between its start and end is discarded and rerun."""
    return before is not None and before == after


def last_boot():
    """The box's LastBootUpTime, as WMI reports it."""
    if os.name != "nt":
        try:
            return Path("/proc/stat").read_text().split("btime", 1)[1].split()[0]
        except (OSError, IndexError):
            return None
    result = subprocess.run(["powershell", "-NoProfile", "-Command",
                             "(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')"],
                            capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def pid_live(pid):
    if pid <= 0:
        return False
    if os.name == "nt":
        result = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"], capture_output=True,
                                text=True)
        return result.returncode == 0 and re.search(rf',"{pid}"(?:,|$)', result.stdout) is not None
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def _lock_owner(lock_path):
    text = lock_path.read_text(encoding="utf-8").strip()
    try:
        value = json.loads(text)
    except ValueError:
        value = text
    if isinstance(value, dict):
        value = value.get("pid")
    return int(value)


# The OS lock that makes recovery atomic (T-3202 F-9): one byte far past any content, so a reader of the pid content
# (run_paged_kv_pairs.py's PidLock, the 7.4 and lifecycle legs) is never blocked by it.
_LOCK_BYTE = 1 << 20


def _os_lock(fd):
    """Take the exclusive, non-blocking OS byte lock on `fd`; False when another process holds it. The OS releases it
    when the holder exits, however it exits."""
    os.lseek(fd, _LOCK_BYTE, os.SEEK_SET)
    try:
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, _LOCK_BYTE)
        return True
    except OSError:
        return False


def _os_unlock(fd):
    os.lseek(fd, _LOCK_BYTE, os.SEEK_SET)
    try:
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.lockf(fd, fcntl.LOCK_UN, 1, _LOCK_BYTE)
    except OSError:
        pass


def _read_owner(fd):
    """The pid the lock's content names, None when the content is empty (a fresh or released file); raises ValueError
    on content that is neither a pid nor a {"pid": ...} object."""
    os.lseek(fd, 0, os.SEEK_SET)
    chunks = []
    while True:
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        chunks.append(chunk)
    text = b"".join(chunks).decode("utf-8").strip()
    if not text:
        return None
    try:
        value = json.loads(text)
    except ValueError:
        value = text
    if isinstance(value, dict):
        value = value.get("pid")
    return int(value)


@contextlib.contextmanager
def timing_lock(result_path, lock_path=LOCK_PATH):
    """Context manager: the box-global C6 timing pid lock, independent of the result path. A live owner refuses a
    second runner whatever its result path (79-10); a dead owner's lock is recovered, and the recovery recorded.

    Recovery is atomic (T-3202 F-9): every runner opens the lock file and takes an exclusive OS byte lock on it before
    it reads the owner, so of two runners that both find the same dead owner, exactly one holds the OS lock; the other
    is refused. Only the OS-lock holder may judge the recorded owner dead and overwrite it. The pid content stays for
    the legs that lock by exclusive create (run_paged_kv_pairs.py's PidLock): they see the file and its live pid."""
    lock_path = Path(lock_path)
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o600)
    if not _os_lock(fd):
        os.close(fd)
        raise Refusal(f"79-10: the C6 timing lock {lock_path} is held by a live runner; second runner "
                      f"(result {result_path}) refused")
    try:
        try:
            owner = _read_owner(fd)
        except (OSError, ValueError, TypeError, UnicodeDecodeError):
            raise Refusal(f"torn timing lock: {lock_path}") from None
        if owner is not None and owner != os.getpid() and pid_live(owner):
            raise Refusal(f"79-10: the C6 timing lock {lock_path} is held by live pid {owner}; second runner "
                          f"(result {result_path}) refused")
        recovered = owner if owner is not None and owner != os.getpid() else None
        content = json.dumps({"pid": os.getpid(), "result": str(result_path), "recovered_from": recovered,
                              "acquired": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}).encode("utf-8")
        os.lseek(fd, 0, os.SEEK_SET)
        os.ftruncate(fd, 0)
        os.write(fd, content)
    except BaseException:
        _os_unlock(fd)
        os.close(fd)
        raise
    try:
        yield {"recovered_from": recovered}
    finally:
        os.lseek(fd, 0, os.SEEK_SET)
        os.ftruncate(fd, 0)
        _os_unlock(fd)
        os.close(fd)
        try:
            lock_path.unlink()
        except OSError:
            pass  # another runner has it open; its content is empty or its own


# ---- records -----------------------------------------------------------------------------------------------------

def write_record(path, record):
    """Temp-then-rename."""
    path = Path(path)
    temp = path.with_name(path.name + f".{os.getpid()}.partial")
    try:
        temp.write_text(json.dumps(record, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def read_record(path):
    """The record, or None when it is missing or torn (a torn record never supplies a verdict)."""
    try:
        record = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return record if isinstance(record, dict) else None


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


# ---- 79-6 and 79-7 ------------------------------------------------------------------------------------------------

def check_row_blob(manifest_path, model_path, restore):
    """79-6: the row's blob checked against its manifest before each process restores it: the manifest chain (each
    piece's start, input digest and own digest), the model binding (the model file's SHA-256) and the stream
    (seed 79, `restore` prompt tokens), and the final blob's digest. Returns (blob path, manifest SHA-256); raises
    Refusal (exit code EXIT_CODES["blob"]) on any mismatch."""
    manifest_path, model_path = Path(manifest_path), Path(model_path)

    def refuse(why):
        raise Refusal(f"79-6: {manifest_path}: {why}", "blob")

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        refuse("missing or torn manifest")
    if not isinstance(manifest, dict) or manifest.get("complete") is not True:
        refuse("the manifest is not complete")
    if not model_path.is_file() or manifest.get("model_sha256") != sha(model_path):
        refuse("the model binding does not match the model file")
    if manifest.get("stream") != {"seed": 79, "restore": int(restore)}:
        refuse(f"the stream is {manifest.get('stream')}, seed 79 and restore {restore} owed")
    pieces = manifest.get("pieces")
    if not isinstance(pieces, list) or not pieces:
        refuse("no pieces")
    at, previous = 0, "-"
    for i, piece in enumerate(pieces):
        path = manifest_path.parent / str(piece.get("file", ""))
        if piece.get("start") != at or piece.get("input_sha256") != previous:
            refuse(f"piece {i}: the chain breaks (start {piece.get('start')}, {at} owed)")
        if not path.is_file() or sha(path) != piece.get("sha256"):
            refuse(f"piece {i} ({path.name}): its digest does not match")
        count = piece.get("count")
        if not isinstance(count, int) or count <= 0:
            refuse(f"piece {i}: count {count}")
        at += count
        previous = piece["sha256"]
    if at != int(restore):
        refuse(f"the chain reaches {at} tokens, {restore} owed")
    blob = manifest_path.parent / str(manifest.get("final_blob", ""))
    if (manifest.get("final_blob") != pieces[-1].get("file") or manifest.get("final_sha256") != pieces[-1].get("sha256")
            or not blob.is_file() or sha(blob) != manifest.get("final_sha256")):
        refuse("the final blob does not match")
    return blob, sha(manifest_path)


def check_build_identity(identity_line):
    """79-7: the production pair executable's identity line, checked once per process before timing; anything but
    "PKV_PAIR_PRODUCTION_V2 defs=none" raises Refusal (exit code EXIT_CODES["seam"])."""
    if (identity_line or "").strip() != IDENTITY:
        raise Refusal(f"79-7: pair executable identity {identity_line!r}, {IDENTITY!r} owed", "seam")
    return None


# ---- the row ------------------------------------------------------------------------------------------------------

def grade_row(processes):
    """processes: one dict per process, {"status": "ok", "median": float} or {"status": "refused", "kind": k}.
    A row is graded only on five complete processes: {"graded": True, "verdict": row_verdict(...), "record": {...}};
    otherwise {"graded": False, "verdict": None, "record": None, "exit_code": EXIT_CODES[kind of the first refusal]}."""
    for p in processes:
        if p.get("status") != "ok":
            kind = p.get("kind")
            return {"graded": False, "verdict": None, "record": None,
                    "exit_code": EXIT_CODES.get(kind, EXIT_INFRASTRUCTURE)}
    if len(processes) != PROCESSES_PER_ROW:
        return {"graded": False, "verdict": None, "record": None, "exit_code": EXIT_INFRASTRUCTURE}
    medians = [float(p["median"]) for p in processes]
    verdict = row_verdict(medians)
    return {"graded": True, "verdict": verdict,
            "record": {"verdict": verdict, "process_medians": medians, "largest": max(medians),
                       "median_of_medians": statistics.median(medians), "bar": BAR}}


def parse_process_output(text):
    """The pair executable's stdout: the identity line, then PAIR lines, then IDS, and optionally a RESERVE line (the
    paged arm's scatter-reserve construction: `RESERVE seed=<s> singles=<n> source=<header>`). Returns (identity,
    pairs, ids). The RESERVE fields are read by parse_reserve."""
    lines = text.splitlines()
    identity = lines[0] if lines else ""
    check_build_identity(identity)  # 79-7, before any pair is read
    pairs, ids = [], None
    for line in lines[1:]:
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "PAIR":
            if len(fields) != 5:
                raise Refusal(f"malformed pair line {line!r}", "pairs")
            pairs.append((float(fields[1]), float(fields[2]), int(fields[3]), int(fields[4])))
        elif fields[0] == "IDS":
            ids = [int(x) for x in fields[1:]]
    return identity, pairs, ids


def parse_reserve(text):
    """The RESERVE line's fields ({"seed", "singles", "source"}), or None when the pair executable reports none
    (T-3202 F-11: every 7.9 verdict is measured over one reserve permutation, and the records name it)."""
    for line in text.splitlines():
        fields = line.split()
        if fields and fields[0] == "RESERVE":
            out = dict(f.split("=", 1) for f in fields[1:] if "=" in f)
            if "seed" in out:
                out["seed"] = int(out["seed"])
            if "singles" in out:
                out["singles"] = int(out["singles"])
            return out
    return None


def pair_executable(pair_cmd):
    """The pair executable a --pair-cmd runs: its first element, resolved on PATH when it is not a path."""
    first = Path(pair_cmd[0])
    if first.is_file():
        return first.resolve()
    found = shutil.which(pair_cmd[0])
    if not found:
        raise Refusal(f"the pair executable {pair_cmd[0]!r} does not exist")
    return Path(found).resolve()


def main(argv=None):
    """The process loop, cell 7.9's driver, as a command (rev 16.23 G-4; T-3196 F-6). See the module docstring.
    Returns the exit code, 0 for a graded row (its verdict PASS, FAIL or NO RESULT is in the row record)."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pair-cmd", required=True, help="JSON list: the pair executable's command line")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--restore", required=True, type=int)
    parser.add_argument("--slot", required=True)
    parser.add_argument("--result", required=True, type=Path)
    parser.add_argument("--lock", type=Path, default=LOCK_PATH)
    parser.add_argument("--page-positions", type=int, default=64, choices=(64, 16))
    args = parser.parse_args(argv)
    try:
        pair_cmd = json.loads(args.pair_cmd)
        if not isinstance(pair_cmd, list) or not pair_cmd or not all(isinstance(x, str) for x in pair_cmd):
            raise ValueError
    except ValueError:
        print("INFRASTRUCTURE: --pair-cmd is not a JSON list of strings", file=sys.stderr)
        return EXIT_INFRASTRUCTURE
    result = args.result
    if args.slot not in _table(args.page_positions):
        print(f"INFRASTRUCTURE: slot {args.slot} has no row at B = {args.page_positions}", file=sys.stderr)
        return EXIT_INFRASTRUCTURE
    stem = result.with_suffix("")
    process_path = lambda k: stem.with_name(f"{stem.name}.p{k}.json")  # noqa: E731
    exe_path, exe_sha = None, None
    try:
        # D-SLM8461: the pair executable's SHA-256, taken by the driver before the first process starts
        exe_path = pair_executable(pair_cmd)
        exe_sha = sha(exe_path)
        with timing_lock(result, args.lock) as lock:
            processes, id_lists, reserves = [], [], []
            for k in range(1, PROCESSES_PER_ROW + 1):
                attempts = 0
                while True:
                    attempts += 1
                    if attempts > RERUN_LIMIT:
                        raise Refusal(f"INFRASTRUCTURE: process {k} failed {RERUN_LIMIT} times")
                    boot_before = last_boot()
                    blob, manifest_sha = check_row_blob(args.manifest, args.model, args.restore)  # 79-6, per process
                    command = pair_cmd + ["--blob", str(blob), "--model", str(args.model), "--process", str(k),
                                          "--pairs", str(PAIRS_PER_PROCESS), "--warmup", str(WARMUP_TOKENS)]
                    try:
                        done = subprocess.run(command, capture_output=True, text=True, timeout=PROCESS_CAP_SECONDS)
                    except subprocess.TimeoutExpired:
                        print(f"process {k}: over {PROCESS_CAP_SECONDS} s; rerun whole", file=sys.stderr)
                        continue
                    # T-3202 F-2: the exit status first. A pair process that crashed (any non-zero exit, before or
                    # after its identity line) is rerun whole; only a clean exit's identity is graded (79-7).
                    if done.returncode != 0:
                        print(f"process {k}: the pair executable exited {done.returncode}; rerun whole\n"
                              f"{done.stderr[-2000:]}", file=sys.stderr)
                        continue
                    identity, pairs, ids = parse_process_output(done.stdout)  # 79-7
                    reserve = parse_reserve(done.stdout)
                    median = process_statistic(pairs)  # 79-9, 79-8
                    boot_after = last_boot()
                    if not same_boot(boot_before, boot_after):
                        print(f"process {k}: the boot changed ({boot_before} -> {boot_after}); discarded, rerun",
                              file=sys.stderr)
                        continue
                    break
                record = {"slot": args.slot, "process": k, "complete": True, "median": median,
                          "pairs": [list(p) for p in pairs], "ids": ids, "identity": identity, "boot": boot_before,
                          "blob_sha256": sha(blob), "manifest_sha256": manifest_sha,
                          "model_sha256": sha(args.model), "restore": args.restore, "attempts": attempts,
                          "page_positions": args.page_positions, "exe": str(exe_path), "exe_sha256": exe_sha,
                          "reserve": reserve}
                write_record(process_path(k), record)
                processes.append({"status": "ok", "median": median})
                id_lists.append(ids or [])
                reserves.append(reserve)
            graded = grade_row(processes)
            # D-SLM8461: STANDING exactly when the driver finished the row and wrote a verdict (PASS, FAIL or NO
            # RESULT); the verdict stays its own field. T-3202 F-11: the reserve the ids were read over, with its seed
            # and source, as the pair executable reported it (null where it reports none).
            row = {"slot": args.slot, "restore": args.restore, "page_positions": args.page_positions,
                   "processes": [process_path(k).name for k in range(1, PROCESSES_PER_ROW + 1)],
                   "lock_recovered_from": lock["recovered_from"], "rule_verdict": graded["verdict"],
                   "status": "STANDING", "exe": str(exe_path), "exe_sha256": exe_sha,
                   "reserve": reserves[0] if all(r == reserves[0] for r in reserves) else reserves}
            row.update(graded["record"] or {})
            if not id_lists_agree(id_lists):
                row.update(verdict="NO RESULT", reason="the five measured-holder id lists differ (construction defect)")
            else:
                ids = id_lists[0]
                m99 = guard_bound(args.slot, args.page_positions, len(ids))
                m = mode_step(ids)
                row.update(guard={"n": len(ids), "M": m, "M99": m99, "admits": m <= m99})
                if m > m99:
                    row.update(verdict="NO RESULT", reason="the scatter guard rejects the measured holder's ids")
            write_record(result, row)
            print(f"{args.slot}: {row['verdict']} (process medians {graded['record']['process_medians']})")
            return 0
    except Refusal as refusal:
        print(f"REFUSED: {refusal}", file=sys.stderr)
        code = EXIT_CODES.get(refusal.kind, EXIT_INFRASTRUCTURE)
        supersede_refused(result, args.slot, code, str(refusal), exe_path, exe_sha)
        return code


def supersede_refused(result, slot, code, reason, exe_path, exe_sha):
    """T-3202 F-12 with D-SLM8461: a refused run never leaves an earlier run's row record standing at its path. Where a
    record exists at `result`, it is replaced by a REFUSED record carrying the exit code and no verdict; where none
    exists, none is written (a refused row has no row record, cell 7.9's per-process rule)."""
    if not Path(result).exists():
        return
    write_record(result, {"slot": slot, "status": "REFUSED", "exit_code": code, "reason": reason, "verdict": None,
                          "exe": str(exe_path) if exe_path else None, "exe_sha256": exe_sha,
                          "superseded": "the previous row record at this path"})


if __name__ == "__main__":
    sys.exit(main())
