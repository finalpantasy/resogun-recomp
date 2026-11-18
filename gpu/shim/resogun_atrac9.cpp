// SPDX-License-Identifier: GPL-2.0-or-later
// ATRAC9 decoding for the Ngs2 sampler (src/runtime_ngs2.c), through libavcodec's decoder.
// One decoder per playing voice; the caller feeds whole superframes.
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}

namespace {
struct Atrac9 {
    AVCodecContext* context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int channels = 0, superframe_bytes = 0, superframe_samples = 0;
};
// The 4-byte config: magic 0xFE, sample rate index 4 bits, block config 3 bits, a zero bit,
// frame size - 1 in 11 bits, superframe index 2 bits.
constexpr int kChannels[6] = {1, 2, 2, 6, 8, 4};
constexpr int kFrameLog2[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
} // namespace

extern "C" void resogun_atrac9_close(void* handle) {
    auto* a = static_cast<Atrac9*>(handle);
    if (!a) {
        return;
    }
    avcodec_free_context(&a->context);
    av_packet_free(&a->packet);
    av_frame_free(&a->frame);
    delete a;
}

// Opens a decoder for `config` (the stream's 4 config bytes in memory order). Returns nullptr when
// the config is not ATRAC9 or the decoder is missing; otherwise writes the channel count, the bytes
// of one superframe and the samples per channel it decodes to.
extern "C" void* resogun_atrac9_open(const uint8_t config[4], int* channels, int* superframe_bytes,
                                     int* superframe_samples) {
    if (!config || config[0] != 0xFE) {
        return nullptr;
    }
    const uint32_t bits = uint32_t(config[1]) << 16 | uint32_t(config[2]) << 8 | config[3];
    const int rate_index = bits >> 20 & 15, block = bits >> 17 & 7;
    const int frame_bytes = int(bits >> 5 & 0x7ff) + 1, superframe_index = bits >> 3 & 3;
    if (block > 5 || (bits >> 16 & 1) || superframe_index == 3) {
        return nullptr;
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC9);
    if (!codec) {
        return nullptr;
    }
    auto* a = new Atrac9;
    a->channels = kChannels[block];
    a->superframe_bytes = frame_bytes << superframe_index;
    a->superframe_samples = (1 << kFrameLog2[rate_index]) << superframe_index;
    a->context = avcodec_alloc_context3(codec);
    a->packet = av_packet_alloc();
    a->frame = av_frame_alloc();
    bool ok = a->context && a->packet && a->frame;
    if (ok) {
        a->context->extradata = static_cast<uint8_t*>(av_mallocz(12 + AV_INPUT_BUFFER_PADDING_SIZE));
        ok = a->context->extradata != nullptr;
    }
    if (ok) {
        a->context->extradata_size = 12;
        a->context->extradata[0] = 1; // version
        std::memcpy(a->context->extradata + 4, config, 4);
        a->context->block_align = a->superframe_bytes;
        ok = avcodec_open2(a->context, codec, nullptr) == 0;
    }
    if (!ok) {
        resogun_atrac9_close(a);
        return nullptr;
    }
    *channels = a->channels;
    *superframe_bytes = a->superframe_bytes;
    *superframe_samples = a->superframe_samples;
    return a;
}

// Forgets the decoder's overlap state (a voice restarting or looping to another position).
extern "C" void resogun_atrac9_reset(void* handle) {
    if (auto* a = static_cast<Atrac9*>(handle)) {
        avcodec_flush_buffers(a->context);
    }
}

// Decodes one superframe into interleaved floats (room for superframe_samples * channels).
// Returns the samples per channel written, or 0 on an error.
extern "C" int resogun_atrac9_decode(void* handle, const uint8_t* data, int size, float* out) {
    auto* a = static_cast<Atrac9*>(handle);
    if (!a || !data || size < a->superframe_bytes || av_new_packet(a->packet, a->superframe_bytes) != 0) {
        return 0;
    }
    std::memcpy(a->packet->data, data, a->superframe_bytes);
    int samples = 0;
    if (avcodec_send_packet(a->context, a->packet) == 0 && avcodec_receive_frame(a->context, a->frame) == 0 &&
        a->frame->format == AV_SAMPLE_FMT_FLTP && a->frame->ch_layout.nb_channels == a->channels &&
        a->frame->nb_samples <= a->superframe_samples) {
        samples = a->frame->nb_samples;
        for (int c = 0; c < a->channels; ++c) {
            const float* plane = reinterpret_cast<const float*>(a->frame->extended_data[c]);
            for (int i = 0; i < samples; ++i) {
                out[i * a->channels + c] = plane[i];
            }
        }
    }
    av_frame_unref(a->frame);
    av_packet_unref(a->packet);
    return samples;
}
