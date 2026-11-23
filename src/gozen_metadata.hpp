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


// One-shot media catalog probe, the home of every media fact.
//
// open() probes every stream of the source, keeps a snapshot of the catalog, then releases the
// FFmpeg contexts at once: it never holds a connection.
// Getters taking an index use absolute stream indices and return zero or empty on an invalid one.
class GoZenMetadata : public Resource {
	GDCLASS(GoZenMetadata, Resource);

  public:
	GoZenMetadata() = default;
	~GoZenMetadata() { close(); }

	// Custom HTTP headers of a network open(), as FFmpeg's headers option.
	inline void set_headers(const String& p_headers) { headers = p_headers; }
	inline String get_headers() const { return headers; }

	// Seconds the blocking open may stall before it fails, 0 for infinite.
	inline void set_network_timeout(double seconds) {
		network_timeout_us = seconds > 0.0 ? (int64_t)(seconds * 1'000'000.0) : 0;
	}
	inline double get_network_timeout() const { return (double)network_timeout_us / 1'000'000.0; }

	// Probes path, replacing any previous snapshot. Fails at once after cancel().
	Error open(const String& path);
	// Drops the snapshot.
	void close();
	// Aborts the current blocking open() and makes every later one fail at once: a cancelled
	// instance is dead. Callable from any thread.
	inline void cancel() { interrupt.aborted.store(true, std::memory_order_relaxed); }
	// True while a snapshot is held.
	inline bool is_open() const { return metadata.valid; }

	// Container: duration in µs (0 when unknown), bit rate in bit/s, format name and tags.
	inline int64_t get_duration_us() const { return metadata.duration_us; }
	inline int64_t get_bit_rate() const { return metadata.bit_rate; }
	inline String get_format_name() const { return metadata.format_name; }
	inline Dictionary get_format_tags() const { return metadata.format_tags; }

	// Stream count, and the indices of the video, audio and subtitle streams.
	inline int get_stream_count() const { return (int)metadata.streams.size(); }
	inline PackedInt32Array get_video_streams() const { return metadata.video_streams; }
	inline PackedInt32Array get_audio_streams() const { return metadata.audio_streams; }
	inline PackedInt32Array get_subtitle_streams() const { return metadata.subtitle_streams; }

	// Codec, title, language and tags of a stream of any type.
	String get_stream_codec(int index) const;
	String get_stream_title(int index) const;
	String get_stream_language(int index) const;
	Dictionary get_stream_metadata(int index) const;

	// Video stream facts: resolution in pixels, framerate in FPS, rotation in degrees.
	Vector2i get_video_resolution(int index) const;
	float get_video_framerate(int index) const;
	String get_video_pixel_format(int index) const;
	int get_video_rotation(int index) const;
	float get_video_sample_aspect_ratio(int index) const;
	String get_video_color_primaries(int index) const;

	// Audio stream facts: sample rate in Hz, channel count and layout.
	int get_audio_sample_rate(int index) const;
	int get_audio_channels(int index) const;
	String get_audio_channel_layout(int index) const;

	// Chapter count, and the bounds in µs and tags of each chapter.
	inline int get_chapter_count() const { return (int)metadata.chapters.size(); }
	int64_t get_chapter_start_us(int chapter_index) const;
	int64_t get_chapter_end_us(int chapter_index) const;
	Dictionary get_chapter_metadata(int chapter_index) const;

  protected:
	static void _bind_methods();

  private:
	String headers;
	MediaMetadata metadata;

	InterruptState interrupt;               // Abort and deadline of open().
	int64_t network_timeout_us = 5'000'000; // 0 for infinite.

	// True when a snapshot is held and index names one of its streams, of the given type for the
	// video and audio checks, or one of its chapters. Logs a false result.
	bool _valid_stream(int index) const;
	bool _valid_video(int index) const;
	bool _valid_audio(int index) const;
	bool _valid_chapter(int index) const;

	// Prints message as an error.
	static inline void _log_err(const String& message) {
		UtilityFunctions::printerr("GoZenMetadata: ", message, "!");
	}
};
