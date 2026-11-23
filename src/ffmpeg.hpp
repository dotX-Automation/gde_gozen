#pragma once

extern "C" {
#include "libavformat/avio.h"

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "ffmpeg_helpers.hpp"

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <vector>

using namespace godot;


// --- Media-catalog data types ---

// Media type a container open sets up, ALL for every stream.
// It sets the RTSP demuxer's allowed_media_types option, and the server then transmits only that
// type. Other inputs ignore it.
enum class StreamMediaType { ALL, VIDEO, AUDIO };

// Facts of one stream, backing GoZenMetadata's getters. The video fields are zero or empty on any
// other stream, and so are the audio fields.
struct StreamInfo {
	int index = 0;
	AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
	String codec, title, language;
	Dictionary tags; // Every tag of the stream.
	// Video: size in pixels, rotation in degrees, framerate in FPS.
	int width = 0, height = 0, rotation = 0;
	float framerate = 0.f, sample_aspect_ratio = 0.f;
	String pixel_format, color_primaries;
	// Audio: sample rate in Hz.
	int sample_rate = 0, channels = 0;
	String channel_layout;
};

// One chapter: its bounds in µs and its tags.
struct ChapterInfo {
	int64_t start_us = 0, end_us = 0;
	Dictionary tags;
};

// Catalog snapshot of a container, filled by populate_media_metadata and held only by GoZenMetadata.
struct MediaMetadata {
	bool valid = false;
	String format_name;
	int64_t duration_us = 0; // 0 when unknown.
	int64_t bit_rate = 0;    // In bit/s.
	Dictionary format_tags;  // Every tag of the container.
	PackedInt32Array video_streams, audio_streams, subtitle_streams;
	std::vector<StreamInfo> streams; // Indexed by absolute stream index.
	std::vector<ChapterInfo> chapters;
};


// --- FFmpeg utility class ---

// Static FFmpeg helpers shared by the decoders.
class FFmpeg {
  public:
	const static int AVIO_CTX_BUFFER_SIZE = 4 * 1024 * 1024; // AVIO buffer of a memory source, in bytes.

	// Prints message followed by FFmpeg's description of error.
	static void print_av_error(const char* message, int error);

	// Sets the decoder's threading, before avcodec_open2. thread_count is the processor count minus
	// one, clamped to [1, 16]. A finite source gets frame threading, a live one slice threading or a
	// single thread. Frame threading delays output by thread_count - 1 frames.
	static void enable_multithreading(AVCodecContext* codec_ctx, const AVCodec* codec, bool live);
	// Receives the next decoded frame of stream stream_id, reading and sending packets as the decoder
	// asks. Returns 0 with a frame, else a negative AVERROR: AVERROR_EOF once drained, AVERROR_EXIT
	// when the interrupt callback fired. A read past its deadline or cancelled fails even when
	// libavformat returned a packet.
	static int get_frame(AVFormatContext* format_ctx, AVCodecContext* codec_ctx, int stream_id, AVFrame* frame,
						 AVPacket* packet);
	// Returns *hw_pix_fmt when the AV_PIX_FMT_NONE-terminated list pix_fmt holds it, else AV_PIX_FMT_NONE.
	static enum AVPixelFormat get_hw_format(const enum AVPixelFormat* pix_fmt, enum AVPixelFormat* hw_pix_fmt);

	// AVIO read and seek callbacks over the BufferData in opaque. seek_buffer answers AVSEEK_SIZE.
	static int read_buffer_packet(void* opaque, uint8_t* buffer, int buffer_size);
	static int64_t seek_buffer(void* opaque, int64_t offset, int where);
};


// --- Interrupt / timeout helpers ---

// Arms the deadline of s timeout_us µs from now, or clears it when timeout_us <= 0.
inline void arm_deadline(InterruptState& s, int64_t timeout_us) {
	s.deadline_us.store(timeout_us > 0 ? av_gettime_relative() + timeout_us : 0, std::memory_order_relaxed);
}

// Installs on ctx the interrupt callback that libavformat polls during blocking I/O, reading state.
// state must outlive ctx.
void install_interrupt_callback(AVFormatContext* ctx, InterruptState* state);


// --- Container open + metadata ---

// Opens path, or bytes for a res:// or user:// path, and runs avformat_find_stream_info.
// RTSP runs over TCP, headers are sent as HTTP headers and restrict_to limits the streams set up.
// A memory source fills out_avio and out_buffer_data, and bytes must outlive the contexts.
// Fails at once on a cancelled interrupt. Returns false on failure, after logging it.
bool open_format_context(const String& path, const PackedByteArray& bytes, const String& headers,
						 StreamMediaType restrict_to, UniqueAVFormatCtxInput& out_format_ctx,
						 UniqueAVIOContext& out_avio, std::unique_ptr<BufferData>& out_buffer_data,
						 InterruptState* interrupt = nullptr);

// Resets out and fills it from format_ctx: container facts, typed facts and tags of each stream, and
// chapters. Sets out.valid unless format_ctx is null.
// Reads only parsed structures: call it after avformat_find_stream_info.
void populate_media_metadata(AVFormatContext* format_ctx, MediaMetadata& out);


// --- Shared audio pipeline helpers (used by AudioStreamFFmpeg and GoZenAudio) ---

// Opens audio stream stream_index of path (-1 for the first one), its decoder, and a resampler to
// out_layout and out_fmt at out_sample_rate Hz (0 keeps the source rate). use_multithreading is for
// finite sources only. For a memory source, bytes must outlive out.
// Returns false on failure, after logging it.
bool open_audio_pipeline(const String& path, const PackedByteArray& bytes, const String& headers,
						 int stream_index, const AVChannelLayout& out_layout, AVSampleFormat out_fmt,
						 int out_sample_rate, bool use_multithreading, AudioDecodePipeline& out,
						 InterruptState* interrupt = nullptr);

// Decodes the next audio frame and resamples it into scratch, growing scratch as needed.
// Returns the samples per channel written, 0 when the resampler produced none, or a negative
// AVERROR at the end of the stream or on an error.
int decode_audio_frame(AudioDecodePipeline& pipe, AVFrame* frame, AVPacket* packet, std::vector<uint8_t>& scratch);

// Drains the samples the resampler still buffers into scratch, after the end of the stream.
// Returns the samples per channel written, 0 on error.
int flush_audio_resampler(AudioDecodePipeline& pipe, std::vector<uint8_t>& scratch);
