#!/usr/bin/env python3
"""Fetch the pinned Lierda SDK and overlay this repository's OpenKit code."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


SDK_URL = "https://github.com/lierda-iot/CAT1.bis_OpenCPU.git"
SDK_COMMIT = "59f1250c4b8caad4bbf4f74167b7b39c79097f8c"
OPENKIT = "examples/NT26FxDx_OpenKit"
FIRMWARE = OPENKIT + "/firmware/"
REPO = Path(__file__).resolve().parents[1]


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
        raise ValueError("SDK revision differs from the audited version; choose a new --sdk-dir.")
    if not check and pending.is_file() and pending.read_text(encoding="utf-8") == expected:
        pending.unlink()


def overlay(sdk: Path, check: bool) -> None:
    state_path = git_path(sdk, "xiaotai-overlay.json")
    previous = {}
    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if not isinstance(state, dict) or state.get("sdk_commit") != SDK_COMMIT:
            raise ValueError("Invalid previous OpenKit overlay record; choose a new --sdk-dir.")
        previous = state.get("files", {})
        if not isinstance(previous, dict) or any(
            not isinstance(name, str) or not isinstance(value, str)
            or (name != "Makefile" and not name.startswith(OPENKIT + "/"))
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
    # Published downloads are not build inputs. Keep SDK-generated packages intact.
    sources = {name: REPO / name for name in names if name and not name.startswith(FIRMWARE)}
    if not sources or not any(name.startswith(OPENKIT + "/tirtc_lite/") for name in sources):
        raise ValueError("OpenKit source files are missing from this checkout.")

    with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-patch-") as scratch:
        patched = Path(scratch) / "Makefile"
        patched.write_bytes(git(sdk, "show", f"{SDK_COMMIT}:Makefile"))
        git(Path(scratch), "apply", "--no-index", str(REPO / "tools/lierda-sdk.patch"))
        sources["Makefile"] = patched

        # Inspect every destination before copying anything. Preserve local edits.
        errors = []
        pending = []
        removed = []
        installed = {}
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
            if name.startswith(FIRMWARE):
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
            if raw and raw.decode("utf-8") not in sources:
                errors.append(f"Local SDK changes: {raw.decode('utf-8')}")
        if errors:
            raise ValueError("\n".join(errors[:12]) +
                             "\nNothing was overwritten. Preserve your edits and choose a new --sdk-dir.")
        for source, target in pending:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        for target in removed:
            target.unlink()
        if not check:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=state_path.parent,
                                             prefix="xiaotai-overlay-", suffix=".tmp", delete=False) as stream:
                staged_state = Path(stream.name)
                json.dump({"sdk_commit": SDK_COMMIT, "files": installed}, stream, indent=2)
                stream.write("\n")
            try:
                staged_state.replace(state_path)
            finally:
                staged_state.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-dir", type=Path, default=REPO.parent / "CAT1.bis_OpenCPU-sdk",
                        help="external SDK checkout (default: ../CAT1.bis_OpenCPU-sdk)")
    parser.add_argument("--check", action="store_true", help="verify an existing prepared SDK")
    args = parser.parse_args()
    sdk = args.sdk_dir.expanduser().resolve()
    try:
        prepare(sdk, args.check)
        overlay(sdk, args.check)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        detail = error.stderr.decode(errors="replace").strip() if isinstance(
            error, subprocess.CalledProcessError) else str(error)
        print(f"ERROR: {detail}", file=sys.stderr)
        return 1
    print(f"SDK and OpenKit ready: {sdk}\nRevision: {SDK_COMMIT}")
    print(f"Windows: cd {sdk}\n  .\\examples\\NT26FxDx_OpenKit\\tirtc_lite\\build.bat"
          "\n  .\\examples\\NT26FxDx_OpenKit\\tirtc_pro\\build.bat")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
