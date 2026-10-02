"""Contract tests for the isolated WSL pressure-vessel graphics hook."""
from pathlib import Path
import stat
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import wsl_graphics as hook

PARENT = '/home/test/runtime'
SOURCE = PARENT + '/tmp-ABC123/usr'
BINARY = '/home/test/steamrt/pressure-vessel/srt-bwrap'
SETTINGS = hook.Settings(PARENT, BINARY)
ENTRY = sorted(hook.ADVERBS)[0]
ARGV = ['--args', '27', ENTRY, '--prefix=/usr/lib/pressure-vessel/from-host',
        '--set-ld-library-path', '/usr/lib/pressure-vessel/overrides/aliases',
        '--', '/home/test/proton', 'run', 'Game.exe']
OPTIONS = ['--ro-bind', '/etc', '/etc', '--bind', SOURCE, '/usr',
           '--symlink', 'usr/bin', '/bin', '--unshare-pid']
MOUNTINFO = '1 0 8:1 / / rw - ext4 /dev/sda rw\n'


def fake_stat(path):
    return SimpleNamespace(st_mode=(stat.S_IFREG | 0o755) if path == BINARY else (stat.S_IFDIR | 0o700),
                           st_uid=1000, st_file_attributes=0)


class WslGraphicsTests(unittest.TestCase):
    def test_runtime_copy_overlay_is_ephemeral_and_wsl_bind_readonly(self):
        original_argv, original_options = ARGV.copy(), OPTIONS.copy()
        result = hook.rewrite(ARGV, OPTIONS, SETTINGS)
        self.assertTrue(result.changed)
        self.assertEqual(result.source, SOURCE)
        self.assertEqual(result.options, ['--ro-bind', '/etc', '/etc',
            '--overlay-src', SOURCE, '--tmp-overlay', '/usr', '--symlink', 'usr/bin', '/bin',
            '--unshare-pid', '--ro-bind', '/usr/lib/wsl', '/usr/lib/wsl', '--remount-ro', '/usr'])
        self.assertEqual(result.argv[5], '/usr/lib/wsl/lib:/usr/lib/pressure-vessel/overrides/aliases')
        self.assertEqual(ARGV, original_argv)
        self.assertEqual(OPTIONS, original_options)

    def test_readonly_runtime_bind_and_second_supported_adverb(self):
        options = OPTIONS.copy()
        options[3] = '--ro-bind'
        for entry in hook.ADVERBS:
            argv = ARGV.copy()
            argv[2] = entry
            self.assertTrue(hook.rewrite(argv, options, SETTINGS).changed)

    def test_game_argv_metacharacters_empty_strings_and_option_names_preserved(self):
        payload = ['', '$(touch /tmp/no)', '`rm -rf /`', ';', 'a b', 'c\\d',
                   '--args', '99', '--set-ld-library-path', 'game-owned-value', '--']
        result = hook.rewrite(ARGV + payload, OPTIONS, SETTINGS)
        self.assertEqual(result.argv[6:], (ARGV + payload)[6:])
        self.assertNotIn('--overlay', result.options)

    def test_nul_payload_roundtrip_preserves_empty_and_non_utf8_arguments(self):
        data = b'--symlink\0\0/tmp/a b\0--setenv\0NAME\0\xff\0'
        decoded = hook.decode_args(data)
        self.assertEqual(b'\0'.join(item.encode('utf-8', 'surrogateescape') for item in decoded) + b'\0', data)

    def test_malformed_payload_and_descriptors_rejected(self):
        for data in (b'', b'--ro-bind', b'a' * (hook.MAX_ARGS_BYTES + 1) + b'\0'):
            with self.subTest(data_size=len(data)), self.assertRaises(hook.HookError):
                hook.decode_args(data)
        for argv in (['--args'], ['--args', '3'], ['--args', '-1', ENTRY],
                     ['--args', '2', ENTRY], ['--args', '3x', ENTRY], ['--args', '2147483648', ENTRY]):
            with self.subTest(argv=argv), self.assertRaises(hook.HookError):
                hook.args_fd(argv)
        with self.assertRaises(hook.HookError):
            hook.rewrite(ARGV, None, SETTINGS)

    def test_missing_bind_arguments_nested_args_and_unknown_options_rejected(self):
        for options in (['--bind', SOURCE], ['--args', '8'], ['--unknown', 'foo']):
            with self.subTest(options=options), self.assertRaises(hook.HookError):
                hook.rewrite(ARGV, options, SETTINGS)

    def test_only_direct_private_tmp_usr_source_accepted(self):
        for source in ('/usr', '/home/test/runtime-other/tmp-ABC123/usr',
                       PARENT + '/nested/tmp-ABC123/usr', PARENT + '/tmp-ABC123/usr/lib',
                       PARENT + '/tmp-ABC123/../usr', PARENT + '/tmp-abc/usr',
                       'relative/tmp-ABC123/usr', SOURCE + '/', '/' + SOURCE, SOURCE.replace('/tmp', '//tmp')):
            with self.subTest(source=source), self.assertRaises(hook.HookError):
                hook.runtime_source(source, PARENT)
        self.assertEqual(hook.runtime_source(SOURCE, PARENT), SOURCE)

    def test_multiple_or_unexpected_usr_mount_rejected(self):
        for options in (OPTIONS + ['--ro-bind', '/usr', '/usr'],
                       ['--dev-bind', SOURCE, '/usr'], ['--ro-bind-try', SOURCE, '/usr'],
                       ['--tmpfs', '/usr'], ['--symlink', '/run/host/usr', '/usr'],
                       ['--ro-bind', SOURCE, '/usr/'], ['--ro-bind', SOURCE, '/x/../usr']):
            with self.subTest(options=options), self.assertRaises(hook.HookError):
                hook.rewrite(ARGV, options, SETTINGS)

    def test_unexpected_entry_or_library_setter_rejected(self):
        malformed = [ARGV[:2] + ['/bin/sh'] + ARGV[3:],
                     ARGV[:4] + ARGV[6:], ARGV[:5] + ARGV[6:],
                     ARGV[:6] + ['--set-ld-library-path', '/other'] + ARGV[6:],
                     ARGV[:6] + ARGV[7:]]
        for argv in malformed:
            with self.subTest(argv=argv), self.assertRaises(hook.HookError):
                hook.rewrite(argv, OPTIONS, SETTINGS)

    def test_library_path_keeps_other_entries_and_deduplicates_wsl(self):
        argv = ARGV.copy()
        argv[5] = '/usr/lib/wsl/lib:/own path:/usr/lib/wsl/lib:'
        result = hook.rewrite(argv, OPTIONS, SETTINGS)
        self.assertEqual(result.argv[5], '/usr/lib/wsl/lib:/own path:')

    def test_bubblewrap_selftest_and_runtime_readlink_probes_pass_unchanged(self):
        probes = [['--ro-bind', '/usr', '/usr', '--', '/bin/true'],
                  ['--ro-bind', SOURCE, '/usr', '--symlink', 'usr/bin', '/bin',
                   'readlink', '-e', '/lib64/ld-linux-x86-64.so.2']]
        for argv in probes:
            result = hook.rewrite(argv, None, SETTINGS)
            self.assertFalse(result.changed)
            self.assertEqual(result.argv, argv)
        self.assertEqual(hook.rewrite(probes[1], None, SETTINGS).source, SOURCE)

    def test_unbundled_usr_program_is_not_treated_as_setup_probe(self):
        with self.assertRaises(hook.HookError):
            hook.rewrite(['--ro-bind', SOURCE, '/usr', '/bin/sh'], None, SETTINGS)

    def test_nonruntime_helper_namespace_and_mounts_preserved(self):
        argv = ['--ro-bind', SOURCE, '/tmp/mnt/usr', '--unshare-user', '--unshare-pid',
                '--die-with-parent', '/bin/true']
        result = hook.rewrite(argv, None, SETTINGS)
        self.assertEqual(result.argv, argv)
        self.assertFalse(result.changed)

    def test_existing_other_overlay_does_not_replace_validated_source(self):
        options = ['--overlay-src', '/some/other', '--tmp-overlay', '/other', *OPTIONS]
        self.assertEqual(hook.rewrite(ARGV, options, SETTINGS).source, SOURCE)

    def test_ancestors_checked_without_following_links_or_reparse_points(self):
        visited = []
        def symlink(path):
            visited.append(path)
            info = fake_stat(path)
            if path == '/home/test':
                info.st_mode = stat.S_IFLNK | 0o777
            return info
        with self.assertRaises(hook.HookError):
            hook.path_without_links(SOURCE, directory=True, lstat=symlink)
        self.assertEqual(visited, ['/', '/home', '/home/test'])
        def reparse(path):
            info = fake_stat(path)
            if path == PARENT:
                info.st_file_attributes = 0x400
            return info
        with self.assertRaises(hook.HookError):
            hook.path_without_links(SOURCE, directory=True, lstat=reparse)

    def test_private_ext4_configuration_validates(self):
        hook.validate_settings(SETTINGS, lstat=fake_stat, mountinfo=MOUNTINFO, uid=1000)
        for uid in (1001, 0):
            with self.assertRaises(hook.HookError):
                hook.validate_settings(SETTINGS, lstat=fake_stat, mountinfo=MOUNTINFO, uid=uid)
        for mountinfo in (MOUNTINFO.replace('ext4', '9p'), ''):
            with self.assertRaises(hook.HookError):
                hook.validate_settings(SETTINGS, lstat=fake_stat, mountinfo=mountinfo, uid=1000)

    def test_binary_parent_permissions_and_private_log_directory_rejected(self):
        def writable(path):
            info = fake_stat(path)
            if path == PARENT:
                info.st_mode |= 0o020
            return info
        with self.assertRaises(hook.HookError):
            hook.validate_settings(SETTINGS, lstat=writable, mountinfo=MOUNTINFO, uid=1000)
        with self.assertRaises(hook.HookError):
            hook.validate_settings(hook.Settings(PARENT, '/usr/bin/bwrap'), lstat=fake_stat,
                                   mountinfo=MOUNTINFO, uid=1000)
        def public_log(path):
            info = fake_stat(path)
            if path == '/home/test/logs':
                info.st_mode |= 0o005
            return info
        with self.assertRaises(hook.HookError):
            hook.validate_settings(hook.Settings(PARENT, BINARY, '/home/test/logs'), lstat=public_log,
                                   mountinfo=MOUNTINFO, uid=1000)

    def test_mount_selection_honors_boundaries_and_escaped_spaces(self):
        mounts = MOUNTINFO + '2 1 0:1 / /home/test/runtime-other rw - 9p drvfs rw\n'
        self.assertEqual(hook.filesystem_type(PARENT, mounts), 'ext4')
        mounts += '3 1 0:2 / /home/test/runtime rw - 9p drvfs rw\n'
        self.assertEqual(hook.filesystem_type(SOURCE, mounts), '9p')
        mounts += '4 1 8:2 / /private\\040space rw - ext4 /dev/sdb rw\n'
        self.assertEqual(hook.filesystem_type('/private space/runtime', mounts), 'ext4')

    def test_embedded_nul_rejected_before_execution(self):
        with self.assertRaises(hook.HookError):
            hook.rewrite(ARGV + ['bad\0arg'], OPTIONS, SETTINGS)


if __name__ == '__main__':
    unittest.main()
