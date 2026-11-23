#include "audio_stream_ffmpeg.hpp"

size_t AudioSampleRing::write(const AudioSampleS16Stereo* src, size_t n) {
	const size_t to_write = std::min(n, space());
	const size_t tail = (_head + _count) % _data.size();
	const size_t first = std::min(to_write, _data.size() - tail);
	std::memcpy(&_data[tail], src, first * sizeof(AudioSampleS16Stereo));
	if (to_write > first)
		std::memcpy(&_data[0], src + first, (to_write - first) * sizeof(AudioSampleS16Stereo));
	_count += to_write;
	return to_write;
}

size_t AudioSampleRing::read(AudioSampleS16Stereo* dst, size_t n) {
	const size_t to_read = std::min(n, _count);
	const size_t first = std::min(to_read, _data.size() - _head);
	std::memcpy(dst, &_data[_head], first * sizeof(AudioSampleS16Stereo));
	if (to_read > first)
		std::memcpy(dst + first, &_data[0], (to_read - first) * sizeof(AudioSampleS16Stereo));
	_head = (_head + to_read) % _data.size();
	_count -= to_read;
	return to_read;
}

Error AudioStreamFFmpeg::open(const String& path, int stream_index) {
	close();
	read_failed.store(false, std::memory_order_relaxed);

	file_path = path;
	if (path.begins_with("res://") || path.begins_with("user://"))
		file_buffer = FileAccess::get_file_as_bytes(path);

	AVChannelLayout stereo_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
	arm_deadline(interrupt, network_timeout_us);
	bool ok = open_audio_pipeline(file_path, file_buffer, headers, stream_index, stereo_layout,
								  AV_SAMPLE_FMT_S16, 0, false, probe_pipeline, &interrupt);

	interrupt.deadline_us.store(0, std::memory_order_relaxed);
	if (!ok)
		return FAILED;

	AVStream* s = probe_pipeline.stream;
	if (s->duration != AV_NOPTS_VALUE)
		length = s->duration * av_q2d(s->time_base);
	else if (probe_pipeline.format_ctx->duration != AV_NOPTS_VALUE)
		length = probe_pipeline.format_ctx->duration / (double)AV_TIME_BASE;
	else
		length = 0;

	sample_rate = probe_pipeline.sample_rate;
	stereo = probe_pipeline.stereo;
	audio_stream_index = s->index;

	probe_available = true;
	loaded = true;
	return OK;
}

void AudioStreamFFmpeg::close() {
	if (!loaded && !probe_available)
		return;

	{
		std::lock_guard<std::mutex> lock(probe_mutex);
		probe_pipeline = AudioDecodePipeline{};
		probe_available = false;
	}

	loaded = false;
	length = 0;
	audio_stream_index = -1;
}

bool AudioStreamFFmpeg::take_probe_pipeline(AudioDecodePipeline& out) {
	std::lock_guard<std::mutex> lock(probe_mutex);
	if (!probe_available || !probe_pipeline.valid())
		return false;
	out = std::move(probe_pipeline);
	probe_pipeline = AudioDecodePipeline{};
	probe_available = false;
	return true;
}

Ref<AudioStreamPlayback> AudioStreamFFmpeg::_instantiate_playback() const {
	if (!loaded)
		return nullptr;

	Ref<AudioStreamFFmpegPlayback> playback;
	playback.instantiate();
	playback->stream = Ref<AudioStreamFFmpeg>(const_cast<AudioStreamFFmpeg*>(this));
	playback->mix_rate = sample_rate;
	playback->stereo = stereo;
	return playback;
}

AudioStreamFFmpegPlayback::~AudioStreamFFmpegPlayback() {}

bool AudioStreamFFmpegPlayback::ensure_open() {
	if (pipe.valid())
		return true;
	if (stream.is_null())
		return false;

	if (stream->take_probe_pipeline(pipe) && pipe.valid()) {
		install_interrupt_callback(pipe.format_ctx.get(), &playback_interrupt);
	} else {
		AVChannelLayout stereo_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
		arm_deadline(playback_interrupt, stream->network_timeout_us);
		bool ok = open_audio_pipeline(stream->file_path, stream->file_buffer, stream->headers,
									  stream->audio_stream_index, stereo_layout, AV_SAMPLE_FMT_S16, 0, false, pipe,
									  &playback_interrupt);
		playback_interrupt.deadline_us.store(0, std::memory_order_relaxed);
		if (!ok)
			return false;
	}

	mix_rate = pipe.sample_rate;
	stereo = pipe.stereo;
	ring.reset((size_t)pipe.sample_rate);
	return true;
}

bool AudioStreamFFmpegPlayback::fill() {
	if (!pipe.valid())
		return false;

	int converted = decode_audio_frame(pipe, av_frame.get(), av_packet.get(), decode_buf);
	if (converted < 0) {
		stream->read_failed.store(true, std::memory_order_relaxed);
		return false;
	}
	if (converted == 0)
		return true;

	const AudioSampleS16Stereo* samples = reinterpret_cast<const AudioSampleS16Stereo*>(decode_buf.data());
	size_t written = ring.write(samples, (size_t)converted);
	if (written < (size_t)converted)
		UtilityFunctions::printerr("GoZenAudioStream: audio ring overflow, dropped ",
								   (int)((size_t)converted - written), " samples!");
	return true;
}

