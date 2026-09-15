#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/native/xma_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        ++failures; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static uint32_t fnv1a(const int16_t* p, size_t words) {
    uint32_t h = 2166136261u;
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < words * 2; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t beWord(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static int16_t beS16(float v) {
    if (!(v >= -1.0f) || !(v <= 1.0f)) v = (v != v) ? 0.0f : (v < 0.0f ? -1.0f : 1.0f);
    int32_t s = int32_t(lrintf(v * 32767.0f));
    if (s < -32768) s = -32768;
    if (s > 32767) s = 32767;
    return int16_t((s << 8) | ((s >> 8) & 0xFF));
}

struct Capture {
    uint32_t image[8];
    std::vector<uint8_t> pkt[2];
    int rateHz = 0;
    int channels = 0;
};

static bool loadCapture(const char* path, Capture& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open capture %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d;
    d.resize(size_t(n));
    if (fread(d.data(), 1, d.size(), f) != d.size()) { fclose(f); return false; }
    fclose(f);
    if (d.size() < 180 || memcmp(d.data(), "XMACAP01", 8) != 0) return false;
    const uint8_t* ctx = d.data() + 20 + 96;
    for (int i = 0; i < 8; ++i) out.image[i] = beWord(ctx + i * 4);
    size_t off = 20 + 96 + 64;
    for (int b = 0; b < 2; ++b) {
        if (off + 16 > d.size()) return false;
        const uint8_t* h = d.data() + off;
        uint32_t valid = h[0] | (uint32_t(h[1]) << 8) | (uint32_t(h[2]) << 16) | (uint32_t(h[3]) << 24);
        uint32_t count = h[4] | (uint32_t(h[5]) << 8) | (uint32_t(h[6]) << 16) | (uint32_t(h[7]) << 24);
        (void)valid;
        uint32_t take = h[12] | (uint32_t(h[13]) << 8) | (uint32_t(h[14]) << 16) | (uint32_t(h[15]) << 24);
        off += 16;
        if (take < 1) return false;
        if (off + 2048 > d.size()) return false;
        out.pkt[b].assign(d.begin() + off, d.begin() + off + 2048);
        off += 2048;
        (void)count;
    }
    uint32_t w1 = out.image[1];
    switch ((w1 >> 27) & 3u) {
        case 0: out.rateHz = 24000; break;
        case 1: out.rateHz = 32000; break;
        case 2: out.rateHz = 44100; break;
        default: out.rateHz = 48000; break;
    }
    out.channels = ((w1 >> 29) & 1u) ? 2 : 1;
    return true;
}

struct RefCtx {
    AVCodecContext* c = nullptr;
    std::vector<int16_t> pcm;
    ~RefCtx() { if (c) avcodec_free_context(&c); }
};

static bool refOpen(RefCtx& r, int rateHz, int channels) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA1);
    if (!codec) return false;
    r.c = avcodec_alloc_context3(codec);
    if (!r.c) return false;
    r.c->sample_rate = rateHz;
    r.c->block_align = 2048;
    av_channel_layout_default(&r.c->ch_layout, channels);
    static constexpr size_t kExtra = 28;
    r.c->extradata = (uint8_t*)av_mallocz(kExtra + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!r.c->extradata) return false;
    r.c->extradata[4] = 1;
    r.c->extradata[8 + 17] = uint8_t(channels);
    r.c->extradata_size = int(kExtra);
    if (avcodec_open2(r.c, codec, nullptr) < 0) return false;
    return true;
}

static int refSend(RefCtx& r, const uint8_t* data, int channels, bool& accepted) {
    accepted = false;
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return -1;
    if (av_new_packet(pkt, 2048) < 0) { av_packet_free(&pkt); return -1; }
    memcpy(pkt->data, data, 2048);
    int rc = avcodec_send_packet(r.c, pkt);
    av_packet_free(&pkt);
    if (rc == AVERROR(EAGAIN)) return 1;
    if (rc < 0) return -1;
    accepted = true;
    return 0;
}

static int refDrain(RefCtx& r, int channels, std::vector<int16_t>* out) {
    int frames = 0;
    for (;;) {
        AVFrame* f = av_frame_alloc();
        if (!f) return -1;
        int rc = avcodec_receive_frame(r.c, f);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) { av_frame_free(&f); break; }
        if (rc < 0) { av_frame_free(&f); return -1; }
        if (f->format != AV_SAMPLE_FMT_FLTP) { av_frame_free(&f); return -1; }
        const float* pl[2] = {(const float*)f->extended_data[0],
                              channels > 1 ? (const float*)f->extended_data[1] : nullptr};
        for (int s = 0; s < f->nb_samples; ++s)
            for (int c = 0; c < channels; ++c) {
                int16_t v = beS16(pl[c][s]);
                r.pcm.push_back(v);
                if (out) out->push_back(v);
            }
        ++frames;
        av_frame_free(&f);
    }
    return frames;
}

