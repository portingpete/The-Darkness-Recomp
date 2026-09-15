#include "xma_decoder.h"
#include <windows.h>
#include <cmath>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace {
bool xmaGuard(const uint8_t* base, uint32_t address, uint32_t bytes) {
    if (!base || !bytes || uint64_t(address) + bytes > 0x100000000ull) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(base + address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto regionEnd = reinterpret_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    return base + address + bytes <= regionEnd;
}
bool xmaRateOk(int rateHz) {
    return rateHz == 24000 || rateHz == 32000 || rateHz == 44100 || rateHz == 48000;
}
int xmaEnumRate(uint32_t bits) {
    switch (bits & 3u) {
        case 0: return 24000;
        case 1: return 32000;
        case 2: return 44100;
        default: return 48000;
    }
}
int16_t xmaConvertSample(float v) {
    int32_t s = int32_t(lrintf(v * 32767.0f));
    if (s < -32768) s = -32768;
    if (s > 32767) s = 32767;
    return int16_t((s << 8) | ((s >> 8) & 0xFF));
}
bool xmaFrameFinite(const AVFrame* frame, int channels) {
    for (int c = 0; c < channels; ++c) {
        const float* p = (const float*)frame->extended_data[c];
        for (int s = 0; s < frame->nb_samples; ++s) {
            if (!isfinite(p[s])) return false;
        }
    }
    return true;
}
void xmaFreePending(XmaStreamDecoder* state) {
    if (state->pending) {
        av_frame_free(&state->pending);
        state->pending = nullptr;
    }
    state->pendingOff = 0;
}
void xmaResetStream(XmaStreamDecoder* state) {
    state->packetsAccepted = 0;
    state->eofSent = false;
    state->eofTerminal = false;
    state->inPtr[0] = state->inPtr[1] = 0;
    state->inDone[0] = state->inDone[1] = 0;
    state->inGen[0] = state->inGen[1] = 0;
    state->inSeen[0] = state->inSeen[1] = false;
    state->spill.clear();
    xmaFreePending(state);
}
}  // namespace

AVCodecContext* XmaDecoderPool::openDecoder(int rateHz, int channels) {
    if (!xmaRateOk(rateHz) || channels < 1 || channels > 2) return nullptr;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA1);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;
    ctx->sample_rate = rateHz;
    ctx->block_align = int(kPacketBytes);
    av_channel_layout_default(&ctx->ch_layout, channels);
    static constexpr size_t kExtra = 28;
    ctx->extradata = (uint8_t*)av_mallocz(kExtra + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->extradata) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    ctx->extradata[4] = 1;
    ctx->extradata[8 + 17] = uint8_t(channels);
    ctx->extradata_size = int(kExtra);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

namespace {
// Single shared output pump used by submit, drain and drainEofForTest.
// Moves owned spill, then the owned pending frame, then newly received codec
// frames into outBE (up to cap) and the bounded spill. A partially emitted
// frame is retained with its cursor; nothing is ever discarded.
// Returns kError, 0 (progress or nothing available), or 1 (blocked: output
// full, spill full, frame still held). When eofMode, sets terminal once the
// codec reports EOF and no frame is held.
int xmaPump(XmaStreamDecoder* state, int16_t* outBE, uint32_t cap, uint32_t& written, bool eofMode) {
    const int channels = state->channels;
    auto spillHave = [&]() { return uint32_t(state->spill.size() / size_t(channels)); };
    auto spillRoom = [&]() {
        uint32_t have = spillHave();
        return have >= XmaDecoderPool::kMaxSpillSamplesPerChannel
                   ? 0u
                   : XmaDecoderPool::kMaxSpillSamplesPerChannel - have;
    };
    auto drainSpill = [&]() {
        if (state->spill.empty() || written >= cap || !outBE) return;
        uint32_t take = spillHave();
        if (take > cap - written) take = cap - written;
        if (!take) return;
        memcpy(outBE + size_t(written) * size_t(channels), state->spill.data(),
               size_t(take) * size_t(channels) * 2);
        written += take;
        uint32_t left = spillHave() - take;
        if (!left) {
            state->spill.clear();
        } else {
            memmove(state->spill.data(), state->spill.data() + size_t(take) * size_t(channels),
                    size_t(left) * size_t(channels) * 2);
            state->spill.resize(size_t(left) * size_t(channels));
        }
    };
    drainSpill();
    for (;;) {
        if (!state->pending) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) return XmaDecoderPool::kError;
            int rc = avcodec_receive_frame(state->codec, frame);
            if (rc == AVERROR(EAGAIN)) {
                av_frame_free(&frame);
                return 0;
            }
            if (rc == AVERROR_EOF) {
                av_frame_free(&frame);
                if (eofMode) state->eofTerminal = true;
                return 0;
            }
            if (rc < 0 || frame->format != AV_SAMPLE_FMT_FLTP ||
                frame->ch_layout.nb_channels != channels || frame->nb_samples <= 0) {
                av_frame_free(&frame);
                return XmaDecoderPool::kError;
            }
            const float* p0 = (const float*)frame->extended_data[0];
            const float* p1 = channels > 1 ? (const float*)frame->extended_data[1] : nullptr;
            if (!p0 || (channels > 1 && !p1) || !xmaFrameFinite(frame, channels)) {
                av_frame_free(&frame);
                return XmaDecoderPool::kError;
            }
            state->pending = frame;
            state->pendingOff = 0;
        }
        AVFrame* frame = state->pending;
        uint32_t total = uint32_t(frame->nb_samples);
        const float* planes[2]{(const float*)frame->extended_data[0],
                               channels > 1 ? (const float*)frame->extended_data[1] : nullptr};
        while (state->pendingOff < total) {
            if (outBE && written < cap) {
                uint32_t step = total - state->pendingOff;
                if (step > cap - written) step = cap - written;
                for (uint32_t s = 0; s < step; ++s)
                    for (int c = 0; c < channels; ++c)
                        outBE[(size_t(written) + s) * size_t(channels) + size_t(c)] =
                            xmaConvertSample(planes[c][state->pendingOff + s]);
                written += step;
                state->pendingOff += step;
            } else if (spillRoom() > 0) {
                uint32_t rest = total - state->pendingOff;
                uint32_t room = spillRoom();
                if (rest > room) rest = room;
                size_t at = state->spill.size();
                state->spill.resize(at + size_t(rest) * size_t(channels));
                for (uint32_t s = 0; s < rest; ++s)
                    for (int c = 0; c < channels; ++c)
                        state->spill[at + size_t(s) * size_t(channels) + size_t(c)] =
                            xmaConvertSample(planes[c][state->pendingOff + s]);
                state->pendingOff += rest;
            } else {
                return 1;
            }
        }
        xmaFreePending(state);
    }
}
}  // namespace