int32_t AudioStreamFFmpegPlayback::_mix_resampled(AudioFrame* p_buffer, int32_t p_frames) {
	std::lock_guard<std::mutex> lock(decode_mutex);
	if (!pipe.valid())
		return 0;
	if (stream->read_failed.load(std::memory_order_relaxed))
		return 0;

	arm_deadline(playback_interrupt, stream->network_timeout_us);

	while (ring.count() < (size_t)p_frames)
		if (!fill())
			break;

	const size_t available = std::min((size_t)p_frames, ring.count());
	if (available == 0)
		return 0;

	if ((size_t)scratch.size() < available)
		scratch.resize(available);
	ring.read(scratch.data(), available);

	for (size_t i = 0; i < available; ++i)
		p_buffer[i] = AudioFrame{(float)scratch[i].l / 32768.0f, (float)scratch[i].r / 32768.0f};

	mixed += (uint32_t)available;
	return (int32_t)available;
}

void AudioStreamFFmpegPlayback::_start(double p_from_pos) {
	{
		std::lock_guard<std::mutex> lock(decode_mutex);
		if (!ensure_open()) {
			is_playing = false;
			return;
		}
	}
	is_playing = true;
	mixed = 0;
	_seek(p_from_pos);
}

void AudioStreamFFmpegPlayback::_stop() {
	is_playing = false;
}

void AudioStreamFFmpegPlayback::_seek(double p_position) {
	std::lock_guard<std::mutex> lock(decode_mutex);
	if (!pipe.valid())
		return;

	ring.clear();
	stream->read_failed.store(false, std::memory_order_relaxed);

	int64_t target_ts = av_rescale_q((int64_t)(p_position * AV_TIME_BASE), AV_TIME_BASE_Q, pipe.stream->time_base);

	avcodec_flush_buffers(pipe.codec_ctx.get());
	if (int err = av_seek_frame(pipe.format_ctx.get(), pipe.stream->index, target_ts, AVSEEK_FLAG_BACKWARD)) {
		FFmpeg::print_av_error("audio_decoder: Error while seeking", err);
		return;
	}
	avcodec_flush_buffers(pipe.codec_ctx.get());

	while (true) {
		int r = FFmpeg::get_frame(pipe.format_ctx.get(), pipe.codec_ctx.get(), pipe.stream->index, av_frame.get(),
								  av_packet.get());
		if (r < 0)
			return;

		int64_t frame_pts = av_frame->pts;
		int64_t frame_dur = av_frame->nb_samples;
		if (frame_pts + frame_dur < target_ts) {
			av_frame_unref(av_frame.get());
			continue;
		}

		mixed = (uint32_t)(p_position * mix_rate);

		int max_out = swr_get_out_samples(pipe.swr_ctx.get(), av_frame->nb_samples);
		if (max_out > 0) {
			if ((int)scratch.size() < max_out)
				scratch.resize(max_out);
			uint8_t* out_ptr = (uint8_t*)scratch.data();
			int converted = swr_convert(pipe.swr_ctx.get(), &out_ptr, max_out,
										(const uint8_t**)av_frame->extended_data, av_frame->nb_samples);
			if (converted > 0) {
				size_t skip = 0;
				if (frame_pts < target_ts) {
					int64_t to_skip = target_ts - frame_pts;
					skip = (to_skip < converted) ? (size_t)to_skip : (size_t)converted;
				}
				ring.write(scratch.data() + skip, (size_t)converted - skip);
			}
		}
		av_frame_unref(av_frame.get());
		break;
	}
}

void AudioStreamFFmpeg::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path", "stream_index"), &AudioStreamFFmpeg::open, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("close"), &AudioStreamFFmpeg::close);
	ClassDB::bind_method(D_METHOD("cancel"), &AudioStreamFFmpeg::cancel);

	ClassDB::bind_method(D_METHOD("is_open"), &AudioStreamFFmpeg::is_open);
	ClassDB::bind_method(D_METHOD("is_stream_healthy"), &AudioStreamFFmpeg::is_stream_healthy);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &AudioStreamFFmpeg::get_sample_rate);
	ClassDB::bind_method(D_METHOD("is_stereo"), &AudioStreamFFmpeg::is_stereo);
	ClassDB::bind_method(D_METHOD("get_headers"), &AudioStreamFFmpeg::get_headers);
	ClassDB::bind_method(D_METHOD("get_network_timeout"), &AudioStreamFFmpeg::get_network_timeout);

	ClassDB::bind_method(D_METHOD("set_headers", "headers"), &AudioStreamFFmpeg::set_headers);
	ClassDB::bind_method(D_METHOD("set_network_timeout", "seconds"), &AudioStreamFFmpeg::set_network_timeout);

	ClassDB::add_property(get_class_static(), PropertyInfo(Variant::STRING, "headers"), "set_headers", "get_headers");
}
