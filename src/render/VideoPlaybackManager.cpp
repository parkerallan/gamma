#include "render/VideoPlaybackManager.h"

#include "audio/AudioEngine.h"
#include "vfs/AssetVFS.h"

#include <SDL3/SDL_log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "miniaudio.h"

#include <algorithm>
#include <cstring>

namespace
{
std::string FFErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// AVIO callbacks for in-memory pak streaming. The opaque pointer is the
// owning VideoStream so we can read from its byte buffer + cursor.
struct PakIoState
{
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t* pos = nullptr;
};

int PakIoRead(void* opaque, std::uint8_t* out_buf, int buf_size)
{
    PakIoState* st = static_cast<PakIoState*>(opaque);
    if (st == nullptr || st->pos == nullptr || *st->pos >= st->size)
    {
        return AVERROR_EOF;
    }
    const std::size_t remaining = st->size - *st->pos;
    const std::size_t to_read = std::min<std::size_t>(static_cast<std::size_t>(buf_size), remaining);
    std::memcpy(out_buf, st->data + *st->pos, to_read);
    *st->pos += to_read;
    return static_cast<int>(to_read);
}

int64_t PakIoSeek(void* opaque, int64_t offset, int whence)
{
    PakIoState* st = static_cast<PakIoState*>(opaque);
    if (st == nullptr || st->pos == nullptr)
    {
        return -1;
    }
    if (whence == AVSEEK_SIZE)
    {
        return static_cast<int64_t>(st->size);
    }
    int64_t target = 0;
    switch (whence & ~AVSEEK_FORCE)
    {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = static_cast<int64_t>(*st->pos) + offset; break;
        case SEEK_END: target = static_cast<int64_t>(st->size) + offset; break;
        default: return -1;
    }
    if (target < 0 || target > static_cast<int64_t>(st->size))
    {
        return -1;
    }
    *st->pos = static_cast<std::size_t>(target);
    return target;
}

// Codec ctx "opaque" pointer holds the chosen hw pixel format for get_format.
AVPixelFormat HwGetFormatCallback(AVCodecContext* ctx, const AVPixelFormat* pix_fmts)
{
    const AVPixelFormat want = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == want)
        {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}
} // namespace

VideoPlaybackManager::~VideoPlaybackManager()
{
    Shutdown();
}

bool VideoPlaybackManager::Initialize(Scene2DRenderer* scene_2d_renderer, AudioEngine* audio_engine)
{
    if (scene_2d_renderer == nullptr)
    {
        return false;
    }
    scene_2d_renderer_ = scene_2d_renderer;
    audio_engine_ = audio_engine;
    // Quiet FFmpeg's normal log level; the engine surfaces its own messages
    // via SDL_Log so we only want fatal/error spam from libav.
    av_log_set_level(AV_LOG_ERROR);
    SDL_Log("VideoPlaybackManager: initialized (FFmpeg %s, audio=%s)",
            av_version_info(),
            (audio_engine != nullptr && audio_engine->IsInitialized()) ? "yes" : "no");
    return true;
}

void VideoPlaybackManager::Shutdown()
{
    for (auto& [key, stream] : streams_)
    {
        if (stream)
        {
            ReleaseStream(*stream);
        }
    }
    streams_.clear();
    scene_2d_renderer_ = nullptr;
    audio_engine_ = nullptr;
}

