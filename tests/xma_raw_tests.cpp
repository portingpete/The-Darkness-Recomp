#include "runtime/native/xma_raw_decoder.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}
using namespace DarkRecomp::Native;
static void check(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
template<class T> static T symbol(HMODULE dll, const char* name) {
    auto p = GetProcAddress(dll, name); check(p != nullptr, name); return reinterpret_cast<T>(p);
}
static uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static std::vector<float> stock(const std::vector<uint8_t>& packets, bool finish) {
    // Separate unmodified stock DLL; no patched functions/private-structure casts.
    HMODULE codecDll = LoadLibraryA("avcodec-62.dll");
    HMODULE utilDll = LoadLibraryA("avutil-60.dll");
    check(codecDll && utilDll, "Stock oracle DLL load failed");
#define API(dll, f) auto f = symbol<decltype(&::f)>(dll, #f)
    API(codecDll, avcodec_find_decoder); API(codecDll, avcodec_alloc_context3);
    API(codecDll, avcodec_open2); API(codecDll, avcodec_free_context);
    API(codecDll, avcodec_send_packet); API(codecDll, avcodec_receive_frame);
    API(codecDll, av_packet_alloc); API(codecDll, av_new_packet); API(codecDll, av_packet_free);
    API(utilDll, av_mallocz); API(utilDll, av_channel_layout_default);
    API(utilDll, av_frame_alloc); API(utilDll, av_frame_unref); API(utilDll, av_frame_free);
#undef API
    auto* codec = avcodec_find_decoder(AV_CODEC_ID_XMA1);
    auto* c = avcodec_alloc_context3(codec);
    check(c != nullptr, "Oracle allocation failed");
    c->sample_rate = 24000; c->block_align = 2048;
    av_channel_layout_default(&c->ch_layout, 2);
    c->extradata = static_cast<uint8_t*>(av_mallocz(28 + AV_INPUT_BUFFER_PADDING_SIZE));
    check(c->extradata != nullptr, "Oracle extra allocation failed");
    c->extradata_size = 28; c->extradata[4] = 1; c->extradata[25] = 2;
    check(avcodec_open2(c, codec, nullptr) == 0, "Oracle open failed");
    AVFrame* frame = av_frame_alloc();
    std::vector<float> pcm;
    auto drain = [&] {
        for (;;) {
            int rc = avcodec_receive_frame(c, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
            check(rc == 0 && frame->format == AV_SAMPLE_FMT_FLTP, "Oracle decode failed");
            for (int i = 0; i < frame->nb_samples; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    pcm.push_back(reinterpret_cast<float*>(frame->extended_data[ch])[i]);
            av_frame_unref(frame);
        }
    };
    for (size_t offset = 0; offset < packets.size(); offset += 2048) {
        AVPacket* packet = av_packet_alloc();
        check(packet && av_new_packet(packet, 2048) == 0, "Oracle packet allocation failed");
        memcpy(packet->data, packets.data() + offset, 2048);
        check(avcodec_send_packet(c, packet) == 0, "Oracle packet rejected");
        av_packet_free(&packet); drain();
    }
    if (finish) { check(avcodec_send_packet(c, nullptr) == 0, "Oracle EOF rejected"); drain(); }
    av_frame_free(&frame); avcodec_free_context(&c);
    FreeLibrary(codecDll); FreeLibrary(utilDll);
    return pcm;
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Capture required");
        std::ifstream file(argv[1], std::ios::binary);
        std::vector<uint8_t> capture((std::istreambuf_iterator<char>(file)), {});
        check(capture.size() >= 180 && !memcmp(capture.data(), "XMACAP01", 8), "Capture invalid");
        size_t at = 180;
        std::vector<uint8_t> packets;
        for (int b = 0; b < 2; ++b) {
            check(at + 16 <= capture.size(), "Missing packet descriptor");
            uint32_t n = le32(capture.data() + at + 12); at += 16;
            check(n == 1 && at + 2048 <= capture.size(), "Expected two captured packets");
            packets.insert(packets.end(), capture.begin() + at, capture.begin() + at + 2048);
            at += 2048;
        }
        auto decode = [&](uint32_t chunk) {
            XmaRawDecoder decoder;
            check(decoder.open(24000, 2), "Raw decoder option/init failed");
            std::vector<float> output;
            float frame[1024];
            for (size_t offset = 0; offset < packets.size(); offset += 2048) {
                check(decoder.push(packets.data() + offset) == 0, "Raw packet rejected");
                size_t before = output.size();
                for (;;) {
                    int n = decoder.read(frame, chunk);
                    check(n >= 0, "Raw read failed");
                    if (!n) break;
                    output.insert(output.end(), frame, frame + n * 2);
                }
                check(output.size() > before, "Packet still held behind container FIFO delay");
                check(decoder.read(frame, chunk) == 0, "Input exhaustion synthesized PCM");
            }
            return output;
        };
        auto raw = decode(512), small = decode(127);
        check(raw == small && raw.size() > 8192, "Partial-frame reads lost or replayed samples");
        auto streaming = stock(packets, false), finite = stock(packets, true);
        check(!streaming.empty() && finite.size() > streaming.size(), "Stock oracle lacks expected retained tail");
        // Stock skips the first decoded 512-frame plus 64 samples. Compare all
        // overlapping mathematical output, excluding its synthesized EOF tail.
        constexpr size_t skip = 576 * 2;
        size_t overlap = (std::min)(raw.size() - skip, finite.size());
        float maxError = 0;
        for (size_t i = 0; i < overlap; ++i) maxError = (std::max)(maxError, std::abs(raw[skip + i] - finite[i]));
        check(overlap >= streaming.size() && maxError < 0.00001f, "Raw PCM differs from independent stock overlap");
        check(std::any_of(raw.begin(), raw.end(), [](float f) { return std::abs(f) > 0.001f; }), "Raw output is silence");
        printf("Raw %zu/ch; stock streaming %zu/ch, finite %zu/ch; overlap %zu/ch max error %.9g.\n",
               raw.size()/2, streaming.size()/2, finite.size()/2, overlap/2, maxError);
        puts("Raw first-packet output, continued decoding, bounded partial reads and no-EOF exhaustion verified.");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "TEST FAILED: %s\n", e.what()); return 1; }
}
