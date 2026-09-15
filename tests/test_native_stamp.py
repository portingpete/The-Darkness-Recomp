"""Build a tiny native target through backdated edits and a source rollback."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[1]
scratch_root = (root / 'build_native/run').resolve()
scratch_root.mkdir(parents=True, exist_ok=True)
fixture = Path(tempfile.mkdtemp(prefix='native-stamp-test-', dir=scratch_root)).resolve()
if not fixture.is_relative_to(scratch_root):
    raise RuntimeError('Fixture escaped the test workspace')
env = {key.upper(): value for key, value in os.environ.items()}


def command(arguments, expect_mismatch=False):
    result = subprocess.run(arguments, cwd=fixture, env=env, capture_output=True,
                            text=True, creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
    if expect_mismatch:
        if not result.returncode or 'DarkNativeInputs' not in result.stdout + result.stderr:
            raise AssertionError('Linker accepted a stale native archive or failed for an unrelated reason')
    elif result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


try:
    (fixture / 'app').mkdir()
    (fixture / 'renderer').mkdir()
    main = fixture / 'app/main.cpp'
    value = fixture / 'renderer/value.h'
    original = b'constexpr int value = 1;\n'
    main.write_text('int payload();\nint main() { return payload(); }\n')
    (fixture / 'renderer/payload.cpp').write_text('#include "value.h"\nint payload() { return value; }\n')
    value.write_bytes(original)
    old_time = 1_000_000_000_000_000_000
    os.utime(value, ns=(old_time, old_time))
    (fixture / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(StampFixture LANGUAGES CXX)\n'
        f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
        f'include("{(root / "cmake/NativeCompilationStamp.cmake").as_posix()}")\n'
        'add_library(Payload STATIC renderer/payload.cpp)\n'
        'add_executable(StampFixture app/main.cpp)\ntarget_link_libraries(StampFixture PRIVATE Payload)\n'
        'dark_enable_native_compilation_stamp()\n')
    command(['cmake', '-S', '.', '-B', 'build', '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'ClangCL'])
    executable = fixture / 'build/Release/StampFixture.exe'
    header = fixture / 'build/native_inputs.h'

    def build(expected):
        command(['cmake', '--build', 'build', '--config', 'Release', '--parallel', '1', '--', '/nodeReuse:false'])
        code = subprocess.run([str(executable)], creationflags=subprocess.CREATE_NO_WINDOW, timeout=10).returncode
        if code != expected:
            raise AssertionError(f'Stale executable result: {code}, expected {expected}')

    build(1)
    archive = fixture / 'build/Release/Payload.lib'
    stale_archive = archive.read_bytes()
    first_stamp = header.read_bytes()
    value.write_text('constexpr int value = 2;\n')
    os.utime(value, ns=(old_time, old_time))
    build(2)
    assert header.read_bytes() != first_stamp, 'Backdated header did not invalidate native objects'
    unchanged_times = (header.stat().st_mtime_ns, executable.stat().st_mtime_ns)
    build(2)
    assert unchanged_times == (header.stat().st_mtime_ns, executable.stat().st_mtime_ns), 'Unchanged build was unnecessarily rewritten'
    value.write_bytes(original)
    os.utime(value, ns=(old_time, old_time))
    build(1)
    assert header.read_bytes() == first_stamp, 'Rollback did not restore the source fingerprint'
    main.write_text('int payload();\nint main() { return payload() + 2; }\n')
    os.utime(main, ns=(old_time, old_time))
    build(3)
    # Even if a stale archive is falsely newer than its objects, its fingerprint
    # must disagree with the current application and fail at link time.
    archive.write_bytes(stale_archive)
    future = time.time_ns() + 1_000_000_000
    os.utime(archive, ns=(future, future))
    command(['cmake', '--build', 'build', '--config', 'Release', '--parallel', '1', '--', '/nodeReuse:false'],
            expect_mismatch=True)
    print('Native compilation passed backdated header/source edits, rollback, unchanged builds and stale-archive rejection.')
finally:
    # Only remove this freshly allocated and resolved fixture inside run/.
    if fixture.is_relative_to(scratch_root):
        shutil.rmtree(fixture)
