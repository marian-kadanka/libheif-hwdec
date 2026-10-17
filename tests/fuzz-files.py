#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Mutation fuzzer for libheif-hwdec.

Corrupts HEIC files (byte flips, overwritten runs, truncation, all inside the coded image data
after the 'mdat' box header), decodes each corrupted file with the hwdec plugin in a separate
heif-hwdec-bench process, and keeps every file whose decode crashes, trips a sanitizer or runs
past the timeout. A decode that fails with an error message is the expected outcome for a broken
file and counts as a pass.

Build heif-hwdec-bench and the plugin with -fsanitize=address,undefined for the sanitizer checks.

Usage: fuzz-files.py BENCH PLUGIN_DIR BACKEND ITERATIONS OUT_DIR FILE...
  BACKEND     vaapi, cuda or vulkan (sets LIBHEIF_HWDEC_BACKEND)
  OUT_DIR     receives <n>.heic and <n>.log for every failing case
Environment: FUZZ_SEED (default 1), FUZZ_TIMEOUT seconds per decode (default 30)
"""

import os
import random
import subprocess
import sys
import tempfile

SANITIZER_EXIT = 99


def mutate(data, rng):
    mdat = data.find(b"mdat")
    start = mdat + 4 if mdat >= 0 else 0
    if start >= len(data):
        return data, "unchanged"
    buf = bytearray(data)
    kind = rng.choice(["flip", "flip", "run", "truncate"])
    if kind == "flip":
        n = rng.randint(1, 16)
        for _ in range(n):
            pos = rng.randrange(start, len(buf))
            buf[pos] ^= 1 << rng.randrange(8)
        return bytes(buf), f"flip {n} bits"
    if kind == "run":
        pos = rng.randrange(start, len(buf))
        length = rng.randint(1, 64)
        value = rng.choice([0x00, 0xff, rng.randrange(256)])
        for i in range(pos, min(pos + length, len(buf))):
            buf[i] = value
        return bytes(buf), f"run of 0x{value:02x} at {pos} length {length}"
    cut = rng.randrange(start, len(buf))
    return bytes(buf[:cut]), f"truncate at {cut} of {len(buf)}"


def main():
    if len(sys.argv) < 7:
        print(__doc__)
        return 2
    bench, plugin_dir, backend, iterations, out_dir = sys.argv[1:6]
    files = sys.argv[6:]
    iterations = int(iterations)
    seed = int(os.environ.get("FUZZ_SEED", "1"))
    timeout = float(os.environ.get("FUZZ_TIMEOUT", "30"))
    rng = random.Random(seed)
    os.makedirs(out_dir, exist_ok=True)

    system_plugins = subprocess.run([bench, "--plugin-dir"], capture_output=True, text=True).stdout.strip()
    env = dict(os.environ)
    env["LIBHEIF_PLUGIN_PATH"] = f"{plugin_dir}:{system_plugins}"
    env["LIBHEIF_HWDEC_BACKEND"] = backend
    env["LIBHEIF_HWDEC_DEBUG"] = "1"
    # GPU drivers keep allocations until exit; leak reports from them are not findings.
    # protect_shadow_gap=0: with the gap protected, cuInit() fails with CUDA_ERROR_OUT_OF_MEMORY.
    env["ASAN_OPTIONS"] = f"detect_leaks=0:protect_shadow_gap=0:exitcode={SANITIZER_EXIT}:abort_on_error=0"
    env["UBSAN_OPTIONS"] = f"print_stacktrace=1:exitcode={SANITIZER_EXIT}"

    originals = [open(f, "rb").read() for f in files]
    counts = {"decoded": 0, "error": 0, "crash": 0, "sanitizer": 0, "timeout": 0}
    gpu_pictures = 0

    with tempfile.TemporaryDirectory() as work:
        for n in range(iterations):
            index = rng.randrange(len(files))
            data, how = mutate(originals[index], rng)
            path = os.path.join(work, "case.heic")
            with open(path, "wb") as f:
                f.write(data)

            try:
                proc = subprocess.run([bench, "-d", "hwdec", path], env=env, capture_output=True,
                                      timeout=timeout)
                stdout = proc.stdout.decode(errors="replace")
                stderr = proc.stderr.decode(errors="replace")
                gpu_pictures += stderr.count(" on gpu")
                if proc.returncode < 0:
                    outcome = "crash"
                elif proc.returncode == SANITIZER_EXIT or "ERROR: AddressSanitizer" in stderr or \
                        "runtime error:" in stderr:
                    outcome = "sanitizer"
                elif proc.returncode == 0:
                    outcome = "decoded"
                elif "FAILED" in stdout:
                    outcome = "error"
                else:
                    outcome = "crash"
                log = f"exit {proc.returncode}\n--- stdout\n{stdout}\n--- stderr\n{stderr}"
            except subprocess.TimeoutExpired as e:
                outcome = "timeout"
                log = f"timeout after {timeout} s\n--- stderr\n{(e.stderr or b'').decode(errors='replace')}"

            counts[outcome] += 1
            if outcome in ("crash", "sanitizer", "timeout"):
                case = os.path.join(out_dir, f"{n:05d}")
                with open(case + ".heic", "wb") as f:
                    f.write(data)
                with open(case + ".log", "w") as f:
                    f.write(f"source {files[index]}\nmutation {how}\n{log}")
                print(f"case {n}: {outcome} ({os.path.basename(files[index])}, {how}) -> {case}.heic",
                      flush=True)

    print(f"{backend}: {iterations} cases from {len(files)} files, seed {seed}: "
          + ", ".join(f"{k} {v}" for k, v in counts.items())
          + f"; {gpu_pictures} pictures decoded on the GPU")
    return 1 if counts["crash"] or counts["sanitizer"] or counts["timeout"] else 0


if __name__ == "__main__":
    sys.exit(main())
