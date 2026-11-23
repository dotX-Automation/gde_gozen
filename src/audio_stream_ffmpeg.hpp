#pragma once

#include "ffmpeg.hpp"
#include "ffmpeg_helpers.hpp"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_playback_resampled.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

using namespace godot;

// One interleaved stereo S16 frame.
struct AudioSampleS16Stereo {
	int16_t l;
	int16_t r;
};

// Ring buffer of stereo S16 frames, with the capacity set by reset().
// One producer and one consumer, serialized by the owning playback's decode_mutex.
class AudioSampleRing {
  public:
	// Sets the capacity to frames, at least 1, and empties the ring.
	void reset(size_t frames) {
		_data.assign(frames ? frames : 1, AudioSampleS16Stereo{0, 0});
		_head = 0;
		_count = 0;
	}
	void clear() {
		_head = 0;
		_count = 0;
	}
	size_t capacity() const { return _data.size(); }
	size_t count() const { return _count; }
	size_t space() const { return _data.size() - _count; }

	// Appends up to n frames of src, as many as fit, and returns their count.
	size_t write(const AudioSampleS16Stereo* src, size_t n);
	// Moves up to n frames into dst and returns their count.
	size_t read(AudioSampleS16Stereo* dst, size_t n);

  private:
	std::vector<AudioSampleS16Stereo> _data;
	size_t _head = 0;  // Index of the oldest frame.
	size_t _count = 0; // Frames held.
};

class AudioStreamFFmpegPlayback;

// Streaming AudioStream over FFmpeg, the audio path of live sources.
//
// open() probes the source and keeps the opened pipeline for the first playback to adopt.
// Each playback decodes on the audio thread as the audio server pulls it, to stereo at the source
// rate.
// It exposes the playback facts only: the media catalog is GoZenMetadata's.
class AudioStreamFFmpeg : public AudioStream {
	GDCLASS(AudioStreamFFmpeg, AudioStream);
	friend class AudioStreamFFmpegPlayback;

  public:
	AudioStreamFFmpeg() = default;
	~AudioStreamFFmpeg() { close(); }

	// Opens audio stream stream_index of path (-1 for the first one), closing any previous source.
	// Fails at once after cancel().
	Error open(const String& path, int stream_index = -1);
	// Releases the probe pipeline. Playbacks keep their own.
	void close();
	// Aborts the current blocking open() and makes every later one fail at once: a cancelled
	// instance is dead. Callable from any thread. Existing playbacks read on their own interrupt.
	inline void cancel() { interrupt.aborted.store(true, std::memory_order_relaxed); }
	inline bool is_open() const { return loaded; }
	// False once a playback's steady-state read failed, by a timeout, cancel() or the end of the
	// stream, until open() or the next seek. MediaPlayback polls it to reconnect a live source.
	inline bool is_stream_healthy() const { return !read_failed.load(std::memory_order_relaxed); }
	// Output sample rate in Hz, the source's own.
	int get_sample_rate() const { return sample_rate; }
	// True when the source has two or more channels. The output is stereo either way.
	bool is_stereo() const { return stereo; }

	// Custom HTTP headers of a network open(), as FFmpeg's headers option.
	void set_headers(const String& headers_str) { headers = headers_str; }
	String get_headers() const { return headers; }

	// Seconds a blocking open or read may stall before it fails, 0 for infinite.
	void set_network_timeout(double seconds) {
		network_timeout_us = seconds > 0.0 ? (int64_t)(seconds * 1'000'000.0) : 0;
	}
	double get_network_timeout() const { return (double)network_timeout_us / 1'000'000.0; }

	// Length in seconds, 0 when unknown.
	double _get_length() const override { return length; }
	bool _is_monophonic() const override { return !stereo; }
	// Null until open() succeeds.
	Ref<AudioStreamPlayback> _instantiate_playback() const override;

  protected:
	static void _bind_methods();

