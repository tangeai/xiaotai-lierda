#!/usr/bin/env python3
"""Fetch the pinned Lierda SDK and overlay this repository's OpenKit code."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
from http.client import HTTPException
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen


SDK_URL = "https://github.com/lierda-iot/CAT1.bis_OpenCPU.git"
SDK_COMMIT = "59f1250c4b8caad4bbf4f74167b7b39c79097f8c"
LEGACY_HM_CACHE = "xiaotai-sdk/F9D_D-1.7.1/"
HM_TIRTC_RELEASE = "tirtc__eigencomm-ec71x__gcc-arm-none-eabi-10-2020-q4-major__v2.5.0__mini"
HM_TIRTC_URL = ("https://repo-sdk.tange-ai.com/repository/tirtc-sdks/releases/eigencomm-ec71x/"
                f"{HM_TIRTC_RELEASE}/{HM_TIRTC_RELEASE}.tgz")
HM_TIRTC_SHA256 = "9c1cf0ac1f3b0c7f9df25837453c079935997fc28bf576c6531e3b193fbc8e65"
OPENKIT = "examples/NT26FxDx_OpenKit"
FIRMWARE = OPENKIT + "/firmware/"
REPO = Path(__file__).resolve().parents[1]
LITE = "TiRTC-EC718PM-Lite"
PRO = "TiRTC-EC718PM-Pro"
HM_PRO = "TiRTC-EC718HM-Pro"
HM_TIRTC_FILES = {
    "include/tirtc/basedef.h": "23883b8040838fcda10927523536def667cfe7be7fcd8b6f4a1c9410d3e03c04",
    "include/tirtc/tgtrp.h": "c46c23d112340d7a2fbf308f6a70001df5eb1c4bab62a9beafbcd250c503d4fa",
    "include/tirtc/ticloudstorage.h": "00c6db292468095fcf6ee00e3ec3c61e79752e9e12adb4bd1c66b0210c2c58d7",
    "include/tirtc/tiRTC_stat.h": "2932246a6a09aad0cc3d8252410b34c349e45ffe052682a6dfc0ac10ee1d6541",
    "include/tirtc/tiRTC.h": "b1315121abf843c43c669f754382cab302c3b0b35bdfe41f4c4756ef95636293",
    "lib/libTiRTC.a": "31c72553a082104b28eb8a2baa3e837b87afd25a54c2330eadc640dd0f287a5d",
}
LEGACY_BUILD_ENTRIES = {
    "tirtc_lite": (LITE, ("build.bat", "build_app.bat")),
    "tirtc_pro": (PRO, ("build.bat", "build_app.bat", "build_assets.bat")),
}


def git(directory: Path, *args: str) -> bytes:
    return subprocess.check_output(
        ["git", "-c", "core.autocrlf=false", "-c", "core.longpaths=true",
         "-C", str(directory), *args],
        stderr=subprocess.PIPE,
    )


def digest(path: Path, *, blob: bool = False) -> str:
    result = hashlib.sha1() if blob else hashlib.sha256()
    if blob:
        result.update(b"blob " + str(path.stat().st_size).encode("ascii") + b"\0")
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def git_path(sdk: Path, name: str) -> Path:
    path = Path(git(sdk, "rev-parse", "--git-path", name).decode().strip())
    return path if path.is_absolute() else sdk / path


def prepare(sdk: Path, check: bool) -> None:
    if sdk == REPO or REPO in sdk.parents or sdk in REPO.parents:
        raise ValueError("SDK directory must be outside this repository and its parents.")
    if not sdk.exists() or (sdk.is_dir() and not any(sdk.iterdir())):
        if check:
            raise ValueError("SDK is not prepared. Run setup_sdk.py without --check first.")
        sdk.mkdir(parents=True, exist_ok=True)
        git(sdk, "init", "--quiet")
        git(sdk, "config", "core.autocrlf", "false")
        git(sdk, "config", "core.longpaths", "true")
        git(sdk, "remote", "add", "origin", SDK_URL)
        git_path(sdk, "xiaotai-setup-pending").write_text(
            SDK_COMMIT + "\n" + SDK_URL + "\n", encoding="utf-8", newline="\n")
    if not (sdk / ".git").exists():
        raise ValueError("SDK directory is not a Git checkout; choose a new --sdk-dir.")
    if Path(git(sdk, "rev-parse", "--show-toplevel").decode().strip()).resolve() != sdk:
        raise ValueError("SDK directory must be the checkout root.")
    pending = git_path(sdk, "xiaotai-setup-pending")
    expected = SDK_COMMIT + "\n" + SDK_URL + "\n"
    try:
        revision = git(sdk, "rev-parse", "--verify", "HEAD").decode().strip()
    except subprocess.CalledProcessError:
        if check:
            raise ValueError("SDK download is incomplete. Run setup_sdk.py without --check to retry.")
        if not pending.is_file() or pending.read_text(encoding="utf-8") != expected:
            raise ValueError("SDK has no revision and was not initialized by this script; choose a new --sdk-dir.")
        if git(sdk, "remote", "get-url", "origin").decode().strip() != SDK_URL:
            raise ValueError("Incomplete SDK has a different origin; choose a new --sdk-dir.")
        if any(path.name != ".git" for path in sdk.iterdir()) or git(sdk, "ls-files", "-z"):
            raise ValueError("Incomplete SDK contains local files; nothing was overwritten. Preserve them and choose a new --sdk-dir.")
        print(f"Downloading Lierda SDK {SDK_COMMIT} to {sdk}", flush=True)
        # Retry only our empty, unfinished download. Checkout never discards edits.
        git(sdk, "fetch", "--depth", "1", "origin", SDK_COMMIT)
        git(sdk, "checkout", "--quiet", "--detach", SDK_COMMIT)
        revision = git(sdk, "rev-parse", "--verify", "HEAD").decode().strip()
    if revision != SDK_COMMIT:
        raise ValueError("Use the shared OpenKit 1.7.0 checkout at " + SDK_COMMIT +
                         " with --sdk-dir. The script does not switch an existing checkout.")
    if not check and pending.is_file() and pending.read_text(encoding="utf-8") == expected:
        pending.unlink()


def download(url: str, destination: Path, sha256: str) -> None:
    for attempt in range(3):
        try:
            with urlopen(url, timeout=30) as response, destination.open("wb") as stream:
                shutil.copyfileobj(response, stream, 1024 * 1024)
        except (OSError, HTTPException) as error:
            if attempt == 2:
                raise ValueError(f"Cannot download {url}: {error}") from error
        else:
            if digest(destination) == sha256:
                return
    raise ValueError(f"Download SHA-256 mismatch: {url}")


def hm_tirtc_files(sdk: Path, scratch: Path, check: bool, tirtc_sdk: Path | None) -> dict[str, Path]:
    sources = {}
    tirtc_prefix = f"{OPENKIT}/{HM_PRO}/sdk"
    if tirtc_sdk is None:
        tirtc_sdk = sdk / tirtc_prefix
        if not check and not all((tirtc_sdk / name).is_file() for name in HM_TIRTC_FILES):
            print("Downloading TiRTC 2.5.0 mini for HM Pro", flush=True)
            archive = scratch / "tirtc.tgz"
            download(HM_TIRTC_URL, archive, HM_TIRTC_SHA256)
            tirtc_sdk = scratch / "tirtc"
            with tarfile.open(archive, "r:gz") as package:
                for name in HM_TIRTC_FILES:
                    member = package.getmember(f"{HM_TIRTC_RELEASE}/{name}")
                    if not member.isfile():
                        raise ValueError(f"Unexpected TiRTC SDK archive entry: {name}")
                    destination = tirtc_sdk / name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with package.extractfile(member) as stream, destination.open("wb") as output:
                        shutil.copyfileobj(stream, output)
    for name, sha256 in HM_TIRTC_FILES.items():
        source = tirtc_sdk / name
        if not source.is_file() or source.is_symlink() or digest(source) != sha256:
            raise ValueError(f"HM Pro TiRTC SDK file missing or modified: {source}")
        sources[f"{tirtc_prefix}/{name}"] = source
    return sources


@contextmanager
def overlay_lock(sdk: Path):
    # A shared manifest also tracks the common build files. Serialize writers.
    with git_path(sdk, "xiaotai-overlay.lock").open("a+b") as lock:
        if sys.platform == "win32":
            import msvcrt

            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def overlay(sdk: Path, check: bool, hm_pro: bool, tirtc_sdk: Path | None) -> None:
    products = (HM_PRO,) if hm_pro else (LITE, PRO)
    hm_prefix = f"{OPENKIT}/{HM_PRO}/"
    hm_paths = (hm_prefix, LEGACY_HM_CACHE)
    state_path = git_path(sdk, "xiaotai-overlay.json")
    previous = {}
    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if not isinstance(state, dict) or state.get("sdk_commit") != SDK_COMMIT:
            raise ValueError("Invalid previous OpenKit overlay record; choose a new --sdk-dir.")
        previous = state.get("files", {})
        if not isinstance(previous, dict) or any(
            not isinstance(name, str) or not isinstance(value, str)
            or (name not in ("Makefile", "rules/Makefile.rules")
                and not name.startswith((OPENKIT + "/", LEGACY_HM_CACHE)))
            or "\\" in name or any(part in ("", ".", "..") for part in name.split("/"))
            or len(value) != 64 or any(char not in "0123456789abcdef" for char in value)
            for name, value in previous.items()
        ):
            raise ValueError("Invalid previous OpenKit file record; choose a new --sdk-dir.")
    original = {}
    for row in git(sdk, "ls-tree", "-rz", "HEAD").split(b"\0"):
        if row:
            metadata, name = row.split(b"\t", 1)
            original[name.decode("utf-8")] = metadata.split()[2].decode("ascii")
    names = git(REPO, "ls-files", "-z", "--", OPENKIT).decode("utf-8").split("\0")
    external_sdk_paths = (f"{hm_prefix}sdk/include/", f"{hm_prefix}sdk/lib/")
    # Published downloads are not build inputs. Keep SDK-generated packages intact.
    sources = {
        name: REPO / name for name in sorted(names)
        if name and not name.startswith(FIRMWARE)
        and name != f"{hm_prefix}.gitkeep" and not name.startswith(external_sdk_paths)
        and (name.startswith(hm_prefix) == hm_pro or name == f"{OPENKIT}/Makefile")
    }
    if any(f"{OPENKIT}/{product}/build.bat" not in sources for product in products):
        raise ValueError("OpenKit source files are missing from this checkout.")
    with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-patch-") as scratch:
        scratch = Path(scratch)
        # An existing 1.7.1 video-board installation keeps its routing until
        # that product is selected for migration. PM builds use the same rules
        # on either side of the migration.
        legacy_hm = any(name.startswith(LEGACY_HM_CACHE) for name in previous)
        if legacy_hm and not hm_pro:
            for name in ("Makefile", "rules/Makefile.rules"):
                target = sdk / name
                if not target.is_file() or digest(target) != previous.get(name):
                    raise ValueError(f"Local changes to shared SDK build file: {name}")
                sources[name] = target
        else:
            patched = scratch / "Makefile"
            patched.write_bytes(git(sdk, "show", f"{SDK_COMMIT}:Makefile"))
            git(scratch, "apply", "--no-index", str(REPO / "tools/lierda-sdk.patch"))
            sources["Makefile"] = patched
            if "rules/Makefile.rules" in previous:
                # Restore the stock rules only when we own the previous overlay.
                rules = scratch / "Makefile.rules"
                rules.write_bytes(git(sdk, "show", f"{SDK_COMMIT}:rules/Makefile.rules"))
                sources["rules/Makefile.rules"] = rules
        if hm_pro:
            base_lib = sdk / "components/basePkg/F9D_D/lib"
            for archive in base_lib.glob("*.a"):
                if archive.relative_to(sdk).as_posix() not in original:
                    raise ValueError(f"Unexpected archive in native F9D_D: {archive}")
            sources.update(hm_tirtc_files(sdk, scratch, check, tirtc_sdk))
        # Keep existing customer batch commands working in prepared SDK trees.
        legacy_build_entries = {} if hm_pro else LEGACY_BUILD_ENTRIES
        for old, (product, scripts) in legacy_build_entries.items():
            for script in scripts:
                shim = scratch / f"{old}-{script}"
                shim.write_bytes((
                    f'@echo off\r\ncall "%~dp0..\\{product}\\{script}" %*\r\n'
                    'exit /b %errorlevel%\r\n'
                ).encode("ascii"))
                sources[f"{OPENKIT}/{old}/{script}"] = shim

        # Inspect every destination before copying anything. Preserve local edits.
        errors = []
        if check and hm_pro:
            # These product-specific globs feed the production HM object list in
            # tirtc_app.mk. Extra C files here can silently enter a release.
            hm_app = sdk / hm_prefix
            for pattern in ("ui/*.c", "ui/*/*.c", "ui/*/*/*.c", "network/*.c",
                            "platform/*.c", "contacts/*.c", "calls/*.c", "video/*.c", "group/*.c"):
                for candidate in hm_app.glob(pattern):
                    if candidate.relative_to(sdk).as_posix() not in sources:
                        errors.append(f"Unmanaged HM release source: {candidate}. "
                                      "Track it in the application repository and rerun setup, "
                                      "or move it outside the SDK product directory.")
        pending = []
        removed = []
        installed = {name: sha256 for name, sha256 in previous.items()
                     if name.startswith(hm_paths) != hm_pro and name not in sources}
        for name, source in sources.items():
            target = sdk / name
            if not source.is_file() or source.is_symlink():
                errors.append(f"Missing or unsupported source: {name}")
                continue
            if not target.resolve().is_relative_to(sdk) or target.is_symlink():
                errors.append(f"Unsafe SDK path: {name}")
                continue
            expected_hash = digest(source)
            installed[name] = expected_hash
            current_hash = digest(target) if target.is_file() else None
            if current_hash == expected_hash:
                continue
            if not target.exists() and name in previous:
                errors.append(f"Locally deleted application file: {name}")
                continue
            if target.exists() and (
                not target.is_file() or name not in original
                or digest(target, blob=True) != original[name]
            ) and (current_hash is None or current_hash != previous.get(name)):
                errors.append(f"Local SDK/application changes: {name}")
            elif check:
                errors.append(f"Overlay missing or outdated: {name}")
            else:
                pending.append((source, target))
        for name in previous.keys() - sources.keys():
            if name in installed or name.startswith((FIRMWARE, LEGACY_HM_CACHE)):
                continue
            target = sdk / name
            if not target.resolve().is_relative_to(sdk) or target.is_symlink() or name in original:
                errors.append(f"Cannot remove previous overlay path: {name}")
            elif target.exists():
                if not target.is_file() or digest(target) != previous[name]:
                    errors.append(f"Local changes to removed application file: {name}")
                elif check:
                    errors.append(f"Stale application file: {name}")
                else:
                    removed.append(target)
        changed = git(sdk, "diff", "HEAD", "--name-only", "-z", "--no-renames")
        for raw in changed.split(b"\0"):
            if raw and raw.decode("utf-8") not in sources and raw.decode("utf-8") not in installed:
                errors.append(f"Local SDK changes: {raw.decode('utf-8')}")
        if errors:
            raise ValueError("\n".join(errors[:12]) +
                             "\nNothing was overwritten. Preserve your edits and choose a new --sdk-dir.")
        for source, target in pending:
            target.parent.mkdir(parents=True, exist_ok=True)
            staged = None
            try:
                with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".xiaotai-",
                                                 suffix=".tmp", delete=False) as stream:
                    staged = Path(stream.name)
                    with source.open("rb") as content:
                        shutil.copyfileobj(content, stream)
                staged.replace(target)
            finally:
                if staged is not None:
                    staged.unlink(missing_ok=True)
        for target in removed:
            target.unlink()
        if not check:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=state_path.parent,
                                             prefix="xiaotai-overlay-", suffix=".tmp", delete=False) as stream:
                staged_state = Path(stream.name)
                json.dump({"sdk_commit": SDK_COMMIT, "files": installed}, stream, indent=2, sort_keys=True)
                stream.write("\n")
            try:
                staged_state.replace(state_path)
            finally:
                staged_state.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", choices=("pm", "hm-pro"), default="pm",
                        help="prepare the selected product in the shared OpenKit 1.7.0 SDK")
    parser.add_argument("--sdk-dir", type=Path,
                        help="shared 1.7.0 SDK checkout (default: ../CAT1.bis_OpenCPU-sdk)")
    parser.add_argument("--tirtc-sdk-dir", type=Path,
                        help="optional local copy of the exact HM Pro TiRTC 2.5.0 mini SDK")
    parser.add_argument("--check", action="store_true", help="verify an existing prepared SDK")
    args = parser.parse_args()
    hm_pro = args.product == "hm-pro"
    if not hm_pro and args.tirtc_sdk_dir is not None:
        parser.error("--tirtc-sdk-dir applies only to --product hm-pro")
    sdk = (args.sdk_dir or REPO.parent / "CAT1.bis_OpenCPU-sdk").expanduser().resolve()
    tirtc_sdk = args.tirtc_sdk_dir.expanduser().resolve() if args.tirtc_sdk_dir else None
    try:
        prepare(sdk, args.check)
        if args.check:
            overlay(sdk, True, hm_pro, tirtc_sdk)
        else:
            with overlay_lock(sdk):
                overlay(sdk, False, hm_pro, tirtc_sdk)
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError) as error:
        detail = error.stderr.decode(errors="replace").strip() if isinstance(
            error, subprocess.CalledProcessError) else str(error)
        print(f"ERROR: {detail}", file=sys.stderr)
        return 1
    print(f"SDK and OpenKit ready: {sdk}\nOpenKit checkout: {SDK_COMMIT}")
    if hm_pro:
        print("HM Pro base: native OpenKit 1.7.0 components/basePkg/F9D_D")
    print(f"Windows: cd {sdk}")
    for product in ((HM_PRO,) if hm_pro else (LITE, PRO)):
        print(f"  .\\examples\\NT26FxDx_OpenKit\\{product}\\build.bat")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
