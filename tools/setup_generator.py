"""Fetch the pinned XenonRecomp source and apply the bundled Darkness fixes."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "refs/UnleashedRecomp/tools/XenonRecomp"
UPSTREAM = "https://github.com/hedge-dev/XenonRecomp.git"
COMMIT = "c5bfd90d87f2ed0db8cff5c19ea3aff0e161e527"
PATCH = ROOT / "tools/patches/xenonrecomp-darkness.patch"


def git(directory: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if check and result.returncode:
        raise RuntimeError(f"git {' '.join(arguments)} failed:\n{result.stderr.strip()}")
    return result


def setup(destination: Path, *, upstream: str = UPSTREAM, commit: str = COMMIT,
          patch_path: Path = PATCH) -> None:
    destination = destination.resolve()
    patch_path = patch_path.resolve()
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

    # Reverse-checking makes repeated builds work with the already patched
    # development tree. Git checks the entire patch before changing any files.
    patch_arguments = ("--ignore-space-change", "--whitespace=nowarn", str(patch_path))
    if git(destination, "apply", "--reverse", "--check", *patch_arguments,
           check=False).returncode == 0:
        print("Darkness generator patches are already installed.")
    else:
        ready = git(destination, "apply", "--check", *patch_arguments, check=False)
        if ready.returncode:
            raise RuntimeError("Cannot apply the bundled XenonRecomp patch to this checkout. "
                               "Local changes or a partially applied patch may conflict. "
                               f"Move {destination} aside and rerun tools/build.ps1; "
                               f"your files have not been changed.\n{ready.stderr.strip()}")
        git(destination, "apply", *patch_arguments)
        print("Installed Darkness generator patches.")
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