void VideoPlaybackManager::ReleaseStream(VideoStream& stream)
{
    if (stream.audio_sound != nullptr)
    {
        ma_sound_uninit(stream.audio_sound);
        delete stream.audio_sound;
        stream.audio_sound = nullptr;
    }
    if (stream.audio_buffer_ref != nullptr)
    {
        ma_audio_buffer_ref* ref = static_cast<ma_audio_buffer_ref*>(stream.audio_buffer_ref);
        ma_audio_buffer_ref_uninit(ref);
        delete ref;
        stream.audio_buffer_ref = nullptr;
    }
    if (stream.swr_ctx != nullptr)
    {
        swr_free(&stream.swr_ctx);
    }
    if (stream.audio_codec_ctx != nullptr)
    {
        avcodec_free_context(&stream.audio_codec_ctx);
    }
    stream.pcm_buffer.clear();
    stream.pcm_frame_count = 0;
    stream.pcm_sample_rate = 0;
    stream.audio_stream_index = -1;
    stream.audio_started = false;

    if (stream.sws_ctx != nullptr)
    {
        sws_freeContext(stream.sws_ctx);
        stream.sws_ctx = nullptr;
    }
    if (stream.decoded_frame != nullptr)
    {
        av_frame_free(&stream.decoded_frame);
    }
    if (stream.sw_frame != nullptr)
    {
        av_frame_free(&stream.sw_frame);
    }
    if (stream.rgba_frame != nullptr)
    {
        av_frame_free(&stream.rgba_frame);
    }
    if (stream.packet != nullptr)
    {
        av_packet_free(&stream.packet);
    }
    if (stream.codec_ctx != nullptr)
    {
        avcodec_free_context(&stream.codec_ctx);
    }
    if (stream.hw_device_ctx != nullptr)
    {
        av_buffer_unref(&stream.hw_device_ctx);
    }
    if (stream.format_ctx != nullptr)
    {
        avformat_close_input(&stream.format_ctx);
    }
    if (stream.avio_ctx != nullptr)
    {
        // FFmpeg may have reallocated the buffer; free whatever it points to.
        av_freep(&stream.avio_ctx->buffer);
        // The opaque is heap-allocated PakIoState — delete it before tearing
        // down avio_ctx itself.
        delete static_cast<PakIoState*>(stream.avio_ctx->opaque);
        avio_context_free(&stream.avio_ctx);
    }
    stream.file_bytes.clear();
    stream.file_bytes.shrink_to_fit();
    stream.file_pos = 0;
    stream.hw_pix_fmt = -1;
    stream.sws_src_format = -1;
    if (scene_2d_renderer_ != nullptr)
    {
        scene_2d_renderer_->DestroyExternalTexture(stream.frame_texture);
    }
    stream.rgba_scratch.clear();
    stream.first_frame_uploaded = false;
    stream.finished_once = false;
    stream.reached_eof = false;
    stream.playback_time = 0.0;
    stream.next_frame_pts = -1.0;
    stream.video_stream_index = -1;
    stream.width = 0;
    stream.height = 0;
}