  private:
	// Moves the probe pipeline into out, once. False when none is available.
	bool take_probe_pipeline(AudioDecodePipeline& out);

	// Source of the last open().
	String file_path;
	String headers;
	PackedByteArray file_buffer; // Bytes of a res:// or user:// source, shared with the playbacks.

	// Facts of the last open().
	bool loaded = false;
	std::atomic<bool> read_failed{false}; // Latched by a playback on a failed steady-state read.
	bool stereo = true;
	int sample_rate = 44100;
	double length = 0;
	int audio_stream_index = -1;

	InterruptState interrupt;               // Abort and deadline of open(), cleared when it returns.
	int64_t network_timeout_us = 5'000'000; // 0 for infinite.

	// Probe pipeline kept for the first playback to adopt, guarded by probe_mutex.
	AudioDecodePipeline probe_pipeline;
	bool probe_available = false;
	std::mutex probe_mutex;

	// Prints message as an error.
	static inline bool _log_err(String message) {
		UtilityFunctions::printerr("GoZenAudioStream: ", message, "!");
		return true;
	}
};

// Playback of an AudioStreamFFmpeg: its own decode pipeline feeding a one-second ring, mixed on the
// audio thread.
class AudioStreamFFmpegPlayback : public AudioStreamPlaybackResampled {
	GDCLASS(AudioStreamFFmpegPlayback, AudioStreamPlaybackResampled);
	friend class AudioStreamFFmpeg;

  public:
	AudioStreamFFmpegPlayback() {
		av_packet = make_unique_ffmpeg<AVPacket, AVPacketDeleter>(av_packet_alloc());
		av_frame = make_unique_ffmpeg<AVFrame, AVFrameDeleter>(av_frame_alloc());
	}
	~AudioStreamFFmpegPlayback() override;

	inline float _get_stream_sampling_rate() const override { return mix_rate; }
	// Fills p_buffer from the ring, decoding until the ring holds p_frames or a read fails, under a
	// fresh network_timeout deadline. Returns the frames written. Silent once the stream is unhealthy.
	int32_t _mix_resampled(AudioFrame* p_buffer, int32_t p_frames) override;

	inline int32_t _get_loop_count() const override { return 0; }
	inline double _get_playback_position() const override { return double(mixed.load()) / double(mix_rate); }
	inline bool _is_playing() const override { return is_playing; }
	// Seeks to p_position seconds, refills the ring from there and clears a latched read failure.
	void _seek(double p_position) override;
	// Opens the pipeline when it has none, then seeks to p_from_pos.
	void _start(double p_from_pos) override;
	// Stops mixing. The pipeline stays open.
	void _stop() override;

  protected:
	static inline void _bind_methods() {}

  private:
	// Adopts the stream's probe pipeline, or opens its own when none is available, either way on
	// playback_interrupt, and sizes the ring. True when the pipeline is open. Call under decode_mutex.
	bool ensure_open();
	// Decodes one frame into the ring. False on a failed read, which latches the stream unhealthy.
	// Call under decode_mutex.
	bool fill();

	Ref<AudioStreamFFmpeg> stream; // Keeps the stream and its file_buffer alive.
	// Abort and deadline of the steady-state reads. Declared before pipe, which it outlives.
	InterruptState playback_interrupt;
	AudioDecodePipeline pipe;
	AudioSampleRing ring;
	std::vector<AudioSampleS16Stereo> scratch; // Staging of ring reads and of the seek's resampling.
	std::vector<uint8_t> decode_buf; // Output of decode_audio_frame.

	UniqueAVFrame av_frame;
	UniqueAVPacket av_packet;

	bool is_playing = false;
	std::atomic<uint32_t> mixed{0}; // Playback position in frames.
	uint32_t mix_rate = 44100;
	bool stereo = true;

	std::mutex decode_mutex; // Guards pipe and ring between the audio thread and _start() or _seek().
};
