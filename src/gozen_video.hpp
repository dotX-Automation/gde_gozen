#pragma once

#include "ffmpeg.hpp"
#include "ffmpeg_helpers.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;


// Pull-based video frame decoder over FFmpeg.
//
// open() selects the first decodable video stream, decodes its first frame and seeks to frame 0.
// Each next_frame() or seek_frame() then leaves its frame's YUV(A) 4:2:0 planes in
// get_y/u/v/a_data(), converted through swscale from any other pixel format.
// It exposes the playback and render facts only: the media catalog is GoZenMetadata's.
class GoZenVideo : public Resource {
	GDCLASS(GoZenVideo, Resource);

  public:
	GoZenVideo() {}
	~GoZenVideo() { close(); }

	// Opens video_path: a file, a res:// or user:// path read whole into memory, or a network URL.
	// A live source, one without a frame count or a usable duration, gets no frame threading.
	// Fails when already open or once cancel() ran.
	Error open(const String& video_path);
	// Releases the decoder and the source. Idempotent.
	void close();

	inline bool is_open() const { return loaded; }

	// Seeks to frame frame_nr and decodes it into the planes. A seek that lands on the end of the
	// source retries one frame earlier, up to 10 times.
	Error seek_frame(int frame_nr);
	// Decodes the next frame into the planes, or only counts it when skip is true.
	// False on a failed read: the end of the source, an error, a timeout or cancel().
	bool next_frame(bool skip = false);

	// Custom HTTP headers of a network open(), as FFmpeg's headers option.
	inline void set_headers(const String& headers_str) { headers = headers_str; }
	inline String get_headers() const { return headers; }

	// Aborts the current blocking open(), seek_frame() or next_frame() and makes every later one fail
	// at once: a cancelled instance is dead. Callable from any thread.
	inline void cancel() { interrupt.aborted.store(true, std::memory_order_relaxed); }

	// Seconds a blocking open, seek or read may stall before it fails, 0 for infinite.
	inline void set_network_timeout(double seconds) {
		network_timeout_us = seconds > 0.0 ? (int64_t)(seconds * 1'000'000.0) : 0;
	}
	inline double get_network_timeout() const { return (double)network_timeout_us / 1'000'000.0; }

	// Scaling algorithm of the swscale conversion, read by open().
	inline void set_sws_flag_bilinear() { sws_flag = SWS_BILINEAR; }
	inline void set_sws_flag_bicubic() { sws_flag = SWS_BICUBIC; }

	// Planes of the current frame as FORMAT_R8 images: Y and A at the resolution, U and V at half of
	// it, rounded up. get_a_data() holds a plane only when has_alpha().
	inline Ref<Image> get_y_data() const { return y_data; }
	inline Ref<Image> get_u_data() const { return u_data; }
	inline Ref<Image> get_v_data() const { return v_data; }
	inline Ref<Image> get_a_data() const { return a_data; }

	// Path of the last open().
	inline String get_media_path() const { return path; }

	// Decoded frame size in pixels, which is the render size.
	inline Vector2i get_resolution() const { return resolution; }

	inline int get_width() const { return resolution.x; }
	inline int get_height() const { return resolution.y; }
	// Row padding of the decoded Y plane in bytes. The exposed planes carry none.
	inline int get_padding() const { return padding; }
	// Display rotation in degrees.
	inline int get_rotation() const { return rotation; }
	// 0 for progressive, 1 for top field first, 2 for bottom field first.
	inline int get_interlaced() const { return interlaced; }

	// Container duration in µs, 0 or less when unknown.
	inline int64_t get_duration_us() const { return duration; }
	// Frame count of the stream, or estimated from the duration. 0 or less when unknown, as for a
	// live source.
	inline int64_t get_frame_count() const { return frame_count; };
	// Index of the frame in the planes.
	inline int64_t get_current_frame() const { return current_frame; }

	// Sample aspect ratio, 0 when unknown.
	inline float get_aspect_ratio() const { return sar; }
	// Frames per second, 24 when the source states none.
	inline float get_framerate() const { return framerate; }