bool VideoPlaybackManager::OpenStream(VideoStream& stream, const std::filesystem::path& path_for_logs, bool from_pak)
{
    const std::string path_utf8 = path_for_logs.generic_string();
    SDL_Log("VideoPlaybackManager: opening '%s' (%s)",
            path_utf8.c_str(), from_pak ? "pak" : "file");

    if (from_pak)
    {
        // file_bytes is already filled in by Update().
        if (stream.file_bytes.empty())
        {
            SDL_Log("VideoPlaybackManager: pak read returned 0 bytes for '%s'", path_utf8.c_str());
            return false;
        }
        constexpr int kAvioBufSize = 32 * 1024;
        unsigned char* avio_buf = static_cast<unsigned char*>(av_malloc(kAvioBufSize));
        if (avio_buf == nullptr)
        {
            SDL_Log("VideoPlaybackManager: av_malloc failed");
            return false;
        }
        auto* io_state = new PakIoState{stream.file_bytes.data(), stream.file_bytes.size(), &stream.file_pos};
        stream.avio_ctx = avio_alloc_context(avio_buf, kAvioBufSize, 0, io_state, &PakIoRead, nullptr, &PakIoSeek);
        if (stream.avio_ctx == nullptr)
        {
            av_free(avio_buf);
            delete io_state;
            SDL_Log("VideoPlaybackManager: avio_alloc_context failed");
            return false;
        }
        stream.format_ctx = avformat_alloc_context();
        if (stream.format_ctx == nullptr)
        {
            SDL_Log("VideoPlaybackManager: avformat_alloc_context failed");
            return false;
        }
        stream.format_ctx->pb = stream.avio_ctx;
        const int err = avformat_open_input(&stream.format_ctx, nullptr, nullptr, nullptr);
        if (err < 0)
        {
            SDL_Log("VideoPlaybackManager: avformat_open_input (pak) failed for '%s': %s",
                    path_utf8.c_str(), FFErr(err).c_str());
            return false;
        }
    }
    else
    {
        const int err = avformat_open_input(&stream.format_ctx, path_utf8.c_str(), nullptr, nullptr);
        if (err < 0)
        {
            SDL_Log("VideoPlaybackManager: avformat_open_input failed for '%s': %s",
                    path_utf8.c_str(), FFErr(err).c_str());
            return false;
        }
    }

    int err = avformat_find_stream_info(stream.format_ctx, nullptr);
    if (err < 0)
    {
        SDL_Log("VideoPlaybackManager: avformat_find_stream_info failed: %s", FFErr(err).c_str());
        return false;
    }

    stream.video_stream_index = av_find_best_stream(
        stream.format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream.video_stream_index < 0)
    {
        SDL_Log("VideoPlaybackManager: no video stream in '%s'", path_utf8.c_str());
        return false;
    }

    AVStream* av_stream = stream.format_ctx->streams[stream.video_stream_index];
    AVCodecParameters* codec_params = av_stream->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (codec == nullptr)
    {
        SDL_Log("VideoPlaybackManager: no decoder for codec id %d", static_cast<int>(codec_params->codec_id));
        return false;
    }

    stream.codec_ctx = avcodec_alloc_context3(codec);
    if (stream.codec_ctx == nullptr)
    {
        SDL_Log("VideoPlaybackManager: avcodec_alloc_context3 failed");
        return false;
    }

    err = avcodec_parameters_to_context(stream.codec_ctx, codec_params);
    if (err < 0)
    {
        SDL_Log("VideoPlaybackManager: avcodec_parameters_to_context failed: %s", FFErr(err).c_str());
        return false;
    }

    // Try hardware acceleration. On Windows we prefer D3D11VA, falling back to
    // DXVA2 if needed; on other platforms we just probe the codec's advertised
    // hw configs in priority order. Any failure falls through to software.
    {
        AVHWDeviceType preferred[] = {
#ifdef _WIN32
            AV_HWDEVICE_TYPE_D3D11VA,
            AV_HWDEVICE_TYPE_DXVA2,
#endif
            AV_HWDEVICE_TYPE_NONE,
        };
        AVHWDeviceType chosen_type = AV_HWDEVICE_TYPE_NONE;
        AVPixelFormat chosen_pix = AV_PIX_FMT_NONE;
        for (AVHWDeviceType type : preferred)
        {
            if (type == AV_HWDEVICE_TYPE_NONE) break;
            for (int i = 0;; ++i)
            {
                const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
                if (config == nullptr) break;
                if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                    config->device_type == type)
                {
                    chosen_type = type;
                    chosen_pix = config->pix_fmt;
                    break;
                }
            }
            if (chosen_type != AV_HWDEVICE_TYPE_NONE) break;
        }

        if (chosen_type != AV_HWDEVICE_TYPE_NONE)
        {
            if (av_hwdevice_ctx_create(&stream.hw_device_ctx, chosen_type, nullptr, nullptr, 0) >= 0)
            {
                stream.codec_ctx->hw_device_ctx = av_buffer_ref(stream.hw_device_ctx);
                stream.codec_ctx->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(chosen_pix));
                stream.codec_ctx->get_format = &HwGetFormatCallback;
                stream.hw_pix_fmt = static_cast<int>(chosen_pix);
                SDL_Log("VideoPlaybackManager: hwaccel enabled (%s, hw_pix=%d)",
                        av_hwdevice_get_type_name(chosen_type), static_cast<int>(chosen_pix));
            }
            else
            {
                SDL_Log("VideoPlaybackManager: av_hwdevice_ctx_create failed; falling back to software");
                if (stream.hw_device_ctx) av_buffer_unref(&stream.hw_device_ctx);
            }
        }
    }

    err = avcodec_open2(stream.codec_ctx, codec, nullptr);
    if (err < 0)
    {
        // Retry without hwaccel.
        if (stream.hw_device_ctx != nullptr)
        {
            SDL_Log("VideoPlaybackManager: hw avcodec_open2 failed (%s); retrying software", FFErr(err).c_str());
            av_buffer_unref(&stream.hw_device_ctx);
            stream.codec_ctx->hw_device_ctx = nullptr;
            stream.codec_ctx->get_format = nullptr;
            stream.codec_ctx->opaque = nullptr;
            stream.hw_pix_fmt = -1;
            err = avcodec_open2(stream.codec_ctx, codec, nullptr);
        }
        if (err < 0)
        {
            SDL_Log("VideoPlaybackManager: avcodec_open2 failed: %s", FFErr(err).c_str());
            return false;
        }
    }

    stream.width = stream.codec_ctx->width;
    stream.height = stream.codec_ctx->height;
    if (stream.width <= 0 || stream.height <= 0)
    {
        SDL_Log("VideoPlaybackManager: invalid video size %dx%d", stream.width, stream.height);
        return false;
    }

    stream.time_base_seconds = (av_stream->time_base.den > 0)
        ? static_cast<double>(av_stream->time_base.num) / static_cast<double>(av_stream->time_base.den)
        : 0.0;

    stream.decoded_frame = av_frame_alloc();
    stream.sw_frame = av_frame_alloc();
    stream.rgba_frame = av_frame_alloc();
    stream.packet = av_packet_alloc();
    if (stream.decoded_frame == nullptr || stream.sw_frame == nullptr ||
        stream.rgba_frame == nullptr || stream.packet == nullptr)
    {
        SDL_Log("VideoPlaybackManager: av_frame_alloc/av_packet_alloc failed");
        return false;
    }

    stream.rgba_scratch.assign(static_cast<std::size_t>(stream.width) * static_cast<std::size_t>(stream.height) * 4u, 0u);
    av_image_fill_arrays(
        stream.rgba_frame->data,
        stream.rgba_frame->linesize,
        stream.rgba_scratch.data(),
        AV_PIX_FMT_RGBA,
        stream.width,
        stream.height,
        1);

    // sws_ctx is created lazily on the first decoded frame so we know the
    // actual source pixel format (NV12 for D3D11VA transfers, YUV420P for
    // most software paths, etc.).

    SDL_Log("VideoPlaybackManager: opened '%s' (%dx%d, codec=%s, %s)",
            path_utf8.c_str(), stream.width, stream.height, codec->name,
            stream.hw_device_ctx ? "hw" : "sw");

    // Best-effort audio setup. Failures are non-fatal — video still plays.
    OpenAudio(stream);
    return true;
}

