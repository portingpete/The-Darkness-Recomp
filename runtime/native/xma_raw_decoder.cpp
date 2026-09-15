#include "xma_raw_decoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
}

namespace DarkRecomp::Native {
XmaRawDecoder::~XmaRawDecoder() {
    av_frame_free(&frame_);
    avcodec_free_context(&codec_);
}
bool XmaRawDecoder::open(int rate, int channels) {
    if (codec_ || (channels != 1 && channels != 2) ||
        (rate != 24000 && rate != 32000 && rate != 44100 && rate != 48000)) return false;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA1);
    if (!codec) return false;
    codec_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    if (!codec_ || !frame_) return false;
    codec_->sample_rate = rate;
    codec_->block_align = 2048;
    av_channel_layout_default(&codec_->ch_layout, channels);
    codec_->extradata = static_cast<uint8_t*>(av_mallocz(28 + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codec_->extradata) return false;
    codec_->extradata_size = 28;
    codec_->extradata[4] = 1;
    codec_->extradata[25] = uint8_t(channels);
    error_ = av_opt_set_int(codec_->priv_data, "darkrecomp_raw_frames", 1, 0);
    if (error_ < 0) return false; // An unpatched stock DLL must never silently work here.
    error_ = avcodec_open2(codec_, codec, nullptr);
    if (error_ < 0) return false;
    channels_ = channels;
    rate_ = rate;
    return true;
}
int XmaRawDecoder::push(const uint8_t* packet) {
    if (!channels_ || !packet || error_ < 0) return -1;
    if (frame_->nb_samples > int(offset_)) return needDrain;
    AVPacket* input = av_packet_alloc();
    if (!input) return -1;
    int rc = av_new_packet(input, 2048);
    if (rc >= 0) {
        memcpy(input->data, packet, 2048);
        rc = avcodec_send_packet(codec_, input);
    }
    av_packet_free(&input);
    if (rc == AVERROR(EAGAIN)) return needDrain;
    if (rc < 0) { error_ = rc; return -1; }
    return 0;
}
int XmaRawDecoder::read(float* out, uint32_t cap) {
    if (!channels_ || !out || !cap || error_ < 0) return -1;
    if (frame_->nb_samples == int(offset_)) {
        av_frame_unref(frame_);
        offset_ = 0;
        int rc = avcodec_receive_frame(codec_, frame_);
        if (rc == AVERROR(EAGAIN)) return 0;
        if (rc < 0) { error_ = rc; return -1; }
        if (frame_->format != AV_SAMPLE_FMT_FLTP || frame_->ch_layout.nb_channels != channels_ ||
            frame_->nb_samples != 512) { error_ = AVERROR_INVALIDDATA; return -1; }
        for (int ch = 0; ch < channels_; ++ch) {
            if (!frame_->extended_data[ch]) { error_ = AVERROR_INVALIDDATA; return -1; }
            const auto* plane = reinterpret_cast<const float*>(frame_->extended_data[ch]);
            for (int i = 0; i < frame_->nb_samples; ++i)
                if (!std::isfinite(plane[i])) { error_ = AVERROR_INVALIDDATA; return -1; }
        }
    }
    uint32_t take = (std::min)(cap, uint32_t(frame_->nb_samples) - offset_);
    for (uint32_t i = 0; i < take; ++i)
        for (int ch = 0; ch < channels_; ++ch)
            out[i * channels_ + ch] = reinterpret_cast<const float*>(frame_->extended_data[ch])[offset_ + i];
    offset_ += take;
    return int(take);
}
}
