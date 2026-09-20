"""Fetch the pinned XenonRecomp source and apply the bundled Darkness fixes."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "refs/UnleashedRecomp/tools/XenonRecomp"
UPSTREAM = "https://github.com/hedge-dev/XenonRecomp.git"
COMMIT = "c5bfd90d87f2ed0db8cff5c19ea3aff0e161e527"
PATCHES = (
    ROOT / "tools/patches/xenonrecomp-darkness.patch",
    ROOT / "tools/patches/xenonrecomp-corrections.patch",
)


def git(directory: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if check and result.returncode:
        raise RuntimeError(f"git {' '.join(arguments)} failed:\n{result.stderr.strip()}")
    return result


def install_patches(destination: Path, commit: str, patch_paths: tuple[Path, ...]) -> None:
    # Build the complete patch series away from the user's working tree. Keep
    # each shipped intermediate version so earlier installs can upgrade safely.
    names = set()
    for patch_path in patch_paths:
        stats = git(destination, "apply", "--numstat", "-z", str(patch_path)).stdout
        for entry in filter(None, stats.split("\0")):
            added, removed, name = entry.split("\t", 2)
            target = (destination / name).resolve()
            if added == "-" or removed == "-" or not target.is_relative_to(destination):
                raise RuntimeError(f"Unsupported generator patch path: {name}")
            names.add(name)
    with tempfile.TemporaryDirectory(prefix="darkrecomp-generator-") as temporary:
        staging = Path(temporary)
        git(staging, "init", "--quiet")
        git(staging, "config", "core.autocrlf", "false")
        versions = {}
        for name in sorted(names):
            original = git(destination, "show", f"{commit}:{name}").stdout.encode("utf-8")
            path = staging / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(original)
            versions[name] = {original}
        for patch_path in patch_paths:
            git(staging, "apply", "--whitespace=nowarn", str(patch_path))
            for name in names:
                versions[name].add((staging / name).read_bytes())

        updates = []
        for name in sorted(names):
            path = destination / name
            current = path.read_bytes()
            normalized = current.replace(b"\r\n", b"\n")
            desired = (staging / name).read_bytes()
            if normalized not in versions[name]:
                raise RuntimeError(f"Local changes in {path} conflict with the generator patches. "
                                   "Save your edits and restore a shipped version, or move the "
                                   "checkout aside and rerun tools/build.ps1. "
                                   "Your files have not been changed.")
            if normalized != desired:
                updates.append((path, desired.replace(b"\n", b"\r\n")
                                if b"\r\n" in current else desired))
        # Validate every affected file before writing any of them. An interrupted
        # write between files is recoverable because every version is recognized.
        for path, content in updates:
            path.write_bytes(content)
        print("Installed Darkness generator patches." if updates
              else "Darkness generator patches are already installed.")


def setup(destination: Path, *, upstream: str = UPSTREAM, commit: str = COMMIT,
          patch_paths: tuple[Path, ...] = PATCHES) -> None:
    destination = destination.resolve()
    patch_paths = tuple(path.resolve() for path in patch_paths)
    for patch_path in patch_paths:
        if not patch_path.is_file():
            raise RuntimeError(f"Missing bundled generator patch: {patch_path}")

    # Keep the original dependency location compatible with existing builds.
    # A fresh setup only needs XenonRecomp, not the whole UnleashedRecomp tree.
    if not destination.exists() or not any(destination.iterdir()):
        destination.parent.mkdir(parents=True, exist_ok=True)
        print("Downloading XenonRecomp...", flush=True)
        git(destination.parent, "clone", "--no-checkout", "--", upstream, str(destination))
        git(destination, "checkout", "--detach", commit)

    # Do not accidentally operate on an enclosing repository or replace a
    # developer's checkout. This also accepts the old README's Git submodule.
    root = git(destination, "rev-parse", "--show-toplevel", check=False)
    if root.returncode or Path(root.stdout.strip()).resolve() != destination:
        raise RuntimeError(f"Expected a XenonRecomp Git checkout at {destination}. "
                           "Move that directory aside and rerun tools/build.ps1.")
    head = git(destination, "rev-parse", "HEAD").stdout.strip()
    if head != commit:
        raise RuntimeError(f"XenonRecomp is at {head}, but this release requires {commit}. "
                           f"Move {destination} aside and rerun tools/build.ps1; "
                           "your checkout has not been changed.")

    install_patches(destination, commit, patch_paths)
    # Run even for an already patched tree: a download may have been interrupted.
    print("Preparing XenonRecomp dependencies...", flush=True)
    git(destination, "submodule", "update", "--init", "--recursive")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=DESTINATION,
                        help="Generator checkout directory (normally managed by tools/build.ps1)")
    args = parser.parse_args()
    try:
        setup(args.destination)
    except (OSError, RuntimeError) as exc:
        print(f"Generator setup: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