bool VideoPlaybackManager::OpenAudio(VideoStream& stream)
{
    if (audio_engine_ == nullptr || !audio_engine_->IsInitialized())
    {
        return false;
    }
    ma_engine* ma = audio_engine_->GetEngine();
    if (ma == nullptr)
    {
        return false;
    }

    stream.audio_stream_index = av_find_best_stream(
        stream.format_ctx, AVMEDIA_TYPE_AUDIO, -1, stream.video_stream_index, nullptr, 0);
    if (stream.audio_stream_index < 0)
    {
        SDL_Log("VideoPlaybackManager: no audio stream in '%s'", stream.source_path.c_str());
        return false;
    }

    AVStream* astream = stream.format_ctx->streams[stream.audio_stream_index];
    AVCodecParameters* aparams = astream->codecpar;
    const AVCodec* acodec = avcodec_find_decoder(aparams->codec_id);
    if (acodec == nullptr)
    {
        SDL_Log("VideoPlaybackManager: no audio decoder for codec id %d",
                static_cast<int>(aparams->codec_id));
        stream.audio_stream_index = -1;
        return false;
    }

    stream.audio_codec_ctx = avcodec_alloc_context3(acodec);
    if (stream.audio_codec_ctx == nullptr ||
        avcodec_parameters_to_context(stream.audio_codec_ctx, aparams) < 0 ||
        avcodec_open2(stream.audio_codec_ctx, acodec, nullptr) < 0)
    {
        SDL_Log("VideoPlaybackManager: audio codec open failed");
        if (stream.audio_codec_ctx != nullptr)
        {
            avcodec_free_context(&stream.audio_codec_ctx);
        }
        stream.audio_stream_index = -1;
        return false;
    }

    stream.pcm_sample_rate = ma_engine_get_sample_rate(ma);
    if (stream.pcm_sample_rate == 0)
    {
        stream.pcm_sample_rate = 48000;
    }

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int swr_err = swr_alloc_set_opts2(
        &stream.swr_ctx,
        &out_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(stream.pcm_sample_rate),
        &stream.audio_codec_ctx->ch_layout, stream.audio_codec_ctx->sample_fmt, stream.audio_codec_ctx->sample_rate,
        0, nullptr);
    if (swr_err < 0 || stream.swr_ctx == nullptr || swr_init(stream.swr_ctx) < 0)
    {
        SDL_Log("VideoPlaybackManager: swr_init failed");
        if (stream.swr_ctx != nullptr) swr_free(&stream.swr_ctx);
        avcodec_free_context(&stream.audio_codec_ctx);
        stream.audio_stream_index = -1;
        return false;
    }

    if (!DecodeAllAudio(stream))
    {
        SDL_Log("VideoPlaybackManager: audio decode failed");
        swr_free(&stream.swr_ctx);
        avcodec_free_context(&stream.audio_codec_ctx);
        stream.audio_stream_index = -1;
        stream.pcm_buffer.clear();
        return false;
    }

    // Build a non-owning audio buffer ref + ma_sound. The PCM data lives in
    // VideoStream::pcm_buffer for the lifetime of the stream.
    ma_audio_buffer_ref* new_ref = new ma_audio_buffer_ref{};
    if (ma_audio_buffer_ref_init(
            ma_format_f32, 2, stream.pcm_buffer.data(),
            stream.pcm_frame_count, new_ref) != MA_SUCCESS)
    {
        SDL_Log("VideoPlaybackManager: ma_audio_buffer_ref_init failed");
        delete new_ref;
        return false;
    }
    stream.audio_buffer_ref = new_ref;

    stream.audio_sound = new ma_sound{};
    if (ma_sound_init_from_data_source(
            ma, new_ref, MA_SOUND_FLAG_NO_SPATIALIZATION,
            nullptr, stream.audio_sound) != MA_SUCCESS)
    {
        SDL_Log("VideoPlaybackManager: ma_sound_init_from_data_source failed");
        delete stream.audio_sound;
        stream.audio_sound = nullptr;
        ma_audio_buffer_ref_uninit(new_ref);
        delete new_ref;
        stream.audio_buffer_ref = nullptr;
        return false;
    }
    ma_sound_set_spatialization_enabled(stream.audio_sound, MA_FALSE);

    SDL_Log("VideoPlaybackManager: audio ready for '%s' (%llu frames @ %u Hz)",
            stream.source_path.c_str(),
            static_cast<unsigned long long>(stream.pcm_frame_count),
            stream.pcm_sample_rate);
    if (ma_device* dev = ma_engine_get_device(ma))
    {
        const ma_uint32 buf = dev->playback.internalPeriodSizeInFrames * dev->playback.internalPeriods;
        SDL_Log("VideoPlaybackManager: device buffer = %u frames * %u periods = %u (%.1f ms @ %u Hz)",
                dev->playback.internalPeriodSizeInFrames,
                dev->playback.internalPeriods,
                buf,
                (1000.0 * buf) / static_cast<double>(stream.pcm_sample_rate),
                stream.pcm_sample_rate);
    }
    return true;
}