int main(int argc, char** argv) {
    const char* capPath = argc > 1 ? argv[1] : "build_native/run/xma-capture-000.bin";
    Capture cap;
    CHECK(loadCapture(capPath, cap), "load capture %s", capPath);
    if (failures) return 1;
    printf("capture %s rate=%d ch=%d pkt0=%zu pkt1=%zu\n", capPath, cap.rateHz, cap.channels,
           cap.pkt[0].size(), cap.pkt[1].size());
    CHECK(cap.rateHz == 24000 && cap.channels == 2, "capture must be 24k stereo, got %d/%d",
          cap.rateHz, cap.channels);
    CHECK(cap.pkt[0].size() == 2048 && cap.pkt[1].size() == 2048, "packet sizes");

    uint8_t* region = (uint8_t*)VirtualAlloc(nullptr, 16 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(region != nullptr, "VirtualAlloc base");
    if (!region) return 1;
    const uint8_t* base = region;
    const uint32_t kP0 = 0x10000, kP1 = 0x12000;
    memcpy(region + kP0, cap.pkt[0].data(), 2048);
    memcpy(region + kP1, cap.pkt[1].data(), 2048);

    std::vector<int16_t> big(65536 * 2);
    uint32_t consumed[2] = {0, 0};

    // A. streaming, no EOF, stepwise vs independent reference
    {
        XmaDecoderPool pool;
        RefCtx ref;
        CHECK(refOpen(ref, 24000, 2), "ref open");
        uint32_t img0[8];
        memcpy(img0, cap.image, sizeof(img0));
        img0[0] = (img0[0] & ~0x00200000u) | 0x00100000u;
        img0[0] = (img0[0] & ~0xFFFu) | 1u;
        img0[1] = (img0[1] & ~0xFFFu) | 0u;
        img0[5] = kP0;
        img0[6] = 0;
        uint64_t gen[2] = {11, 12};
        int n0 = pool.submit(7, 24000, 2, img0, base, big.data(), uint32_t(big.size() / 2), consumed, gen);
        CHECK(n0 >= 0, "pkt0 submit rc=%d", n0);
        CHECK(consumed[0] == 1 && consumed[1] == 0, "pkt0 consumed exactly once (%u,%u)",
              consumed[0], consumed[1]);
        bool acc = false;
        CHECK(refSend(ref, cap.pkt[0].data(), 2, acc) == 0 && acc, "ref pkt0 accepted");
        size_t refBefore = ref.pcm.size() / 2;
        CHECK(refDrain(ref, 2, nullptr) >= 0, "ref drain0");
        size_t refN0 = ref.pcm.size() / 2 - refBefore;
        CHECK(uint32_t(refN0) == uint32_t(n0), "pkt0 out matches ref: adapter=%d ref=%zu", n0, refN0);
        CHECK(memcmp(big.data(), ref.pcm.data() + refBefore * 2, size_t(n0) * 2 * 2) == 0,
              "pkt0 bytes match ref");
        printf("A1 first-packet accepted: consumed=(1,0) out=%d ref=%zu (NO EOF sent)\n", n0, refN0);

        uint32_t img1[8];
        memcpy(img1, cap.image, sizeof(img1));
        img1[0] = (img1[0] & ~0x00100000u) | 0x00200000u;
        img1[0] = (img1[0] & ~0xFFFu) | 0u;
        img1[1] = (img1[1] & ~0xFFFu) | 1u;
        img1[5] = 0;
        img1[6] = kP1;
        uint64_t gen1[2] = {11, 13};
        std::vector<int16_t> out1(65536 * 2);
        int n1 = pool.submit(7, 24000, 2, img1, base, out1.data(), uint32_t(out1.size() / 2), consumed, gen1);
        CHECK(n1 >= 0, "pkt1 submit rc=%d", n1);
        CHECK(consumed[0] == 0 && consumed[1] == 1, "pkt1 consumed (%u,%u)", consumed[0], consumed[1]);
        refBefore = ref.pcm.size() / 2;
        CHECK(refSend(ref, cap.pkt[1].data(), 2, acc) == 0 && acc, "ref pkt1 accepted");
        CHECK(refDrain(ref, 2, nullptr) >= 0, "ref drain1");
        size_t refN1 = ref.pcm.size() / 2 - refBefore;
        CHECK(uint32_t(refN1) == uint32_t(n1), "pkt1 out matches ref: adapter=%d ref=%zu", n1, refN1);
        if (n1 > 0)
            CHECK(memcmp(out1.data(), ref.pcm.data() + refBefore * 2, size_t(n1) * 2 * 2) == 0,
                  "pkt1 bytes match ref");
        printf("A2 continuation: consumed=(0,1) out=%d ref=%zu\n", n1, refN1);

        std::vector<int16_t> dr(65536 * 2);
        int nd = pool.drain(7, dr.data(), uint32_t(dr.size() / 2));
        CHECK(nd >= 0, "drain rc=%d", nd);
        refBefore = ref.pcm.size() / 2;
        CHECK(refDrain(ref, 2, nullptr) >= 0, "ref drain2");
        size_t refNd = ref.pcm.size() / 2 - refBefore;
        CHECK(uint32_t(refNd) == uint32_t(nd), "drain matches ref: adapter=%d ref=%zu", nd, refNd);
        int nd2 = pool.drain(7, dr.data(), uint32_t(dr.size() / 2));
        CHECK(nd2 == 0, "repeated zero-input drain stable (%d)", nd2);
        printf("A3 zero-input drain: first=%d ref=%zu repeat=%d\n", nd, refNd, nd2);
        printf("A streaming total (NO EOF): %d samples/ch checksum=%08x\n", n0 + n1 + nd,
               fnv1a(ref.pcm.data(), ref.pcm.size()));
    }

    // B. tiny output vs all-at-once
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] = (img[0] | 0x00300000u);
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] = (img[1] & ~0xFFFu) | 1u;
        img[5] = kP0;
        img[6] = kP1;
        uint64_t gen[2] = {21, 22};
        std::vector<int16_t> all(65536 * 2);
        uint32_t cAll[2] = {0, 0};
        int nAll = pool.submit(3, 24000, 2, img, base, all.data(), uint32_t(all.size() / 2), cAll, gen);
        CHECK(nAll >= 0, "all-at-once rc=%d", nAll);
        int dAll = pool.drain(3, all.data() + size_t(nAll) * 2, uint32_t(all.size() / 2 - size_t(nAll)));
        CHECK(dAll >= 0, "all drain=%d", dAll);

        XmaDecoderPool tiny;
        std::vector<int16_t> acc;
        uint32_t cT[2] = {0, 0};
        std::vector<int16_t> chunk(64 * 2);
        uint64_t genT[2] = {21, 22};
        int guard = 0;
        uint32_t seen[2] = {0, 0};
        for (;;) {
            int n = tiny.submit(4, 24000, 2, img, base, chunk.data(), 64, cT, genT);
            CHECK(n >= 0, "tiny submit rc=%d", n);
            seen[0] += cT[0];
            seen[1] += cT[1];
            acc.insert(acc.end(), chunk.begin(), chunk.begin() + size_t(n) * 2);
            int d = tiny.drain(4, chunk.data(), 64);
            CHECK(d >= 0, "tiny drain=%d", d);
            acc.insert(acc.end(), chunk.begin(), chunk.begin() + size_t(d) * 2);
            if (seen[0] >= 1 && seen[1] >= 1 && n == 0 && d == 0) break;
            if (++guard > 200) { CHECK(false, "tiny loop did not settle"); break; }
            if (n == 0 && d == 0) {
                if (seen[0] < 1 || seen[1] < 1) continue;
                break;
            }
        }
        int totalAll = nAll + dAll;
        CHECK(int(acc.size() / 2) == totalAll, "tiny total %zu == all %d", acc.size() / 2, totalAll);
        if (int(acc.size() / 2) == totalAll && totalAll > 0)
            CHECK(memcmp(acc.data(), all.data(), size_t(totalAll) * 2 * 2) == 0,
                  "tiny order/endian/count matches all-at-once");
        printf("B tiny-cap(64) total=%zu all=%d match=%s\n", acc.size() / 2, totalAll,
               (int(acc.size() / 2) == totalAll) ? "yes" : "NO");
    }

    // C. same address, new generation
    {
        XmaDecoderPool pool;
        const uint32_t kQ = 0x14000;
        memcpy(region + kQ, cap.pkt[0].data(), 2048);
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] = (img[0] & ~0x00200000u) | 0x00100000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] &= ~0xFFFu;
        img[5] = kQ;
        img[6] = 0;
        std::vector<int16_t> tmp(65536 * 2);
        uint32_t c[2] = {0, 0};
        uint64_t g0[2] = {100, 0};
        int r0 = pool.submit(5, 24000, 2, img, base, tmp.data(), uint32_t(tmp.size() / 2), c, g0);
        CHECK(r0 >= 0 && c[0] == 1, "gen100 first accept rc=%d c=(%u,%u)", r0, c[0], c[1]);
        memcpy(region + kQ, cap.pkt[1].data(), 2048);
        uint32_t c2[2] = {9, 9};
        int rSame = pool.submit(5, 24000, 2, img, base, tmp.data(), uint32_t(tmp.size() / 2), c2, g0);
        CHECK(rSame >= 0 && c2[0] == 0 && c2[1] == 0,
              "same-ptr same-gen must NOT re-decode: rc=%d c=(%u,%u)", rSame, c2[0], c2[1]);
        uint64_t g1[2] = {101, 0};
        uint32_t c3[2] = {0, 0};
        int rNew = pool.submit(5, 24000, 2, img, base, tmp.data(), uint32_t(tmp.size() / 2), c3, g1);
        CHECK(rNew >= 0 && c3[0] == 1, "same-ptr new-gen must decode: rc=%d c=(%u,%u)", rNew, c3[0], c3[1]);
        printf("C same-address generations: gen100 c=1, same-gen c=0, gen101 c=1\n");
        memcpy(region + kP0, cap.pkt[0].data(), 2048);
    }

    // D. malformed / truncated / null
    {
        XmaDecoderPool pool;
        std::vector<int16_t> tmp(1024 * 2);
        uint32_t c[2] = {0, 0};
        uint64_t g[2] = {1, 1};
        CHECK(pool.submit(0, 24000, 2, nullptr, base, tmp.data(), 512, c, g) ==
                  XmaDecoderPool::kError,
              "null image rejected");
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00100000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] &= ~0xFFFu;
        img[5] = 0xF0000000u;
        img[6] = 0;
        CHECK(pool.submit(0, 24000, 2, img, base, tmp.data(), 512, c, g) == XmaDecoderPool::kError,
              "unmapped ptr rejected");
        std::vector<uint8_t> garbage(2048, 0xFF);
        const uint32_t kG = 0x16000;
        memcpy(region + kG, garbage.data(), 2048);
        img[5] = kG;
        int rc = pool.submit(1, 24000, 2, img, base, tmp.data(), 512, c, g);
        CHECK(rc == XmaDecoderPool::kError || rc >= 0, "garbage no-crash only rc=%d", rc);
        pool.release(1);
        uint32_t c4[2] = {0, 0};
        img[5] = kP0;
        CHECK(pool.submit(1, 24000, 2, img, base, tmp.data(), 512, c4, g) >= 0 && c4[0] == 1,
              "slot reusable after release");
        printf("D no-crash malformed check (garbage assertion is no-crash only), slot reusable\n");
    }

    // E. release / reuse / clear
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00100000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] &= ~0xFFFu;
        img[5] = kP0;
        img[6] = 0;
        std::vector<int16_t> o1(8192 * 2), o2(8192 * 2);
        uint32_t c1[2] = {0, 0}, c2[2] = {0, 0};
        uint64_t g[2] = {7, 7};
        int r1 = pool.submit(9, 24000, 2, img, base, o1.data(), 8192, c1, g);
        CHECK(r1 >= 0 && c1[0] == 1, "pre-release accept");
        pool.release(9);
        CHECK(pool.pendingSamples(9) == 0, "pending cleared on release");
        CHECK(pool.drain(9, o2.data(), 8192) == XmaDecoderPool::kError, "drain after release errors");
        int r2 = pool.submit(9, 24000, 2, img, base, o2.data(), 8192, c2, g);
        CHECK(r2 == r1 && c2[0] == 1 && memcmp(o1.data(), o2.data(), size_t(r1) * 2 * 2) == 0,
              "post-release output identical (r1=%d r2=%d)", r1, r2);
        pool.clear();
        CHECK(pool.pendingSamples(9) == 0, "pending cleared on clear");
        printf("E release/reuse/clear ok\n");
    }

    // F. rate change needs explicit reset
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00100000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] &= ~0xFFFu;
        img[5] = kP0;
        img[6] = 0;
        std::vector<int16_t> tmp(8192 * 2);
        uint32_t c[2] = {0, 0};
        uint64_t g[2] = {3, 3};
        CHECK(pool.submit(10, 24000, 2, img, base, tmp.data(), 8192, c, g) >= 0, "24k open");
        uint32_t img32[8];
        memcpy(img32, cap.image, sizeof(img32));
        img32[0] |= 0x00300000u;
        img32[0] = (img32[0] & ~0xFFFu) | 1u;
        img32[1] = (img32[1] & ~0xFFFu) | 1u;
        img32[1] = (img32[1] & ~(3u << 27)) | (1u << 27);
        img32[5] = kP0;
        img32[6] = kP1;
        uint32_t c2[2] = {0, 0};
        CHECK(pool.submit(10, 32000, 2, img32, base, tmp.data(), 8192, c2, g) ==
                  XmaDecoderPool::kNeedReset,
              "rate change with accepted data needs reset");
        CHECK(c2[0] == 0 && c2[1] == 0, "need-reset consumed nothing");
        pool.release(10);
        uint32_t img0[8];
        memcpy(img0, cap.image, sizeof(img0));
        img0[0] &= ~0x00300000u;
        img0[1] &= ~0xFFFu;
        img0[1] = (img0[1] & ~(3u << 27)) | (1u << 27);
        uint32_t c3[2] = {9, 9};
        CHECK(pool.submit(10, 32000, 2, img0, base, tmp.data(), 8192, c3, g) == 0, "fresh rate after release ok");
        printf("F rate-change reset semantics ok\n");
    }

    // G. unsupported modes explicit
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00100000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] &= ~0xFFFu;
        img[5] = kP0;
        img[6] = 0;
        std::vector<int16_t> tmp(1024 * 2);
        uint32_t c[2] = {9, 9};
        uint64_t g[2] = {1, 1};
        uint32_t bad[8];
        memcpy(bad, img, sizeof(bad));
        bad[0] |= (1u << 12);
        CHECK(pool.submit(12, 24000, 2, bad, base, tmp.data(), 1024, c, g) ==
                  XmaDecoderPool::kUnsupported,
              "loop_count rejected");
        memcpy(bad, img, sizeof(bad));
        bad[3] = 0x100;
        CHECK(pool.submit(12, 24000, 2, bad, base, tmp.data(), 1024, c, g) ==
                  XmaDecoderPool::kUnsupported,
              "loop_start rejected");
        memcpy(bad, img, sizeof(bad));
        bad[1] |= (1u << 24);
        CHECK(pool.submit(12, 24000, 2, bad, base, tmp.data(), 1024, c, g) ==
                  XmaDecoderPool::kUnsupported,
              "subframe_skip rejected");
        memcpy(bad, img, sizeof(bad));
        bad[1] = (bad[1] & ~(3u << 27)) | (1u << 27);
        CHECK(pool.submit(12, 24000, 2, bad, base, tmp.data(), 1024, c, g) ==
                  XmaDecoderPool::kUnsupported,
              "rate-enum mismatch rejected");
        CHECK(c[0] == 0 && c[1] == 0, "unsupported consumed nothing");
        printf("G unsupported modes rejected explicitly\n");
    }

    // H. finite EOF drain, explicitly labeled, separate from streaming
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00300000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] = (img[1] & ~0xFFFu) | 1u;
        img[5] = kP0;
        img[6] = kP1;
        std::vector<int16_t> s(65536 * 2);
        uint32_t c[2] = {0, 0};
        uint64_t g[2] = {31, 32};
        int ns = pool.submit(11, 24000, 2, img, base, s.data(), uint32_t(s.size() / 2), c, g);
        CHECK(ns >= 0 && c[0] == 1 && c[1] == 1, "streaming feed both");
        int ds = pool.drain(11, s.data() + size_t(ns) * 2, uint32_t(s.size() / 2 - size_t(ns)));
        CHECK(ds >= 0, "streaming drain");
        int ds2 = pool.drain(11, s.data(), 1024);
        CHECK(ds2 == 0, "streaming stable without EOF (%d)", ds2);
        int streamTotal = ns + ds;
        std::vector<int16_t> tail(65536 * 2);
        int ne = pool.drainEofForTest(11, tail.data(), uint32_t(tail.size() / 2));
        CHECK(ne > 0, "EOF-labeled drain yields retained tail (%d)", ne);
        printf("H STREAMING(no EOF) total=%d/ch; EOF-LABELED tail=%d/ch; finite total=%d/ch\n", streamTotal,
               ne, streamTotal + ne);
        printf("H checksum streaming=%08x tail=%08x\n", fnv1a(s.data(), size_t(streamTotal) * 2),
               fnv1a(tail.data(), size_t(ne) * 2));
    }

    // J. read-bit offsets: 33/64 rejected before mutation, valid 32 follows
    {
        XmaDecoderPool pool;
        CHECK(cap.image[2] == 32, "capture read offset is the whole-packet 32 (%u)", cap.image[2]);
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00300000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] = (img[1] & ~0xFFFu) | 1u;
        img[5] = kP0;
        img[6] = kP1;
        std::vector<int16_t> tmp(65536 * 2);
        uint64_t g[2] = {41, 42};
        uint32_t bad[8];
        uint32_t c[2] = {9, 9};
        memcpy(bad, img, sizeof(bad));
        bad[2] = 33;
        CHECK(pool.submit(13, 24000, 2, bad, base, tmp.data(), 1024, c, g) ==
                  XmaDecoderPool::kUnsupported,
              "read-bit 33 rejected");
        CHECK(c[0] == 0 && c[1] == 0, "offset 33 consumed nothing");
        memcpy(bad, img, sizeof(bad));
        bad[2] = 64;
        CHECK(pool.submit(13, 24000, 2, bad, base, tmp.data(), 1024, c, g) ==
                  XmaDecoderPool::kUnsupported,
              "read-bit 64 rejected");
        CHECK(c[0] == 0 && c[1] == 0, "offset 64 consumed nothing");
        uint32_t c2[2] = {0, 0};
        int n = pool.submit(13, 24000, 2, img, base, tmp.data(), uint32_t(tmp.size() / 2), c2, g);
        CHECK(n >= 0 && c2[0] == 1 && c2[1] == 1, "valid 32 after rejections rc=%d c=(%u,%u)", n,
              c2[0], c2[1]);
        int d = pool.drain(13, tmp.data() + size_t(n) * 2, uint32_t(tmp.size() / 2 - size_t(n)));
        CHECK(d >= 0 && n + d == 3520, "post-rejection streaming total 3520 (got %d)", n + d);
        uint32_t c3[2] = {9, 9};
        int n2 = pool.submit(13, 24000, 2, img, base, tmp.data(), 1024, c3, g);
        CHECK(n2 >= 0 && c3[0] == 0 && c3[1] == 0, "repeat same gens no duplicate");
        uint32_t idle[8];
        memcpy(idle, cap.image, sizeof(idle));
        idle[0] &= ~0x00300000u;
        idle[1] &= ~0xFFFu;
        idle[2] = 99;
        uint32_t c4[2] = {9, 9};
        CHECK(pool.submit(18, 24000, 2, idle, base, tmp.data(), 1024, c4, g) == 0,
              "zero-input drain exempt from offset check");
        printf("J offsets 33/64 rejected cleanly, 32 works, drain exempt\n");
    }

    // K. terminal EOF: 64-sample chunks vs independent finite oracle
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00300000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] = (img[1] & ~0xFFFu) | 1u;
        img[5] = kP0;
        img[6] = kP1;
        std::vector<int16_t> s(65536 * 2);
        uint32_t c[2] = {0, 0};
        uint64_t g[2] = {51, 52};
        int ns = pool.submit(14, 24000, 2, img, base, s.data(), uint32_t(s.size() / 2), c, g);
        CHECK(ns >= 0 && c[0] == 1 && c[1] == 1, "K streaming feed");
        int ds = pool.drain(14, s.data() + size_t(ns) * 2, uint32_t(s.size() / 2 - size_t(ns)));
        CHECK(ds >= 0, "K streaming drain");
        int streamTotal = ns + ds;
        RefCtx ref;
        CHECK(refOpen(ref, 24000, 2), "K ref open");
        bool acc = false;
        CHECK(refSend(ref, cap.pkt[0].data(), 2, acc) == 0 && acc, "K ref pkt0");
        CHECK(refDrain(ref, 2, nullptr) >= 0, "K ref drain0");
        CHECK(refSend(ref, cap.pkt[1].data(), 2, acc) == 0 && acc, "K ref pkt1");
        CHECK(refDrain(ref, 2, nullptr) >= 0, "K ref drain1");
        CHECK(avcodec_send_packet(ref.c, nullptr) == 0, "K ref EOF send");
        CHECK(refDrain(ref, 2, nullptr) >= 0, "K ref EOF drain");
        size_t finiteTotal = ref.pcm.size() / 2;
        CHECK(finiteTotal >= size_t(streamTotal), "K finite oracle covers streaming (%zu >= %d)",
              finiteTotal, streamTotal);
        CHECK(memcmp(s.data(), ref.pcm.data(), size_t(streamTotal) * 2 * 2) == 0,
              "K streaming prefix matches finite oracle");
        std::vector<int16_t> tail;
        std::vector<int16_t> chunk(64 * 2);
        int guard = 0;
        for (;;) {
            int n = pool.drainEofForTest(14, chunk.data(), 64);
            CHECK(n >= 0, "K eof chunk rc=%d", n);
            tail.insert(tail.end(), chunk.begin(), chunk.begin() + size_t(n) * 2);
            if (n == 0) break;
            if (++guard > 1000) { CHECK(false, "K eof chunks did not terminate"); break; }
        }
        CHECK(int(tail.size() / 2) == int(finiteTotal) - streamTotal,
              "K 64-chunk tail %zu == oracle tail %zu", tail.size() / 2, finiteTotal - streamTotal);
        if (int(tail.size() / 2) == int(finiteTotal) - streamTotal && !tail.empty())
            CHECK(memcmp(tail.data(), ref.pcm.data() + size_t(streamTotal) * 2,
                         tail.size() * 2) == 0,
                  "K tail bytes match oracle");
        int e2 = pool.drainEofForTest(14, chunk.data(), 64);
        CHECK(e2 == 0, "K repeated finish stable (%d)", e2);
        uint32_t ce[2] = {9, 9};
        CHECK(pool.submit(14, 24000, 2, img, base, chunk.data(), 64, ce, g) ==
                  XmaDecoderPool::kNeedReset,
              "K submit-after-EOF needs reset");
        CHECK(ce[0] == 0 && ce[1] == 0, "K post-EOF submit consumed nothing");
        CHECK(pool.drain(14, chunk.data(), 64) == 0, "K drain-after-EOF stable");
        printf("K finite 64-chunks: stream=%d tail=%zu oracle=%zu terminal+NeedReset ok\n", streamTotal,
               tail.size() / 2, finiteTotal);
    }

    // K2. release/reuse with pending nonzero audio
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00300000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] = (img[1] & ~0xFFFu) | 1u;
        img[5] = kP0;
        img[6] = kP1;
        uint64_t g[2] = {55, 56};
        uint32_t c[2] = {0, 0};
        int n0 = pool.submit(15, 24000, 2, img, base, nullptr, 0, c, g);
        CHECK(n0 == 0 && c[0] == 1 && c[1] == 1, "K2 cap-0 prefetch consumed both (%u,%u)",
              c[0], c[1]);
        uint32_t pend = pool.pendingSamples(15);
        CHECK(pend > 0, "K2 pending nonzero after prefetch (%u)", pend);
        pool.release(15);
        std::vector<int16_t> a(65536 * 2), b(65536 * 2);
        uint32_t ca[2] = {0, 0}, cb[2] = {0, 0};
        int na = pool.submit(15, 24000, 2, img, base, a.data(), uint32_t(a.size() / 2), ca, g);
        int da = pool.drain(15, a.data() + size_t(na) * 2, uint32_t(a.size() / 2 - size_t(na)));
        int ea = pool.drainEofForTest(15, a.data() + size_t(na + da) * 2,
                                      uint32_t(a.size() / 2 - size_t(na + da)));
        int nb = pool.submit(16, 24000, 2, img, base, b.data(), uint32_t(b.size() / 2), cb, g);
        int db = pool.drain(16, b.data() + size_t(nb) * 2, uint32_t(b.size() / 2 - size_t(nb)));
        int eb = pool.drainEofForTest(16, b.data() + size_t(nb + db) * 2,
                                      uint32_t(b.size() / 2 - size_t(nb + db)));
        CHECK(na + da + ea == nb + db + eb && (na + da + ea) > 0, "K2 totals equal (%d vs %d)",
              na + da + ea, nb + db + eb);
        if (na + da + ea == nb + db + eb && na + da + ea > 0)
            CHECK(memcmp(a.data(), b.data(), size_t(na + da + ea) * 2 * 2) == 0,
                  "K2 post-release bytes identical");
        printf("K2 release/reuse with pending audio: totals %d identical\n", na + da + ea);
    }

    // K3. explicit finish with queued undrained output, small capacities
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00300000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] = (img[1] & ~0xFFFu) | 1u;
        img[5] = kP0;
        img[6] = kP1;
        uint64_t g[2] = {81, 82};
        uint32_t c[2] = {0, 0};
        int n0 = pool.submit(22, 24000, 2, img, base, nullptr, 0, c, g);
        CHECK(n0 == 0 && c[0] == 1 && c[1] == 1, "K3 prefetch consumed both");
        CHECK(pool.pendingSamples(22) > 0, "K3 queued output pending (%u)", pool.pendingSamples(22));
        RefCtx ref;
        CHECK(refOpen(ref, 24000, 2), "K3 ref open");
        bool acc = false;
        CHECK(refSend(ref, cap.pkt[0].data(), 2, acc) == 0 && acc, "K3 ref pkt0");
        CHECK(refDrain(ref, 2, nullptr) >= 0, "K3 ref drain0");
        CHECK(refSend(ref, cap.pkt[1].data(), 2, acc) == 0 && acc, "K3 ref pkt1");
        CHECK(refDrain(ref, 2, nullptr) >= 0, "K3 ref drain1");
        CHECK(avcodec_send_packet(ref.c, nullptr) == 0, "K3 ref EOF send");
        CHECK(refDrain(ref, 2, nullptr) >= 0, "K3 ref EOF drain");
        std::vector<int16_t> acc196;
        std::vector<int16_t> chunk(64 * 2);
        int guard = 0;
        for (;;) {
            int n = pool.drainEofForTest(22, chunk.data(), 64);
            CHECK(n >= 0, "K3 eof chunk rc=%d", n);
            acc196.insert(acc196.end(), chunk.begin(), chunk.begin() + size_t(n) * 2);
            if (n == 0) break;
            if (++guard > 1000) { CHECK(false, "K3 eof chunks did not terminate"); break; }
        }
        CHECK(acc196.size() == ref.pcm.size() && !acc196.empty(), "K3 chunked finish size %zu == oracle %zu",
              acc196.size(), ref.pcm.size());
        if (acc196.size() == ref.pcm.size() && !acc196.empty())
            CHECK(memcmp(acc196.data(), ref.pcm.data(), acc196.size() * 2) == 0,
                  "K3 chunked finish bytes match oracle");
        CHECK(pool.drainEofForTest(22, chunk.data(), 64) == 0, "K3 terminal repeat stable");
        printf("K3 queued-output finish: %zu samples/ch chunked == oracle\n", acc196.size() / 2);
    }

    // L. truncated mapped span at a protection boundary
    {
        uint8_t* pages = (uint8_t*)VirtualAlloc(nullptr, 12288, MEM_RESERVE, PAGE_NOACCESS);
        CHECK(pages != nullptr, "L reserve pages");
        CHECK(VirtualAlloc(pages, 8192, MEM_COMMIT, PAGE_READWRITE) != nullptr, "L commit two pages");
        const uint8_t* base2 = pages;
        const uint32_t kT = 8192 - 1024;
        memcpy(pages + kT, cap.pkt[0].data(), 1024);
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] = (img[0] & ~0x00200000u) | 0x00100000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] &= ~0xFFFu;
        img[5] = kT;
        img[6] = 0;
        std::vector<int16_t> tmp(1024 * 2);
        uint64_t g[2] = {71, 71};
        uint32_t c[2] = {9, 9};
        CHECK(pool.submit(19, 24000, 2, img, base2, tmp.data(), 1024, c, g) ==
                  XmaDecoderPool::kError,
              "L truncated span rejected");
        CHECK(c[0] == 0 && c[1] == 0, "L truncated span consumed nothing");
        memcpy(pages + 4096, cap.pkt[0].data(), 2048);
        img[5] = 4096;
        uint32_t c2[2] = {0, 0};
        CHECK(pool.submit(19, 24000, 2, img, base2, tmp.data(), 1024, c2, g) >= 0 && c2[0] == 1,
              "L in-bounds nonzero control accepted");
        VirtualFree(pages, 0, MEM_RELEASE);
        printf("L truncated-span rejected, in-bounds control accepted\n");
    }

    // M. current_buffer=1 with both buffers physically swapped, same packet order
    {
        XmaDecoderPool pool;
        const uint32_t kS0 = 0x18000, kS1 = 0x1A000;
        memcpy(region + kS0, cap.pkt[1].data(), 2048);
        memcpy(region + kS1, cap.pkt[0].data(), 2048);
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00300000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] = (img[1] & ~0xFFFu) | 1u;
        img[4] = 0x80000000u;
        img[5] = kS0;
        img[6] = kS1;
        std::vector<int16_t> s(65536 * 2), t(65536 * 2);
        uint64_t g[2] = {61, 62};
        uint32_t c[2] = {0, 0};
        int ns = pool.submit(20, 24000, 2, img, base, s.data(), uint32_t(s.size() / 2), c, g);
        CHECK(ns >= 0 && c[0] == 1 && c[1] == 1, "M swapped feed rc=%d c=(%u,%u)", ns, c[0], c[1]);
        int ds = pool.drain(20, s.data() + size_t(ns) * 2, uint32_t(s.size() / 2 - size_t(ns)));
        CHECK(ds >= 0 && ns + ds == 3520, "M swapped streaming total 3520 (got %d)", ns + ds);
        uint32_t c2[2] = {9, 9};
        CHECK(pool.submit(20, 24000, 2, img, base, t.data(), 1024, c2, g) >= 0 && c2[0] == 0 &&
                  c2[1] == 0,
              "M repeat same gens no duplicate (pre-EOF)");
        int es = pool.drainEofForTest(20, s.data() + size_t(ns + ds) * 2,
                                      uint32_t(s.size() / 2 - size_t(ns + ds)));
        CHECK(es == 4288, "M swapped finite tail 4288 (got %d)", es);
        uint32_t imgN[8];
        memcpy(imgN, cap.image, sizeof(imgN));
        imgN[0] |= 0x00300000u;
        imgN[0] = (imgN[0] & ~0xFFFu) | 1u;
        imgN[1] = (imgN[1] & ~0xFFFu) | 1u;
        imgN[5] = kP0;
        imgN[6] = kP1;
        uint32_t cn[2] = {0, 0};
        int nn = pool.submit(21, 24000, 2, imgN, base, t.data(), uint32_t(t.size() / 2), cn, g);
        int dn = pool.drain(21, t.data() + size_t(nn) * 2, uint32_t(t.size() / 2 - size_t(nn)));
        CHECK(nn + dn == ns + ds && memcmp(s.data(), t.data(), size_t(nn + dn) * 2 * 2) == 0,
              "M swapped order == normal order");
        uint32_t c3[2] = {9, 9};
        CHECK(pool.submit(20, 24000, 2, img, base, t.data(), 1024, c3, g) ==
                  XmaDecoderPool::kNeedReset,
              "M submit after terminal EOF needs reset");
        printf("M current_buffer=1 swapped == normal order, tail 4288, terminal ok\n");
    }

    // I. pool destruction / many slots
    {
        XmaDecoderPool pool;
        uint32_t img[8];
        memcpy(img, cap.image, sizeof(img));
        img[0] |= 0x00100000u;
        img[0] = (img[0] & ~0xFFFu) | 1u;
        img[1] &= ~0xFFFu;
        img[5] = kP0;
        img[6] = 0;
        std::vector<int16_t> tmp(512 * 2);
        uint64_t g[2] = {1, 1};
        for (uint32_t s = 0; s < 16; ++s) {
            uint32_t c[2] = {0, 0};
            CHECK(pool.submit(s, 24000, 2, img, base, tmp.data(), 512, c, g) >= 0, "slot %u", s);
        }
        pool.clear();
        for (uint32_t s = 0; s < 16; ++s) CHECK(pool.pendingSamples(s) == 0, "clear slot %u", s);
        printf("I multi-slot + clear ok\n");
    }

    VirtualFree(region, 0, MEM_RELEASE);
    if (failures) {
        printf("XMA-ADAPTER-TESTS FAILED (%d)\n", failures);
        return 1;
    }
    printf("XMA-ADAPTER-TESTS PASSED\n");
    return 0;
}