	// Pixel format name of the decoded frames.
	inline String get_pixel_format() const { return pixel_format; }
	// Color primaries name, as FFmpeg spells it.
	inline String get_color_profile() const {
		const char* name = av_color_primaries_name(color_profile);
		return name ? String(name) : String("");
	}

	inline bool has_alpha() const { return alpha_layer; }

	// True for full (JPEG) range, false for limited (MPEG) range.
	inline bool is_full_color_range() const { return full_color_range; }
	// True when swscale converts every frame.
	inline bool is_using_sws() const { return using_sws; }

	// Turns this instance's debug log on or off, and sets FFmpeg's global log level to verbose or info.
	inline void enable_debug() {
		av_log_set_level(AV_LOG_VERBOSE);
		debug = true;
	}
	inline void disable_debug() {
		av_log_set_level(AV_LOG_INFO);
		debug = false;
	}
	inline bool get_debug_enabled() const { return debug; }

  private:
	UniqueAVFormatCtxInput av_format_ctx;
	UniqueAVCodecCtx av_codec_ctx;
	UniqueAVIOContext avio_ctx;
	AVStream* av_stream = nullptr; // Borrowed from av_format_ctx.

	UniqueAVPacket av_packet;
	UniqueAVFrame av_frame;
	UniqueAVFrame av_sws_frame;
	UniqueSwsCtx sws_ctx;

	enum AVColorPrimaries color_profile = AVCOL_PRI_UNSPECIFIED;

	std::unique_ptr<BufferData> buffer_data; // AVIO opaque of a memory source, released after avio_ctx.

	InterruptState interrupt;                 // Abort and deadline of open(), seek_frame() and next_frame().
	int64_t network_timeout_us = 5'000'000;   // 0 for infinite.

	int64_t current_frame = 0;
	int padding = 0;

	int8_t rotation = 0;
	int8_t interlaced = 0; // 0 for progressive, 1 for top field first, 2 for bottom field first.

	int64_t duration = 0; // In µs.
	int64_t frame_count = 0;

	int64_t start_time_video = 0; // In 100 ns ticks.
	int64_t frame_timestamp = 0;  // Target of the last seek, in 100 ns ticks from the start.
	int64_t current_pts = 0;      // In stream time base units.

	double average_frame_duration = 0; // In 100 ns ticks.
	double stream_time_base_video = 0; // 100 ns ticks per stream time base unit.

	float sar = 0;
	float framerate = 0.;

	bool loaded = false; // True from a successful open() until close().
	bool debug = false;
	bool using_sws = false; // The decoded pixel format is not YUV420P, YUVJ420P or YUVA420P.
	bool full_color_range = true;

	int sws_flag = SWS_BILINEAR;

	String path = "";
	String pixel_format = "";
	String headers = "";

	Vector2i resolution = Vector2i(0, 0);     // Decoded size, which is the render size.
	Vector2i src_resolution = Vector2i(0, 0); // Display size: the decoded size scaled by the sample aspect ratio.

	Ref<Image> y_data;
	Ref<Image> u_data;
	Ref<Image> v_data;
	Ref<Image> a_data;

	bool alpha_layer = false;

	PackedByteArray file_buffer; // Bytes of a res:// or user:// source.

	// Copies the decoded frame into the planes, through swscale when using_sws.
	void _copy_frame_data();

	// Flushes the decoder and seeks back to the keyframe at or before frame_nr. Sets frame_timestamp
	// and returns av_seek_frame's result.
	int _seek_frame(int frame_nr);

	// Prints message when debug is on.
	inline void _log(const String& message) {
		if (debug)
			UtilityFunctions::print("GoZenVideo: ", message, ".");
	}
	// Prints message as an error and returns FAILED.
	inline Error _log_err(const String& message) {
		UtilityFunctions::printerr("GoZenVideo: ", message, "!");
		return FAILED;
	}

  protected:
	static void _bind_methods();
};
