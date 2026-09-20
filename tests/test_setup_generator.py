"""Exercise dependency setup against real, disposable Git repositories."""
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import setup_generator


class GeneratorSetupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="darkrecomp generator ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.upstream = self.root / "upstream"
        self.upstream.mkdir()
        self.git(self.upstream, "init", "--quiet")
        self.git(self.upstream, "config", "core.autocrlf", "false")
        self.git(self.upstream, "config", "core.safecrlf", "false")
        self.git(self.upstream, "config", "user.name", "Generator setup test")
        self.git(self.upstream, "config", "user.email", "test@example.invalid")
        self.original = {"main.cpp": "before\nbody\nafter\n", "context.h": "old header\n"}
        self.patched = {"main.cpp": "before\npatched body\nafter\n", "context.h": "new header\n"}
        for name, content in {**self.original, "notes.txt": "original note\n"}.items():
            (self.upstream / name).write_text(content, newline="\n")
        self.git(self.upstream, "add", ".")
        self.git(self.upstream, "-c", "commit.gpgsign=false", "commit", "-qm", "Fixture base")
        self.commit = self.git(self.upstream, "rev-parse", "HEAD").stdout.strip()
        for name, content in self.patched.items():
            (self.upstream / name).write_text(content, newline="\n")
        self.patch = self.root / "darkness fixes.patch"
        self.git(self.upstream, "diff", "--binary", "--full-index", f"--output={self.patch}")
        self.git(self.upstream, "restore", ".")
        self.destination = self.root / "checkout with spaces" / "XenonRecomp"

    def git(self, directory, *arguments):
        return subprocess.run(["git", "-C", str(directory), *arguments], check=True,
                              capture_output=True, text=True)

    def setup(self):
        with redirect_stdout(io.StringIO()):
            setup_generator.setup(self.destination, upstream=str(self.upstream),
                                  commit=self.commit, patch_path=self.patch)

    def assert_patched(self):
        for name, content in self.patched.items():
            self.assertEqual((self.destination / name).read_text(), content)

    def clone_original(self):
        self.destination.parent.mkdir(parents=True)
        self.git(self.root, "clone", str(self.upstream), str(self.destination))

    def test_fresh_setup_clones_pinned_revision_and_applies_patch(self):
        # Upstream advances: setup must still select our pinned revision.
        (self.upstream / "notes.txt").write_text("new upstream note\n")
        self.git(self.upstream, "-c", "commit.gpgsign=false", "commit", "-qam", "Advance upstream")
        self.setup()
        self.assertEqual(self.git(self.destination, "rev-parse", "HEAD").stdout.strip(), self.commit)
        self.assert_patched()
        self.assertEqual((self.destination / "notes.txt").read_text(), "original note\n")

    def test_original_checkout_is_patched(self):
        self.clone_original()
        self.setup()
        self.assert_patched()

    def test_repeated_setup_preserves_timestamps_build_outputs_and_other_edits(self):
        self.setup()
        source = self.destination / "main.cpp"
        timestamp = source.stat().st_mtime_ns
        (self.destination / "build").mkdir()
        output = self.destination / "build/generator.exe"
        output.write_bytes(b"existing build")
        notes = self.destination / "notes.txt"
        notes.write_text("developer notes\n")
        self.setup()
        self.assert_patched()
        self.assertEqual(source.stat().st_mtime_ns, timestamp)
        self.assertEqual(output.read_bytes(), b"existing build")
        self.assertEqual(notes.read_text(), "developer notes\n")

    def test_conflicting_edits_fail_without_partially_applying_patch(self):
        self.clone_original()
        source = self.destination / "main.cpp"
        source.write_text("a developer's different implementation\n")
        with self.assertRaisesRegex(RuntimeError, "Local changes"):
            self.setup()
        self.assertEqual(source.read_text(), "a developer's different implementation\n")
        self.assertEqual((self.destination / "context.h").read_text(), self.original["context.h"])

    def test_partially_applied_patch_is_not_overwritten(self):
        self.clone_original()
        (self.destination / "main.cpp").write_text(self.patched["main.cpp"])
        with self.assertRaisesRegex(RuntimeError, "partially applied"):
            self.setup()
        self.assertEqual((self.destination / "main.cpp").read_text(), self.patched["main.cpp"])
        self.assertEqual((self.destination / "context.h").read_text(), self.original["context.h"])

    def test_wrong_revision_is_not_changed(self):
        (self.upstream / "notes.txt").write_text("new upstream note\n")
        self.git(self.upstream, "-c", "commit.gpgsign=false", "commit", "-qam", "Advance upstream")
        self.clone_original()
        head = self.git(self.destination, "rev-parse", "HEAD").stdout
        with self.assertRaisesRegex(RuntimeError, "this release requires"):
            self.setup()
        self.assertEqual(self.git(self.destination, "rev-parse", "HEAD").stdout, head)
        self.assertEqual((self.destination / "main.cpp").read_text(), self.original["main.cpp"])

    def test_enclosing_repository_is_not_mistaken_for_dependency(self):
        self.git(self.root, "init", "--quiet")
        self.destination.mkdir(parents=True)
        (self.destination / "notes.txt").write_text("not a clone\n")
        with self.assertRaisesRegex(RuntimeError, "Expected a XenonRecomp Git checkout"):
            self.setup()
        self.assertEqual((self.destination / "notes.txt").read_text(), "not a clone\n")

    def test_old_readme_submodule_layout_is_supported(self):
        parent = self.root / "UnleashedRecomp"
        parent.mkdir()
        self.git(parent, "init", "--quiet")
        self.git(parent, "-c", "protocol.file.allow=always", "submodule", "add",
                 str(self.upstream), "tools/XenonRecomp")
        self.destination = parent / "tools/XenonRecomp"
        self.assertTrue((self.destination / ".git").is_file())
        self.setup()
        self.assert_patched()

    def test_missing_patch_fails_before_cloning(self):
        self.patch.unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing bundled generator patch"):
            self.setup()
        self.assertFalse(self.destination.exists())

    def test_dependencies_are_initialized_even_when_generator_is_already_patched(self):
        library = self.root / "library"
        library.mkdir()
        self.git(library, "init", "--quiet")
        (library / "header.h").write_text("dependency header\n")
        self.git(library, "add", ".")
        self.git(library, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "Library fixture")
        self.git(self.upstream, "-c", "protocol.file.allow=always", "submodule", "add",
                 str(library), "thirdparty/library")
        self.git(self.upstream, "-c", "commit.gpgsign=false", "commit", "-qam", "Add dependency")
        self.commit = self.git(self.upstream, "rev-parse", "HEAD").stdout.strip()
        self.clone_original()
        self.git(self.destination, "apply", str(self.patch))
        with patch.dict(os.environ, {"GIT_ALLOW_PROTOCOL": "file"}):
            self.setup()
        self.assert_patched()
        self.assertEqual((self.destination / "thirdparty/library/header.h").read_text(),
                         "dependency header\n")


if __name__ == "__main__":
    unittest.main()
