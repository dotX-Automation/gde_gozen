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


class GoZenVideo : public Resource {
	GDCLASS(GoZenVideo, Resource);

  public:
	GoZenVideo() {}
	~GoZenVideo() { close(); }

	Error open(const String& video_path);
	void close();

	inline bool is_open() const { return loaded; }

	Error seek_frame(int frame_nr);
	bool next_frame(bool skip = false);

	inline void set_headers(const String& headers_str) { headers = headers_str; }
	inline String get_headers() const { return headers; }

	inline void cancel() { interrupt.aborted.store(true, std::memory_order_relaxed); }

	inline void set_network_timeout(double seconds) {
		network_timeout_us = seconds > 0.0 ? (int64_t)(seconds * 1'000'000.0) : 0;
	}
	inline double get_network_timeout() const { return (double)network_timeout_us / 1'000'000.0; }

	inline void set_sws_flag_bilinear() { sws_flag = SWS_BILINEAR; }
	inline void set_sws_flag_bicubic() { sws_flag = SWS_BICUBIC; }

	inline Ref<Image> get_y_data() const { return y_data; }
	inline Ref<Image> get_u_data() const { return u_data; }
	inline Ref<Image> get_v_data() const { return v_data; }
	inline Ref<Image> get_a_data() const { return a_data; }

	// Metadata getters
	inline String get_path() const { return path; }

	inline Vector2i get_resolution() const { return resolution; }

	inline int get_width() const { return resolution.x; }
	inline int get_height() const { return resolution.y; }
	inline int get_padding() const { return padding; }
	inline int get_rotation() const { return rotation; }
	inline int get_interlaced() const { return interlaced; }

	inline int64_t get_duration_us() const { return duration; }
	inline int64_t get_frame_count() const { return frame_count; };
	inline int64_t get_current_frame() const { return current_frame; }

	inline float get_aspect_ratio() const { return sar; }
	inline float get_framerate() const { return framerate; }

	inline String get_pixel_format() const { return pixel_format; }
	inline String get_color_profile() const {
		const char* name = av_color_primaries_name(color_profile);
		return name ? String(name) : String("");
	}

	inline bool has_alpha() const { return alpha_layer; }

	inline bool is_full_color_range() const { return full_color_range; }
	inline bool is_using_sws() const { return using_sws; }

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
	// FFmpeg classes.
	UniqueAVFormatCtxInput av_format_ctx;
	UniqueAVCodecCtx av_codec_ctx;
	UniqueAVIOContext avio_ctx;
	AVStream* av_stream = nullptr;

	UniqueAVPacket av_packet;
	UniqueAVFrame av_frame;
	UniqueAVFrame av_sws_frame;
	UniqueSwsCtx sws_ctx;

	enum AVColorPrimaries color_profile = AVCOL_PRI_UNSPECIFIED;

	std::unique_ptr<BufferData> buffer_data;

	InterruptState interrupt;                 // abort/timeout signal for open()/next_frame()/seek_frame()
	int64_t network_timeout_us = 5'000'000;   // 5 s default; 0 = infinite

	// Default variable types.
	int64_t current_frame = 0;
	int padding = 0;

	int8_t rotation = 0;
	int8_t interlaced = 0; // 0 = no interlacing, 1 = interlaced top first, 2 interlaced bottom first.

	int64_t duration = 0;
	int64_t frame_count = 0;

	int64_t start_time_video = 0;
	int64_t frame_timestamp = 0;
	int64_t current_pts = 0;

	double average_frame_duration = 0;
	double stream_time_base_video = 0;

	float sar = 0;
	float framerate = 0.;

	bool loaded = false; // Is true after open().
	bool debug = false;
	bool using_sws = false; // This is set for when the pixel format is foreign and not directly supported by the addon.
	bool full_color_range = true;

	int sws_flag = SWS_BILINEAR;

	// Godot classes.
	String path = "";
	String pixel_format = "";
	String headers = "";

	Vector2i resolution = Vector2i(0, 0);     // decoded/render size — exposed via get_resolution/width/height
	Vector2i src_resolution = Vector2i(0, 0); // SAR-adjusted display size (source property; not exposed)

	Ref<Image> y_data;
	Ref<Image> u_data;
	Ref<Image> v_data;
	Ref<Image> a_data;

	bool alpha_layer = false;

	PackedByteArray file_buffer; // For `res://` videos.

	// Private functions.
	void _copy_frame_data();

	int _seek_frame(int frame_nr);

	inline void _log(const String& message) {
		if (debug)
			UtilityFunctions::print("GoZenVideo: ", message, ".");
	}
	inline Error _log_err(const String& message) {
		UtilityFunctions::printerr("GoZenVideo: ", message, "!");
		return FAILED;
	}

  protected:
	static void _bind_methods();
};