int XmaDecoderPool::submit(uint32_t slot, int rateHz, int channels, const uint32_t* image,
                           const uint8_t* base, int16_t* outBE, uint32_t outCapSamples,
                           uint32_t consumed[2], const uint64_t generation[2]) {
    if (consumed) consumed[0] = consumed[1] = 0;
    if (slot >= kSlots || !xmaRateOk(rateHz) || channels < 1 || channels > 2 || !image || !base ||
        !consumed || !generation || (!outBE && outCapSamples > 0))
        return kError;
    uint32_t w0 = image[0], w1 = image[1];
    uint32_t w2 = image[2], w3 = image[3], w4 = image[4];
    uint32_t counts[2] = {w0 & 0xFFFu, w1 & 0xFFFu};
    uint32_t ptrs[2] = {image[5], image[6]};
    bool valid[2] = {(w0 & 0x00100000u) != 0, (w0 & 0x00200000u) != 0};
    bool anyActive = false;
    for (int b = 0; b < 2; ++b) anyActive = anyActive || (valid[b] && counts[b] && ptrs[b]);
    if ((w0 >> 12 & 0xFFu) != 0) return kUnsupported;
    if (w3 != 0 || (w4 & 0x03FFFFFFu) != 0) return kUnsupported;
    if ((w1 >> 12 & 0x3u) != 0 || (w1 >> 14 & 0x7u) != 0 || (w1 >> 17 & 0x7u) != 0) return kUnsupported;
    if ((w1 >> 24 & 0x7u) != 0) return kUnsupported;
    if (xmaEnumRate(w1 >> 27) != rateHz) return kUnsupported;
    if (((w1 >> 29 & 1u) != 0) != (channels == 2)) return kUnsupported;
    if (anyActive && w2 != kWholePacketReadBit) return kUnsupported;
    uint32_t current = (w4 >> 31) & 1u;
    std::lock_guard lock(mutex_);
    auto& state = decoders_[slot];
    if (!state) state = std::make_unique<XmaStreamDecoder>();
    if (!state->open || state->channels != channels || state->rateHz != rateHz) {
        if (state->open && (!state->spill.empty() || state->pending || state->packetsAccepted > 0))
            return kNeedReset;
        if (state->codec) {
            avcodec_free_context(&state->codec);
            state->open = false;
        }
        state->codec = openDecoder(rateHz, channels);
        if (!state->codec) return kError;
        state->channels = channels;
        state->rateHz = rateHz;
        state->open = true;
        xmaResetStream(state.get());
    }
    if (state->eofSent || state->eofTerminal) return kNeedReset;
    uint32_t written = 0;
    int pr = xmaPump(state.get(), outBE, outCapSamples, written, false);
    if (pr == kError) return kError;
    uint32_t order[2] = {current, current ^ 1u};
    for (int oi = 0; oi < 2; ++oi) {
        uint32_t b = order[oi];
        if (!valid[b] || !counts[b] || !ptrs[b]) {
            state->inPtr[b] = ptrs[b];
            state->inGen[b] = generation[b];
            state->inSeen[b] = true;
            continue;
        }
        if (!state->inSeen[b] || state->inPtr[b] != ptrs[b] || state->inGen[b] != generation[b]) {
            state->inPtr[b] = ptrs[b];
            state->inGen[b] = generation[b];
            state->inSeen[b] = true;
            state->inDone[b] = 0;
        }
        while (state->inDone[b] < counts[b]) {
            bool outFull = !outBE || outCapSamples == 0 || written >= outCapSamples;
            bool spillFull =
                state->spill.size() / size_t(channels) >= kMaxSpillSamplesPerChannel;
            if (outFull && spillFull && state->pending) break;
            uint32_t packetAddr = ptrs[b] + state->inDone[b] * kPacketBytes;
            if (!xmaGuard(base, packetAddr, kPacketBytes)) return kError;
            AVPacket* avpkt = av_packet_alloc();
            if (!avpkt) return kError;
            if (av_new_packet(avpkt, int(kPacketBytes)) < 0) {
                av_packet_free(&avpkt);
                return kError;
            }
            memcpy(avpkt->data, base + packetAddr, kPacketBytes);
            int rc = avcodec_send_packet(state->codec, avpkt);
            av_packet_free(&avpkt);
            if (rc == AVERROR(EAGAIN)) {
                int dr = xmaPump(state.get(), outBE, outCapSamples, written, false);
                if (dr == kError) return kError;
                if (dr == 1) break;
                continue;
            }
            if (rc < 0) return kError;
            ++state->inDone[b];
            ++consumed[b];
            ++state->packetsAccepted;
            int dr = xmaPump(state.get(), outBE, outCapSamples, written, false);
            if (dr == kError) return kError;
            if (dr == 1) break;
        }
        bool blockedOut = (!outBE || outCapSamples == 0 || written >= outCapSamples) &&
                            state->spill.size() / size_t(channels) >= kMaxSpillSamplesPerChannel &&
                            state->pending;
        if (blockedOut) break;
    }
    return int(written);
}

