#pragma once

#include "ffmpeg.hpp"
#include "ffmpeg_helpers.hpp"

#include <cstring>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;


// Batch decoder of a finite audio source into memory.
//
// Its static methods decode a whole audio stream to S16 PCM, and refuse a source without a finite
// duration: live sources play through AudioStreamFFmpeg.
class GoZenAudio : public Resource {
	GDCLASS(GoZenAudio, Resource);

  public:
	// Decodes audio stream stream_index of file_path (-1 for the first one) to interleaved S16 PCM,
	// stereo or mono, at sample_rate Hz (44100 when not positive). Empty on failure.
	static PackedByteArray get_audio_data(const String& file_path, int stream_index = -1, bool stereo = true,
										  int sample_rate = 44100);
	// get_audio_data() wrapped in a 16-bit AudioStreamWAV. Null on failure.
	static Ref<AudioStreamWAV> get_audio_stream_wav(const String& file_path, int stream_index = -1, bool stereo = true,
													int sample_rate = 44100);

  protected:
	static void _bind_methods();

  private:
	// Decodes pipe to its end into a PackedByteArray. Empty when the estimated size reaches about
	// 2 GB, truncated there when the decoded size does.
	static PackedByteArray _decode_to_pba(AudioDecodePipeline& pipe);

	// Prints message.
	static inline void _log(String message) { UtilityFunctions::print("GoZenAudio: ", message, "."); }
	// Prints message as an error.
	static inline bool _log_err(String message) {
		UtilityFunctions::printerr("GoZenAudio: ", message, "!");
		return true;
	}
};
