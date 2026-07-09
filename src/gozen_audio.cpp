#include "gozen_audio.hpp"


PackedByteArray GoZenAudio::get_audio_data(const String& file_path, int stream_index, bool stereo, int sample_rate) {
	if (sample_rate <= 0)
		sample_rate = 44100;

	PackedByteArray data;

	PackedByteArray file_buffer; // res:// / user:// bytes; MUST outlive the pipeline below
	if (file_path.begins_with("res://") || file_path.begins_with("user://")) {
		file_buffer = FileAccess::get_file_as_bytes(file_path);
		if (file_buffer.is_empty()) {
			_log_err("Couldn't load file from path '" + file_path + "'");
			return data;
		}
	}

	AVChannelLayout out_layout =
		stereo ? (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO : (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;

	AudioDecodePipeline pipe;
	if (!open_audio_pipeline(file_path, file_buffer, "", stream_index, out_layout, AV_SAMPLE_FMT_S16,
							 sample_rate, true, pipe))
		return data; // open_audio_pipeline already logged

	// GoZenAudio decodes the whole track to memory, so it requires a finite source.
	if (pipe.stream->duration == AV_NOPTS_VALUE && pipe.format_ctx->duration == AV_NOPTS_VALUE) {
		_log_err("Source has no finite duration (live stream?); use AudioStreamFFmpeg for streaming playback");
		return data;
	}

	return _decode_to_pba(pipe);
}


Ref<AudioStreamWAV> GoZenAudio::get_audio_stream_wav(const String& file_path, int stream_index, bool stereo,
													int sample_rate) {
	if (sample_rate <= 0)
		sample_rate = 44100;

	PackedByteArray pcm = get_audio_data(file_path, stream_index, stereo, sample_rate);
	if (pcm.is_empty())
		return Ref<AudioStreamWAV>(); // null: decode failed / source refused (already logged)

	Ref<AudioStreamWAV> wav;
	wav.instantiate();
	wav->set_data(pcm);
	wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
	wav->set_mix_rate(sample_rate);
	wav->set_stereo(stereo);
	return wav;
}


PackedByteArray GoZenAudio::_decode_to_pba(AudioDecodePipeline& pipe) {
	PackedByteArray audio_data;

	const int frame_bytes = pipe.out_channels * pipe.bytes_per_sample; // bytes per output sample (all channels)
	const int64_t MAX_SIZE = 2147483600;                               // ~2 GB ceiling (Godot PackedByteArray)

	double duration_sec = (pipe.stream->duration != AV_NOPTS_VALUE)
							  ? pipe.stream->duration * av_q2d(pipe.stream->time_base)
							  : (double)pipe.format_ctx->duration / AV_TIME_BASE;

	int64_t estimated = (int64_t)(duration_sec * pipe.sample_rate) * frame_bytes;
	if (estimated < 4096)
		estimated = 4096; // unknown/short: start small, grow geometrically
	if (estimated >= MAX_SIZE) {
		_log_err("Audio is too big, cut the source into smaller parts in order to use");
		return audio_data;
	}
	audio_data.resize(estimated);

	UniqueAVFrame frame = make_unique_avframe();
	UniqueAVPacket packet = make_unique_avpacket();
	std::vector<uint8_t> scratch;

	int64_t audio_size = 0;
	uint8_t* dst = audio_data.ptrw();
	bool overflow = false;

	auto append = [&](int converted) {
		if (converted <= 0 || overflow)
			return;
		int64_t byte_size = (int64_t)converted * frame_bytes;
		if (audio_size + byte_size > audio_data.size()) {
			int64_t need = audio_size + byte_size;
			if (need >= MAX_SIZE) {
				_log_err("Audio exceeds the 2GB limit; truncating. Cut the source into smaller parts");
				overflow = true;
				return;
			}
			int64_t new_size = audio_data.size() * 2; // geometric growth
			if (new_size < need)
				new_size = need;
			if (new_size >= MAX_SIZE)
				new_size = MAX_SIZE;
			audio_data.resize(new_size);
			dst = audio_data.ptrw(); // re-fetch after a grow
		}
		memcpy(dst + audio_size, scratch.data(), byte_size);
		audio_size += byte_size;
	};

	int n;
	while (!overflow && (n = decode_audio_frame(pipe, frame.get(), packet.get(), scratch)) >= 0)
		append(n);
	if (!overflow)
		append(flush_audio_resampler(pipe, scratch)); // drain the resampler delay tail

	audio_data.resize(audio_size);
	return audio_data;
}

void GoZenAudio::_bind_methods() {
	ClassDB::bind_static_method("GoZenAudio",
		D_METHOD("get_audio_data", "file_path", "stream_index", "stereo", "sample_rate"),
		&GoZenAudio::get_audio_data, DEFVAL(-1), DEFVAL(true), DEFVAL(44100));
	ClassDB::bind_static_method("GoZenAudio",
		D_METHOD("get_audio_stream_wav", "file_path", "stream_index", "stereo", "sample_rate"),
		&GoZenAudio::get_audio_stream_wav, DEFVAL(-1), DEFVAL(true), DEFVAL(44100));
}
