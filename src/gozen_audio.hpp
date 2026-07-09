#pragma once

#include "ffmpeg.hpp"
#include "ffmpeg_helpers.hpp"

#include <cstring>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;


class GoZenAudio : public Resource {
	GDCLASS(GoZenAudio, Resource);

  public:
	static PackedByteArray get_audio_data(const String& file_path, int stream_index = -1, bool stereo = true,
										  int sample_rate = 44100);
	static Ref<AudioStreamWAV> get_audio_stream_wav(const String& file_path, int stream_index = -1, bool stereo = true,
													int sample_rate = 44100);

  protected:
	static void _bind_methods();

  private:
	static PackedByteArray _decode_to_pba(AudioDecodePipeline& pipe);

	static inline void _log(String message) { UtilityFunctions::print("GoZenAudio: ", message, "."); }
	static inline bool _log_err(String message) {
		UtilityFunctions::printerr("GoZenAudio: ", message, "!");
		return true;
	}
};
