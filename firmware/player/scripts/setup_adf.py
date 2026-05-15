"""
scripts/setup_adf.py  –  PlatformIO pre-build extra-script
===========================================================
Runs before CMake.  Clones ESP-ADF (if not already present) into
<project_root>/esp-adf and sets the ADF_PATH environment variable so that
the root CMakeLists.txt can find it.

Usage: extra_scripts = pre:scripts/setup_adf.py   (in platformio.ini)
"""

import os
import subprocess
import sys

# SCons environment is injected by PlatformIO
Import("env")  # noqa: F821  (SCons magic import)

# ── Configuration ────────────────────────────────────────────────────────────
ADF_REPO   = "https://github.com/espressif/esp-adf.git"
# Pin to a release tag that supports ESP-IDF 5.x.
# Update this when you upgrade your platform = espressif32 version.
ADF_BRANCH = "v2.7"

PROJECT_DIR = env["PROJECT_DIR"]
ADF_PATH    = os.path.join(PROJECT_DIR, "esp-adf")

# ── Clone if missing ─────────────────────────────────────────────────────────
git_dir = os.path.join(ADF_PATH, ".git")
if not os.path.isdir(git_dir):
    print(f"[ESP-ADF] Cloning {ADF_REPO} @ {ADF_BRANCH} …")
    try:
        subprocess.check_call([
            "git", "clone",
            "--branch", ADF_BRANCH,
            "--depth",  "1",
            "--recurse-submodules",
            "--shallow-submodules",
            ADF_REPO,
            ADF_PATH,
        ])
        print("[ESP-ADF] Clone complete.")
    except subprocess.CalledProcessError as exc:
        print(f"[ESP-ADF] ERROR: git clone failed ({exc})", file=sys.stderr)
        print("[ESP-ADF] Set ADF_PATH manually or clone the repo yourself.", file=sys.stderr)
        env.Exit(1)
else:
    print(f"[ESP-ADF] Found existing clone at: {ADF_PATH}")

# ── Export ADF_PATH to the CMake sub-process ─────────────────────────────────
os.environ["ADF_PATH"] = ADF_PATH

# env["ENV"] is the dictionary SCons passes to every external command it spawns
# (including the cmake/ninja invocations that PlatformIO triggers).
env["ENV"]["ADF_PATH"] = ADF_PATH

print(f"[ESP-ADF] ADF_PATH = {ADF_PATH}")
