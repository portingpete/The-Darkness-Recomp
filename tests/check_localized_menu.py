"""Open a changed startup cache through the guest's real native file bridge."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def main():
    executable, source = map(lambda value: Path(value).resolve(), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="DarkRecomp localized menu ") as temporary:
        game = Path(temporary) / "Darkness"
        for name in ("default.xex", "Content/Gui/CubeWnd.xcr", "Content/Xdf/GameContext_Create.XDF"):
            target = game / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, target)
        archive = game / "Content/Xdf/GameContext_Create.XDF"
        data = bytearray(archive.read_bytes())
        assert struct.unpack_from("<I", data)[0] == 0x101
        # Change only a cached file timestamp. The compressed strings/fonts
        # remain identical, while the source hash selects the private cache.
        files = 8 + struct.unpack_from("<I", data, 4)[0] + 4
        data[files + 16] ^= 1
        archive.write_bytes(data)
        result = subprocess.run([str(executable), str(game), "--video-settings"],
                                capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        print(output, end="")
        if result.returncode:
            return result.returncode
        assert "Preserving this dump's startup assets" in output, "Private cache was not selected"
        assert archive.read_bytes() == data, "Guest changed the source startup archive"
        print("Localized menu cache passed original guest file opens and PC menu controls.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