int XmaDecoderPool::drain(uint32_t slot, int16_t* outBE, uint32_t outCapSamples) {
    if (slot >= kSlots || (!outBE && outCapSamples > 0)) return kError;
    std::lock_guard lock(mutex_);
    auto& state = decoders_[slot];
    if (!state || !state->open) return kError;
    uint32_t written = 0;
    int pr = xmaPump(state.get(), outBE, outCapSamples, written, false);
    if (pr == kError) return kError;
    return int(written);
}

int XmaDecoderPool::drainEofForTest(uint32_t slot, int16_t* outBE, uint32_t outCapSamples) {
    if (slot >= kSlots || (!outBE && outCapSamples > 0)) return kError;
    std::lock_guard lock(mutex_);
    auto& state = decoders_[slot];
    if (!state || !state->open) return kError;
    uint32_t written = 0;
    if (!state->eofSent) {
        int pre = xmaPump(state.get(), outBE, outCapSamples, written, true);
        if (pre == kError) return kError;
        for (int attempt = 0; attempt < 4 && !state->eofSent; ++attempt) {
            int rc = avcodec_send_packet(state->codec, nullptr);
            if (rc == 0 || rc == AVERROR_EOF) {
                state->eofSent = true;
                break;
            }
            if (rc == AVERROR(EAGAIN)) {
                int pr = xmaPump(state.get(), outBE, outCapSamples, written, true);
                if (pr == kError) return kError;
                if (pr == 1) break;
                continue;
            }
            return kError;
        }
    }
    int pr = xmaPump(state.get(), outBE, outCapSamples, written, true);
    if (pr == kError) return kError;
    return int(written);
}

uint32_t XmaDecoderPool::pendingSamples(uint32_t slot) {
    std::lock_guard lock(mutex_);
    if (slot >= kSlots || !decoders_[slot] || !decoders_[slot]->open) return 0;
    auto& state = decoders_[slot];
    if (state->channels <= 0) return 0;
    uint32_t total = uint32_t(state->spill.size() / size_t(state->channels));
    if (state->pending) total += uint32_t(state->pending->nb_samples) - state->pendingOff;
    return total;
}

void XmaDecoderPool::release(uint32_t slot) {
    std::lock_guard lock(mutex_);
    if (slot >= kSlots || !decoders_[slot]) return;
    if (decoders_[slot]->pending) av_frame_free(&decoders_[slot]->pending);
    if (decoders_[slot]->codec) avcodec_free_context(&decoders_[slot]->codec);
    decoders_[slot].reset();
}

void XmaDecoderPool::clear() {
    std::lock_guard lock(mutex_);
    for (auto& decoder : decoders_) {
        if (!decoder) continue;
        if (decoder->pending) av_frame_free(&decoder->pending);
        if (decoder->codec) avcodec_free_context(&decoder->codec);
        decoder.reset();
    }
}

XmaDecoderPool::~XmaDecoderPool() {
    for (auto& decoder : decoders_) {
        if (!decoder) continue;
        if (decoder->pending) av_frame_free(&decoder->pending);
        if (decoder->codec) avcodec_free_context(&decoder->codec);
        decoder.reset();
    }
}