bool VideoPlaybackManager::DecodeAllAudio(VideoStream& stream)
{
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (pkt == nullptr || frame == nullptr)
    {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        return false;
    }

    // Seek to start of audio stream before draining.
    av_seek_frame(stream.format_ctx, stream.audio_stream_index, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(stream.audio_codec_ctx);

    auto append_converted = [&](AVFrame* in_frame) {
        const int max_out = static_cast<int>(av_rescale_rnd(
            swr_get_delay(stream.swr_ctx, stream.audio_codec_ctx->sample_rate)
                + (in_frame ? in_frame->nb_samples : 0),
            static_cast<int>(stream.pcm_sample_rate),
            stream.audio_codec_ctx->sample_rate,
            AV_ROUND_UP));
        if (max_out <= 0) return;

        const std::size_t old_size = stream.pcm_buffer.size();
        stream.pcm_buffer.resize(old_size + static_cast<std::size_t>(max_out) * 2u);
        uint8_t* out_ptrs[1] = { reinterpret_cast<uint8_t*>(stream.pcm_buffer.data() + old_size) };

        int produced = 0;
        if (in_frame != nullptr)
        {
            produced = swr_convert(stream.swr_ctx, out_ptrs, max_out,
                                   const_cast<const uint8_t**>(in_frame->extended_data),
                                   in_frame->nb_samples);
        }
        else
        {
            produced = swr_convert(stream.swr_ctx, out_ptrs, max_out, nullptr, 0);
        }
        if (produced < 0) produced = 0;
        stream.pcm_buffer.resize(old_size + static_cast<std::size_t>(produced) * 2u);
        stream.pcm_frame_count += static_cast<std::uint64_t>(produced);
    };

    bool ok = true;
    while (true)
    {
        const int rerr = av_read_frame(stream.format_ctx, pkt);
        if (rerr == AVERROR_EOF)
        {
            avcodec_send_packet(stream.audio_codec_ctx, nullptr);
        }
        else if (rerr < 0)
        {
            ok = false;
            break;
        }
        else if (pkt->stream_index != stream.audio_stream_index)
        {
            av_packet_unref(pkt);
            continue;
        }
        else
        {
            avcodec_send_packet(stream.audio_codec_ctx, pkt);
            av_packet_unref(pkt);
        }

        while (true)
        {
            const int derr = avcodec_receive_frame(stream.audio_codec_ctx, frame);
            if (derr == AVERROR(EAGAIN) || derr == AVERROR_EOF)
            {
                break;
            }
            if (derr < 0)
            {
                ok = false;
                break;
            }
            append_converted(frame);
            av_frame_unref(frame);
        }

        if (rerr == AVERROR_EOF || !ok)
        {
            break;
        }
    }
    // Flush swr.
    append_converted(nullptr);

    // Restore demuxer to start of video stream for the video decode loop.
    av_seek_frame(stream.format_ctx, stream.video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(stream.codec_ctx);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    return ok && stream.pcm_frame_count > 0;
}

void VideoPlaybackManager::RewindStream(VideoStream& stream)
{
    if (stream.format_ctx == nullptr || stream.video_stream_index < 0)
    {
        return;
    }
    av_seek_frame(stream.format_ctx, stream.video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
    if (stream.codec_ctx != nullptr)
    {
        avcodec_flush_buffers(stream.codec_ctx);
    }
    stream.reached_eof = false;
    stream.finished_once = false;
    stream.playback_time = 0.0;
    stream.next_frame_pts = -1.0;

    if (stream.audio_sound != nullptr && stream.audio_buffer_ref != nullptr)
    {
        ma_sound_seek_to_pcm_frame(stream.audio_sound, 0);
    }
    stream.audio_started = false;
}

bool VideoPlaybackManager::DecodeNextFrame(VideoStream& stream)
{
    if (stream.codec_ctx == nullptr || stream.format_ctx == nullptr)
    {
        return false;
    }

    while (true)
    {
        int err = avcodec_receive_frame(stream.codec_ctx, stream.decoded_frame);
        if (err == 0)
        {
            // If hw-accelerated, transfer the frame from GPU memory to a sw
            // buffer (typically NV12) so we can sws_scale to RGBA.
            if (stream.hw_pix_fmt >= 0 && stream.decoded_frame->format == stream.hw_pix_fmt)
            {
                av_frame_unref(stream.sw_frame);
                const int xfer = av_hwframe_transfer_data(stream.sw_frame, stream.decoded_frame, 0);
                if (xfer < 0)
                {
                    SDL_Log("VideoPlaybackManager: av_hwframe_transfer_data failed: %s", FFErr(xfer).c_str());
                    av_frame_unref(stream.decoded_frame);
                    return false;
                }
                // Preserve PTS metadata from the hw frame.
                stream.sw_frame->best_effort_timestamp = stream.decoded_frame->best_effort_timestamp;
                stream.sw_frame->pts = stream.decoded_frame->pts;
            }

            // Successfully decoded a frame.
            const AVFrame* meta = (stream.hw_pix_fmt >= 0) ? stream.sw_frame : stream.decoded_frame;
            const int64_t pts = (meta->best_effort_timestamp != AV_NOPTS_VALUE)
                ? meta->best_effort_timestamp
                : meta->pts;
            stream.next_frame_pts = (pts == AV_NOPTS_VALUE)
                ? (stream.next_frame_pts < 0.0 ? 0.0 : stream.next_frame_pts)
                : static_cast<double>(pts) * stream.time_base_seconds;
            return true;
        }
        if (err != AVERROR(EAGAIN) && err != AVERROR_EOF)
        {
            SDL_Log("VideoPlaybackManager: avcodec_receive_frame error: %s", FFErr(err).c_str());
            return false;
        }
        if (err == AVERROR_EOF)
        {
            stream.reached_eof = true;
            return false;
        }

        // EAGAIN — feed more packets.
        const int read_err = av_read_frame(stream.format_ctx, stream.packet);
        if (read_err == AVERROR_EOF)
        {
            // Flush decoder.
            avcodec_send_packet(stream.codec_ctx, nullptr);
            stream.reached_eof = true;
            continue;
        }
        if (read_err < 0)
        {
            SDL_Log("VideoPlaybackManager: av_read_frame error: %s", FFErr(read_err).c_str());
            return false;
        }

        if (stream.packet->stream_index == stream.video_stream_index)
        {
            const int send_err = avcodec_send_packet(stream.codec_ctx, stream.packet);
            if (send_err < 0 && send_err != AVERROR(EAGAIN))
            {
                SDL_Log("VideoPlaybackManager: avcodec_send_packet error: %s", FFErr(send_err).c_str());
            }
        }
        av_packet_unref(stream.packet);
    }
}

void VideoPlaybackManager::DecodeAndUpload(VideoStream& stream, float delta_time, const SceneObjectVideo2DAttributes& attr)
{
    if (stream.codec_ctx == nullptr || scene_2d_renderer_ == nullptr)
    {
        return;
    }

    bool need_first_frame = !stream.first_frame_uploaded;
    bool playing = (attr.play_mode != SceneObjectVideoPlayMode::Off) &&
                   !(attr.play_mode == SceneObjectVideoPlayMode::PlayOnce && stream.finished_once);

    if (!need_first_frame && !playing)
    {
        return;
    }

    if (playing)
    {
        if (stream.audio_started && stream.audio_sound != nullptr && stream.pcm_sample_rate > 0)
        {
            // Slave video clock to audio cursor (audio is the master). The
            // cursor is the data-source read position, which is ahead of
            // actual speaker output by the device buffer; subtract that so
            // we display the frame that matches what the user is hearing.
            ma_uint64 cursor_frames = 0;
            if (ma_sound_get_cursor_in_pcm_frames(stream.audio_sound, &cursor_frames) == MA_SUCCESS)
            {
                ma_uint64 latency_frames = 0;
                if (audio_engine_ != nullptr)
                {
                    if (ma_device* dev = ma_engine_get_device(audio_engine_->GetEngine()))
                    {
                        // Full output buffer latency, not one period — most
                        // backends (incl. WASAPI shared) maintain 2-3 periods
                        // of buffering, so subtracting one period under-counts
                        // the gap and leaves audio audibly behind video.
                        latency_frames = static_cast<ma_uint64>(dev->playback.internalPeriodSizeInFrames) *
                                         static_cast<ma_uint64>(dev->playback.internalPeriods);
                    }
                }
                if (cursor_frames > latency_frames)
                {
                    cursor_frames -= latency_frames;
                }
                else
                {
                    cursor_frames = 0;
                }
                stream.playback_time = static_cast<double>(cursor_frames) /
                                       static_cast<double>(stream.pcm_sample_rate);
            }
            else
            {
                stream.playback_time += std::min<double>(delta_time, 0.1);
            }
        }
        else
        {
            // No audio yet (still waiting on first frame, or no audio track).
            // Use dt; the clamp prevents huge jumps when the tab unfocuses.
            stream.playback_time += std::min<double>(delta_time, 0.1);
        }
    }

    bool produced_new_frame = false;
    bool snapped_first_frame = false;

    // Decode-and-display loop: if our playback clock has caught up past the
    // current frame's PTS (or we still need a first frame), advance to the
    // next available frame. Hard cap iterations so a misbehaving stream or a
    // huge dt can't lock the main thread.
    constexpr int kMaxFramesPerUpdate = 4;
    int frames_this_update = 0;
    while (frames_this_update < kMaxFramesPerUpdate)
    {
        bool need_more;
        if (need_first_frame)
        {
            need_more = true;
        }
        else if (!playing)
        {
            need_more = false;
        }
        else
        {
            // Strict greater-than so a snap-equal condition does not loop.
            need_more = (stream.next_frame_pts < 0.0) ||
                        (stream.playback_time > stream.next_frame_pts);
        }

        if (!need_more)
        {
            break;
        }

        if (!DecodeNextFrame(stream))
        {
            if (stream.reached_eof)
            {
                if (attr.play_mode == SceneObjectVideoPlayMode::Loop)
                {
                    RewindStream(stream);
                    continue;
                }
                stream.finished_once = true;
            }
            break;
        }

        produced_new_frame = true;
        ++frames_this_update;

        if (need_first_frame)
        {
            // Snap clock to first frame's PTS so the next iteration does not
            // immediately want another frame.
            stream.playback_time = stream.next_frame_pts;
            need_first_frame = false;
            snapped_first_frame = true;
        }
    }
    (void)snapped_first_frame;

    if (!produced_new_frame)
    {
        return;
    }

    // Pick the frame to display: hw-decoded streams transfer to sw_frame.
    AVFrame* src_frame = (stream.hw_pix_fmt >= 0) ? stream.sw_frame : stream.decoded_frame;

    // Lazy-create / recreate sws_ctx when the source pixel format changes.
    if (stream.sws_ctx == nullptr || stream.sws_src_format != src_frame->format)
    {
        if (stream.sws_ctx != nullptr)
        {
            sws_freeContext(stream.sws_ctx);
            stream.sws_ctx = nullptr;
        }
        stream.sws_src_format = src_frame->format;
        stream.sws_ctx = sws_getContext(
            stream.width, stream.height, static_cast<AVPixelFormat>(src_frame->format),
            stream.width, stream.height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (stream.sws_ctx == nullptr)
        {
            SDL_Log("VideoPlaybackManager: sws_getContext failed for fmt=%d", src_frame->format);
            return;
        }
    }

    // Convert the most recent decoded frame to RGBA.
    sws_scale(
        stream.sws_ctx,
        src_frame->data, src_frame->linesize,
        0, stream.height,
        stream.rgba_frame->data, stream.rgba_frame->linesize);

    if (scene_2d_renderer_->CreateOrUpdateExternalTexture(
            stream.rgba_scratch.data(), stream.width, stream.height, stream.frame_texture))
    {
        if (!stream.first_frame_uploaded)
        {
            SDL_Log("VideoPlaybackManager: first frame uploaded for '%s'", stream.source_path.c_str());
        }
        stream.first_frame_uploaded = true;
    }
}

void VideoPlaybackManager::Update(
    float delta_time,
    const SceneMetadata& scene_metadata,
    const std::filesystem::path& project_root)
{
    if (scene_2d_renderer_ == nullptr)
    {
        return;
    }

    std::unordered_map<StreamKey, bool, StreamKeyHash> seen;
    seen.reserve(streams_.size());

    std::size_t active_count = 0;
    for (std::size_t obj_idx = 0; obj_idx < scene_metadata.objects.size(); ++obj_idx)
    {
        const SceneObjectMetadata& object = scene_metadata.objects[obj_idx];
        for (std::size_t attr_idx = 0; attr_idx < object.attributes.size(); ++attr_idx)
        {
            const SceneObjectAttribute& attr = object.attributes[attr_idx];
            if (attr.kind != SceneObjectAttributeKind::Video2D)
            {
                continue;
            }
            const SceneObjectVideo2DAttributes& vid = attr.video_2d;
            if (vid.video_path.empty())
            {
                continue;
            }
            if (active_count >= kMaxStreams)
            {
                static bool warned = false;
                if (!warned)
                {
                    SDL_Log("VideoPlaybackManager: hit max stream cap (%zu); extras skipped", kMaxStreams);
                    warned = true;
                }
                continue;
            }
            ++active_count;

            const StreamKey key{object.name, attr_idx};
            seen[key] = true;

            const bool from_pak = project_root.empty() && g_asset_reader != nullptr;
            std::filesystem::path resolve_path;
            std::string current_source;
            if (from_pak)
            {
                resolve_path = std::filesystem::path(vid.video_path);
                current_source = std::string("pak:") + resolve_path.generic_string();
            }
            else
            {
                resolve_path = (vid.video_path.front() == '/' ||
                                (vid.video_path.size() >= 2 && vid.video_path[1] == ':'))
                    ? std::filesystem::path(vid.video_path)
                    : (project_root / vid.video_path);
                current_source = resolve_path.generic_string();
            }

            auto it = streams_.find(key);
            if (it != streams_.end() && it->second->source_path != current_source)
            {
                ReleaseStream(*it->second);
                streams_.erase(it);
                it = streams_.end();
            }

            if (it == streams_.end())
            {
                auto stream = std::make_unique<VideoStream>();
                stream->source_path = current_source;
                if (from_pak)
                {
                    stream->file_bytes = ReadAssetFileAsBytes(resolve_path.generic_string());
                    if (stream->file_bytes.empty())
                    {
                        SDL_Log("VideoPlaybackManager: pak file '%s' missing or empty",
                                resolve_path.generic_string().c_str());
                        ReleaseStream(*stream);
                        continue;
                    }
                }
                if (!OpenStream(*stream, resolve_path, from_pak))
                {
                    ReleaseStream(*stream);
                    continue;
                }
                auto [ins_it, _] = streams_.emplace(key, std::move(stream));
                it = ins_it;
            }

            VideoStream& stream = *it->second;

            if (stream.last_play_mode != vid.play_mode)
            {
                if (vid.play_mode != SceneObjectVideoPlayMode::Off &&
                    stream.last_play_mode == SceneObjectVideoPlayMode::Off)
                {
                    RewindStream(stream);
                }
                stream.last_play_mode = vid.play_mode;
            }
            stream.volume = vid.volume;
            stream.muted = vid.muted;

            DecodeAndUpload(stream, delta_time, vid);
            UpdateAudio(stream, vid);
        }
    }

    for (auto it = streams_.begin(); it != streams_.end();)
    {
        if (seen.find(it->first) == seen.end())
        {
            ReleaseStream(*it->second);
            it = streams_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

const Scene2DRenderer::GpuTexture* VideoPlaybackManager::GetFrameTexture(
    const std::string& object_name,
    std::size_t attribute_index) const
{
    const auto it = streams_.find(StreamKey{object_name, attribute_index});
    if (it == streams_.end() || !it->second->first_frame_uploaded)
    {
        return nullptr;
    }
    return &it->second->frame_texture;
}

void VideoPlaybackManager::UpdateAudio(VideoStream& stream, const SceneObjectVideo2DAttributes& attr)
{
    if (stream.audio_sound == nullptr)
    {
        return;
    }

    const bool want_playing =
        attr.play_mode != SceneObjectVideoPlayMode::Off &&
        !(attr.play_mode == SceneObjectVideoPlayMode::PlayOnce && stream.finished_once);

    ma_sound_set_looping(stream.audio_sound,
        attr.play_mode == SceneObjectVideoPlayMode::Loop ? MA_TRUE : MA_FALSE);

    const float effective_volume = attr.muted ? 0.0f : std::clamp(attr.volume, 0.0f, 1.0f);
    ma_sound_set_volume(stream.audio_sound, effective_volume);

    // Hold off on starting audio until the first video frame is on screen so
    // the soundtrack and picture line up. Once started, the video clock is
    // slaved to the audio cursor (see DecodeAndUpload).
    const bool can_start = want_playing && stream.first_frame_uploaded;

    if (can_start && !stream.audio_started)
    {
        ma_sound_seek_to_pcm_frame(stream.audio_sound, 0);
        ma_sound_start(stream.audio_sound);
        stream.audio_started = true;
    }
    else if (!want_playing && stream.audio_started)
    {
        ma_sound_stop(stream.audio_sound);
        stream.audio_started = false;
    }
}
