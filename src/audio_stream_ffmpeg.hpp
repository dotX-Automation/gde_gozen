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

struct AudioSampleS16Stereo {
	int16_t l;
	int16_t r;
};

// Bounded ring buffer of interleaved stereo S16 frames. Fixed capacity set at
// reset(); single-producer/single-consumer, serialized externally by the owning
// playback's decode mutex.
class AudioSampleRing {
  public:
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

	size_t write(const AudioSampleS16Stereo* src, size_t n);
	size_t read(AudioSampleS16Stereo* dst, size_t n);

  private:
	std::vector<AudioSampleS16Stereo> _data;
	size_t _head = 0;  // read index
	size_t _count = 0; // frames available
};

class AudioStreamFFmpegPlayback; // forward declaration

class AudioStreamFFmpeg : public AudioStream {
	GDCLASS(AudioStreamFFmpeg, AudioStream);
	friend class AudioStreamFFmpegPlayback;

  public:
	AudioStreamFFmpeg() = default;
	~AudioStreamFFmpeg() { close(); }

	Error open(const String& path, int stream_index = -1);
	void close();
	// Marks this instance permanently dead: aborts any current/future blocking op on its
	// format context. Only call on an instance about to be discarded. Main-thread-safe (atomic).
	inline void cancel() { interrupt.aborted.store(true, std::memory_order_relaxed); }
	inline bool is_open() const { return loaded; }
	// True while the live decode is healthy; a playback latches this false on an interrupted
	// (timed-out / cancelled) steady-state read. GDScript polls this to drive reconnection.
	inline bool is_stream_healthy() const { return !live_failed.load(std::memory_order_relaxed); }
	int get_sample_rate() const { return sample_rate; }
	bool is_stereo() const { return stereo; }

	void set_headers(const String& headers_str) { headers = headers_str; }
	String get_headers() const { return headers; }

	void set_network_timeout(double seconds) {
		network_timeout_us = seconds > 0.0 ? (int64_t)(seconds * 1'000'000.0) : 0;
	}
	double get_network_timeout() const { return (double)network_timeout_us / 1'000'000.0; }

	double _get_length() const override { return length; }
	bool _is_monophonic() const override { return !stereo; }
	Ref<AudioStreamPlayback> _instantiate_playback() const override;

  protected:
	static void _bind_methods();

  private:
	bool take_probe_pipeline(AudioDecodePipeline& out);

	// Immutable source.
	String file_path;
	String headers;
	PackedByteArray file_buffer; // res:// / user:// bytes (COW, shared)

	// Probed facts.
	bool loaded = false;
	std::atomic<bool> live_failed{false}; // set by the playback on an interrupted steady-state read
	bool stereo = true;
	int sample_rate = 44100;
	double length = 0;
	int audio_stream_index = -1;

	InterruptState interrupt;               // deadline for open() only; steady-state fill() is untouched
	int64_t network_timeout_us = 5'000'000; // 5 s default; 0 = infinite

	// Probe pipeline kept for the first playback to adopt.
	AudioDecodePipeline probe_pipeline;
	bool probe_available = false;
	std::mutex probe_mutex;

	static inline void _log(String message) { UtilityFunctions::print("GoZenAudioStream: ", message, "."); }
	static inline bool _log_err(String message) {
		UtilityFunctions::printerr("GoZenAudioStream: ", message, "!");
		return true;
	}
};

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
	int32_t _mix_resampled(AudioFrame* p_buffer, int32_t p_frames) override;

	inline int32_t _get_loop_count() const override { return 0; }
	inline double _get_playback_position() const override { return double(mixed.load()) / double(mix_rate); }
	inline bool _is_playing() const override { return is_playing; }
	void _seek(double p_position) override;
	void _start(double p_from_pos) override;
	void _stop() override;

  protected:
	static inline void _bind_methods() {}

  private:
	bool ensure_open(); // adopt probe pipeline, else open own; auto-reconnect
	bool fill();        // decode one frame -> resample -> ring (call under decode_mutex)

	Ref<AudioStreamFFmpeg> stream; // keeps the resource + file_buffer alive
	InterruptState playback_interrupt; // owns steady-state read interruption; outlives pipe (declared before it)
	AudioDecodePipeline pipe;
	AudioSampleRing ring;
	std::vector<AudioSampleS16Stereo> scratch; // swr output staging
	std::vector<uint8_t> decode_buf; // reusable byte buffer for decode_audio_frame output

	UniqueAVFrame av_frame;
	UniqueAVPacket av_packet;

	bool is_playing = false;
	std::atomic<uint32_t> mixed{0};
	uint32_t mix_rate = 44100;
	bool stereo = true;

	std::mutex decode_mutex;
};
