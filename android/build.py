#!/usr/bin/env python3
"""Build the Android ARM64 APK using the native shader tool and the NDK."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--discord-sdk", type=Path,
                        help="Official Android Social SDK AAR, version 1.10 or newer")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.discord_sdk and (not args.discord_sdk.is_file()
                             or args.discord_sdk.suffix != ".aar"):
        parser.error("--discord-sdk must point to the official Android AAR")

    root = Path(__file__).resolve().parent.parent

    def run(*command, cwd=root):
        subprocess.run([str(part) for part in command], cwd=cwd, check=True)

    # Keep the pinned upstream revisions. Desktop-only dependencies are not
    # required to cross-compile Android or build the standalone shader tool.
    submodules = subprocess.check_output(
        ["git", "config", "--file", ".gitmodules", "--get-regexp", "path"],
        cwd=root, text=True,
    )
    excluded = {"DirectXShaderCompiler", "mesa", "MoltenVK", "metal-cpp", "wxWidgets"}
    paths = [line.split(None, 1)[1] for line in submodules.splitlines()]
    paths = [path for path in paths if Path(path).name not in excluded]
    run("git", "submodule", "update", "--init", "--depth", "1", "-j", args.jobs,
        "--", *paths)
    run(sys.executable, "xenia-build.py", "slang")
    run(sys.executable, "xenia-build.py", "fetchdata")

    host_build = root / "build" / "host-tools"
    run("cmake", "-S", root / "tools" / "build", "-B", host_build,
        "-DCMAKE_BUILD_TYPE=Release")
    run("cmake", "--build", host_build, "--config", "Release",
        "--target", "xenia-shader-cc", "--parallel", args.jobs)
    executable = "xenia-shader-cc.exe" if os.name == "nt" else "xenia-shader-cc"
    candidates = [host_build / "Release" / executable, host_build / executable]
    shader_cc = next((path for path in candidates if path.is_file()), None)
    if shader_cc is None:
        raise RuntimeError("Native xenia-shader-cc was not produced")

    project = root / "android" / "android_studio_project"
    wrapper = project / ("gradlew.bat" if os.name == "nt" else "gradlew")
    command = [str(wrapper)] if os.name == "nt" else ["sh", str(wrapper)]
    sdk_args = ([f"-PxeniaDiscordSdk={args.discord_sdk.resolve().as_posix()}"]
                if args.discord_sdk else [])
    run(*command, "--console=plain", f"assembleGithub{args.config}",
        f"-PxeniaHostShaderCompiler={shader_cc.as_posix()}",
        f"-PxeniaBuildJobs={args.jobs}", *sdk_args, cwd=project)
    print("APKs:", project / "app" / "build" / "outputs" / "apk" / "github")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
