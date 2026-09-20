"""Build a pinned, minimal FFmpeg with an opt-in single-stream raw XMA mode.

Upstream codec math is unchanged. Container priming, FIFO delay and EOF tail
generation are bypassed only when darkrecomp_raw_frames=1. LGPL source and a
reviewable patch remain beside the DLLs. Requires existing MSYS2 MinGW + LLVM.
"""
from pathlib import Path
import difflib
import hashlib
import json
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / 'build_native/deps'
COMMIT = '1c2c67c0b9f7f66ab32c19dcf7f227bcd290aa4c'  # FFmpeg n8.1.2
ARCHIVE_HASH = '1291ae49c285f7bd55c7c059aa43f1a0fd784a1ae22d5c76297dcd11c531248a'
SOURCE_HASH = '803547a38dea1294891c00402d6b3576a16053b0f00b395768c4983740c86553'

def main():
    DEPS.mkdir(parents=True, exist_ok=True)
    archive = DEPS / 'ffmpeg-darkxma-upstream.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve(f'https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/{COMMIT}', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_HASH:
        raise RuntimeError('Pinned FFmpeg archive hash mismatch')
    source = DEPS / ('FFmpeg-' + COMMIT)
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(DEPS, filter='data')
    path = source / 'libavcodec/wmaprodec.c'
    # Always derive the patch from the pinned original, never mutate an unknown file.
    with tarfile.open(archive) as tar:
        original = tar.extractfile(f'FFmpeg-{COMMIT}/libavcodec/wmaprodec.c').read()
    if hashlib.sha256(original).hexdigest() != SOURCE_HASH:
        raise RuntimeError('Pinned decoder source hash mismatch')
    text = original.decode()
    text = text.replace('#include "libavutil/mem.h"', '#include "libavutil/mem.h"\n#include "libavutil/opt.h"')
    text = text.replace('typedef struct XMADecodeCtx {', '''typedef struct XMADecodeCtx {
    const AVClass *class;
    int raw_frames;''')
    anchor = '    int i, ret = 0, eof = 0;'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '''
    /* DarkRecomp opt-in: expose every mathematically decoded 512-sample frame.
     * The engine owns priming and sample extents. Empty input is not a source
     * of synthetic overlap tail. Packet reservoir and IMDCT stay upstream. */
    if (s->raw_frames) {
        if (!avpkt->size) { *got_frame_ptr = 0; return 0; }
        frame->nb_samples = 512;
        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) return ret;
        return decode_packet(avctx, &s->xma[0], frame, got_frame_ptr, avpkt);
    }
''')
    anchor = '    /* init all streams (several streams of 1/2ch make Nch files) */'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '''    if (s->raw_frames && s->num_streams != 1)
        return AVERROR(EINVAL);
''' + anchor)
    text = text.replace('        s->frames[i] = av_frame_alloc();',
                        '        if (s->raw_frames) s->xma[i].skip_frame = 0;\n        s->frames[i] = av_frame_alloc();')
    text = text.replace('    s->current_stream = 0;\n    s->flushed = 0;',
                        '    if (s->raw_frames) s->xma[0].skip_frame = 0;\n    s->current_stream = 0;\n    s->flushed = 0;')
    options = '''static const AVOption darkrecomp_xma_options[] = {
    { "darkrecomp_raw_frames", "Return raw XMA frames without container delay or trimming",
      offsetof(XMADecodeCtx, raw_frames), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1,
      AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};
static const AVClass darkrecomp_xma_class = {
    .class_name = "DarkRecomp XMA",
    .item_name = av_default_item_name,
    .option = darkrecomp_xma_options,
    .version = LIBAVUTIL_VERSION_INT,
};

'''
    text = text.replace('const FFCodec ff_xma1_decoder = {', options + 'const FFCodec ff_xma1_decoder = {')
    text = text.replace('    .priv_data_size = sizeof(XMADecodeCtx),',
                        '    .p.priv_class   = &darkrecomp_xma_class,\n    .priv_data_size = sizeof(XMADecodeCtx),')
    current = path.read_bytes()
    if current not in (original, text.encode()):
        raise RuntimeError('Unexpected local edits to FFmpeg decoder; refusing overwrite')
    if current != text.encode():
        path.write_text(text, newline='\n')
    patch = ''.join(difflib.unified_diff(original.decode().splitlines(True), text.splitlines(True),
                                        fromfile='a/libavcodec/wmaprodec.c', tofile='b/libavcodec/wmaprodec.c'))
    (DEPS / 'ffmpeg-darkxma.patch').write_text(patch)
    build = DEPS / 'ffmpeg-darkxma-build'
    install = DEPS / 'ffmpeg-darkxma'
    build.mkdir(exist_ok=True)
    def msys(path):
        path = path.as_posix()
        return '/' + path[0].lower() + path[2:]
    configure = [msys(source / 'configure'), '--prefix=' + msys(install), '--target-os=mingw32',
                 '--arch=x86_64', '--cc=gcc', '--disable-everything', '--disable-autodetect',
                 '--disable-programs', '--disable-doc', '--disable-network', '--disable-debug',
                 '--disable-avdevice', '--disable-avfilter', '--disable-avformat',
                 '--disable-swscale', '--disable-swresample', '--disable-asm',
                 '--disable-pthreads', '--enable-w32threads',
                 '--enable-decoder=xma1,xma2', '--enable-avcodec', '--enable-avutil',
                 '--enable-shared', '--disable-static', '--build-suffix=-darkxma',
                 '--extra-ldflags=-static-libgcc']
    script = 'set -eu\nexport PATH=/mingw64/bin:/usr/bin\n'
    script += 'cd ' + shlex.quote(msys(build)) + '\n'
    config_stamp = build / 'darkrecomp-configure.json'
    if not config_stamp.exists() or json.loads(config_stamp.read_text()) != configure:
        script += ' '.join(map(shlex.quote, configure)) + '\n'
    script += 'make -j8\nmake install\n'
    subprocess.run(['C:/msys64/usr/bin/bash.exe', '-c', script], check=True)
    config_stamp.write_text(json.dumps(configure))
    for name, version in [('avcodec', 62), ('avutil', 60)]:
        exports = build / ('lib' + name) / f'{name}-darkxma-{version}.def'
        definition = install / 'lib' / f'{name}-darkxma-import.def'
        definition.write_text(f'LIBRARY {name}-darkxma-{version}.dll\n' + exports.read_text())
        subprocess.run(['C:/Program Files/LLVM/bin/llvm-lib.exe', '/machine:x64',
                        '/def:' + str(definition), '/out:' + str(install / 'lib' / (name + '-darkxma.lib'))], check=True)
    (install / 'PROVENANCE.json').write_text(json.dumps({
        'upstream_commit': COMMIT, 'archive_sha256': ARCHIVE_HASH,
        'original_decoder_sha256': SOURCE_HASH, 'patched_decoder_sha256': hashlib.sha256(text.encode()).hexdigest(),
        'configure': configure, 'license': 'LGPL-2.1-or-later',
    }, indent=2) + '\n')
    for filename in ['COPYING.LGPLv2.1', 'LICENSE.md']:
        (install / filename).write_bytes((source / filename).read_bytes())
    # MinGW's clock_gettime/nanosleep implementations remain a runtime dependency
    # even with the decoder using Win32 threads. Ship the existing runtime and license.
    shutil.copy2('C:/msys64/mingw64/bin/libwinpthread-1.dll', install / 'bin')
    shutil.copy2('C:/msys64/mingw64/share/licenses/winpthreads/COPYING', install / 'COPYING.winpthreads')

if __name__ == '__main__':
    main()
