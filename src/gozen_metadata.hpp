#pragma once

#include "ffmpeg.hpp"
#include "ffmpeg_helpers.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

using namespace godot;


// Standalone media-metadata probe. open() probes ALL media types, snapshots the full catalog into an
// internal MediaMetadata, then releases the FFmpeg contexts immediately (it never holds a connection).
class GoZenMetadata : public Resource {
	GDCLASS(GoZenMetadata, Resource);

  public:
	GoZenMetadata() = default;
	~GoZenMetadata() { close(); }

	inline void set_headers(const String& p_headers) { headers = p_headers; }
	inline String get_headers() const { return headers; }

	inline void set_network_timeout(double seconds) {
		network_timeout_us = seconds > 0.0 ? (int64_t)(seconds * 1'000'000.0) : 0;
	}
	inline double get_network_timeout() const { return (double)network_timeout_us / 1'000'000.0; }

	Error open(const String& path);
	void close();
	inline bool is_open() const { return metadata.valid; }

	// Container
	inline int64_t get_duration_us() const { return metadata.duration_us; }
	inline int64_t get_bit_rate() const { return metadata.bit_rate; }
	inline String get_format_name() const { return metadata.format_name; }
	inline Dictionary get_format_tags() const { return metadata.format_tags; }

	// Stream enumeration
	inline int get_stream_count() const { return (int)metadata.streams.size(); }
	inline PackedInt32Array get_video_streams() const { return metadata.video_streams; }
	inline PackedInt32Array get_audio_streams() const { return metadata.audio_streams; }
	inline PackedInt32Array get_subtitle_streams() const { return metadata.subtitle_streams; }

	// Per-stream (any type), by absolute index
	String get_stream_codec(int index) const;
	String get_stream_title(int index) const;
	String get_stream_language(int index) const;
	Dictionary get_stream_metadata(int index) const;

	// Video-stream intrinsics
	Vector2i get_video_resolution(int index) const;
	float get_video_framerate(int index) const;
	String get_video_pixel_format(int index) const;
	int get_video_rotation(int index) const;
	float get_video_sample_aspect_ratio(int index) const;
	String get_video_color_primaries(int index) const;

	// Audio-stream intrinsics
	int get_audio_sample_rate(int index) const;
	int get_audio_channels(int index) const;
	String get_audio_channel_layout(int index) const;

	// Chapters
	inline int get_chapter_count() const { return (int)metadata.chapters.size(); }
	int64_t get_chapter_start_us(int chapter_index) const;
	int64_t get_chapter_end_us(int chapter_index) const;
	Dictionary get_chapter_metadata(int chapter_index) const;

  protected:
	static void _bind_methods();

  private:
	String headers;
	MediaMetadata metadata;

	InterruptState interrupt;               // deadline for the one-shot probe (no cancel: synchronous)
	int64_t network_timeout_us = 5'000'000; // 5 s default; 0 = infinite

	bool _valid_stream(int index) const;
	bool _valid_video(int index) const;
	bool _valid_audio(int index) const;
	bool _valid_chapter(int index) const;

	static inline void _log_err(const String& message) {
		UtilityFunctions::printerr("GoZenMetadata: ", message, "!");
	}
};
