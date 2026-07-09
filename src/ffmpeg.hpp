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

// Restricts which media streams a container open will set up. For RTSP this maps to FFmpeg's
// `allowed_media_types` option, so the server only transmits the requested type (bandwidth saving).
// No-op for non-RTSP inputs. ALL = open every stream.
enum class StreamMediaType { ALL, VIDEO, AUDIO };

// One decoded stream's facts (typed; backs GoZenMetadata's dedicated getters). Video-only fields are
// zero/empty for non-video streams and vice-versa.
struct StreamInfo {
	int index = 0;
	AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
	String codec, title, language;
	Dictionary tags; // arbitrary per-stream tags
	// video
	int width = 0, height = 0, rotation = 0;
	float framerate = 0.f, sample_aspect_ratio = 0.f;
	String pixel_format, color_primaries;
	// audio
	int sample_rate = 0, channels = 0;
	String channel_layout;
};

struct ChapterInfo {
	int64_t start_us = 0, end_us = 0; // native µs
	Dictionary tags;
};

// Full container snapshot. Owned only by GoZenMetadata; decoders do not embed this.
struct MediaMetadata {
	bool valid = false;
	String format_name;
	int64_t duration_us = 0; // native µs (format_ctx->duration)
	int64_t bit_rate = 0;
	Dictionary format_tags; // arbitrary container tags
	PackedInt32Array video_streams, audio_streams, subtitle_streams;
	std::vector<StreamInfo> streams;   // indexed by absolute stream index
	std::vector<ChapterInfo> chapters;
};


// --- FFmpeg utility class ---

class FFmpeg {
  public:
	const static int AVIO_CTX_BUFFER_SIZE = 4 * 1024 * 1024; // 4 MB

	static void print_av_error(const char* message, int error);

	static void enable_multithreading(AVCodecContext* codec_ctx, const AVCodec* codec);
	static int get_frame(AVFormatContext* format_ctx, AVCodecContext* codec_ctx, int stream_id, AVFrame* frame,
						 AVPacket* packet);
	static enum AVPixelFormat get_hw_format(const enum AVPixelFormat* pix_fmt, enum AVPixelFormat* hw_pix_fmt);

	static int read_buffer_packet(void* opaque, uint8_t* buffer, int buffer_size);	// For `res://` videos.
	static int64_t seek_buffer(void* opaque, int64_t offset, int where);			// For `res://` videos.
};


// --- Interrupt / timeout helpers ---

// Arm (or clear) an InterruptState's deadline: now + timeout, or 0 when timeout <= 0 (infinite).
inline void arm_deadline(InterruptState& s, int64_t timeout_us) {
	s.deadline_us.store(timeout_us > 0 ? av_gettime_relative() + timeout_us : 0, std::memory_order_relaxed);
}

// Installs the shared interrupt callback on `ctx` (polled by libavformat during blocking I/O).
// `state` must outlive `ctx`. Defined in ffmpeg.cpp (ffmpeg_interrupt_cb is file-local there).
void install_interrupt_callback(AVFormatContext* ctx, InterruptState* state);


// --- Container open + metadata ---

// Opens an AVFormatContext from either a real path or an in-memory byte buffer (res:// / user://).
// Builds demux options from headers / rtsp_transport and runs avformat_find_stream_info.
// For memory sources, `bytes` MUST outlive the returned contexts; out_avio + out_buffer_data are
// populated (BufferData heap-allocated so the AVIO opaque has a stable address). On a real-path
// source they are left null. Returns true on success (fills out-params), false on failure (logs).
bool open_format_context(const String& path, const PackedByteArray& bytes, const String& headers,
						 StreamMediaType restrict_to, UniqueAVFormatCtxInput& out_format_ctx,
						 UniqueAVIOContext& out_avio, std::unique_ptr<BufferData>& out_buffer_data,
						 InterruptState* interrupt = nullptr);

// Walks `format_ctx` once and fills `out` (reset first). Sets out.valid on success. format-level facts,
// per-stream typed facts + raw tags, and chapters (µs). Reads only already-parsed container structures
// (call after avformat_find_stream_info).
void populate_media_metadata(AVFormatContext* format_ctx, MediaMetadata& out);


// --- Shared audio pipeline helpers (used by AudioStreamFFmpeg and GoZenAudio) ---

// Opens a full decode pipeline and configures SWR to output `out_layout`/`out_fmt` at
// `out_sample_rate` (0 = match the source rate). When `use_multithreading` is true, the
// decoder is opened multithreaded. For memory sources, `bytes` MUST outlive `out`.
// Returns true on success (fills `out`), false on failure (logs; leaves `out` empty).
bool open_audio_pipeline(const String& path, const PackedByteArray& bytes, const String& headers,
						 int stream_index, const AVChannelLayout& out_layout, AVSampleFormat out_fmt,
						 int out_sample_rate, bool use_multithreading, AudioDecodePipeline& out,
						 InterruptState* interrupt = nullptr);

// Decodes the next audio frame and resamples it to the pipeline's output format into `scratch`
// (resized as needed). Returns output samples-per-channel written (>=0; 0 = none this call),
// or <0 when there are no more frames (EOF) or on a fatal error.
int decode_audio_frame(AudioDecodePipeline& pipe, AVFrame* frame, AVPacket* packet, std::vector<uint8_t>& scratch);

// Drains the resampler's buffered delay after EOF into `scratch`. Returns output
// samples-per-channel written (>=0).
int flush_audio_resampler(AudioDecodePipeline& pipe, std::vector<uint8_t>& scratch);
