#include "ffmpeg.hpp"
#include <cmath>


static bool pipeline_err(const char* msg) {
	UtilityFunctions::printerr("GoZen FFmpeg: ", msg, "!");
	return false;
}

// Polled by libavformat during blocking I/O; returns non-zero to abort the operation with
// AVERROR_EXIT. Fires on either an explicit cancel or an elapsed deadline.
static int ffmpeg_interrupt_cb(void* opaque) {
	InterruptState* s = static_cast<InterruptState*>(opaque);
	if (s->aborted.load(std::memory_order_relaxed))
		return 1;
	int64_t dl = s->deadline_us.load(std::memory_order_relaxed);
	return (dl != 0 && av_gettime_relative() > dl) ? 1 : 0;
}

void install_interrupt_callback(AVFormatContext* ctx, InterruptState* state) {
	ctx->interrupt_callback.callback = ffmpeg_interrupt_cb;
	ctx->interrupt_callback.opaque = state;
}

bool open_format_context(const String& path, const PackedByteArray& bytes, const String& headers,
						 StreamMediaType restrict_to, UniqueAVFormatCtxInput& out_format_ctx,
						 UniqueAVIOContext& out_avio, std::unique_ptr<BufferData>& out_buffer_data,
						 InterruptState* interrupt) {
	out_format_ctx.reset();
	out_avio.reset();
	out_buffer_data.reset();

	AVFormatContext* temp_format_ctx = nullptr;
	AVDictionary* options = nullptr;

	if (path.begins_with("rtsp://"))
		av_dict_set(&options, "rtsp_transport", "tcp", 0);
	if (headers != "")
		av_dict_set(&options, "headers", headers.utf8().get_data(), 0);
	if (restrict_to == StreamMediaType::VIDEO)
		av_dict_set(&options, "allowed_media_types", "video", 0);
	else if (restrict_to == StreamMediaType::AUDIO)
		av_dict_set(&options, "allowed_media_types", "audio", 0);

	const bool is_memory = path.begins_with("res://") || path.begins_with("user://");

	if (is_memory) {
		temp_format_ctx = avformat_alloc_context();
		if (!temp_format_ctx) {
			av_dict_free(&options);
			return pipeline_err("Failed to allocate AVFormatContext");
		}
		if (bytes.is_empty()) {
			av_dict_free(&options);
			avformat_free_context(temp_format_ctx);
			return pipeline_err("Couldn't load file from res:// or user://");
		}

		out_buffer_data = std::make_unique<BufferData>();
		out_buffer_data->ptr = const_cast<uint8_t*>(bytes.ptr()); // read-only use; avoids COW
		out_buffer_data->size = (size_t)bytes.size();
		out_buffer_data->offset = 0;

		unsigned char* avio_ctx_buffer = (unsigned char*)av_malloc(FFmpeg::AVIO_CTX_BUFFER_SIZE);
		out_avio = make_unique_ffmpeg<AVIOContext, AVIOContextDeleter>(
			avio_alloc_context(avio_ctx_buffer, FFmpeg::AVIO_CTX_BUFFER_SIZE, 0, out_buffer_data.get(),
							   &FFmpeg::read_buffer_packet, nullptr, &FFmpeg::seek_buffer));
		if (!out_avio) {
			av_dict_free(&options);
			av_free(avio_ctx_buffer);
			avformat_free_context(temp_format_ctx);
			out_buffer_data.reset();
			return pipeline_err("Failed to create avio_ctx");
		}
		temp_format_ctx->pb = out_avio.get();

		if (interrupt)
			install_interrupt_callback(temp_format_ctx, interrupt);

		if (avformat_open_input(&temp_format_ctx, nullptr, nullptr, nullptr) != 0) {
			av_dict_free(&options);
			out_avio.reset();
			out_buffer_data.reset();
			return pipeline_err("Failed to open input from memory buffer");
		}
	} else {
		if (interrupt) {
			temp_format_ctx = avformat_alloc_context();
			if (!temp_format_ctx) {
				av_dict_free(&options);
				return pipeline_err("Failed to allocate AVFormatContext");
			}
			install_interrupt_callback(temp_format_ctx, interrupt);
		}
		if (avformat_open_input(&temp_format_ctx, path.utf8(), nullptr, &options) != 0) {
			av_dict_free(&options);
			return pipeline_err("Couldn't open file");
		}
	}
	av_dict_free(&options);

	out_format_ctx = make_unique_ffmpeg<AVFormatContext, AVFormatCtxInputDeleter>(temp_format_ctx);
	if (avformat_find_stream_info(out_format_ctx.get(), nullptr) < 0) {
		out_format_ctx.reset();
		out_avio.reset();
		out_buffer_data.reset();
		return pipeline_err("Couldn't find stream info");
	}

	return true;
}

bool open_audio_pipeline(const String& path, const PackedByteArray& bytes, const String& headers,
						 int stream_index, const AVChannelLayout& out_layout, AVSampleFormat out_fmt,
						 int out_sample_rate, bool use_multithreading, AudioDecodePipeline& out,
						 InterruptState* interrupt) {
	out = AudioDecodePipeline{};

	if (!open_format_context(path, bytes, headers, StreamMediaType::AUDIO, out.format_ctx, out.avio_ctx,
							 out.buffer_data, interrupt))
		return false; // open_format_context already logged

	if (stream_index == -1) {
		for (unsigned int i = 0; i < out.format_ctx->nb_streams; i++) {
			if (out.format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				out.stream = out.format_ctx->streams[i];
				break;
			}
		}
	} else if (stream_index >= 0 && stream_index < (int)out.format_ctx->nb_streams) {
		AVStream* s = out.format_ctx->streams[stream_index];
		if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			out.stream = s;
	} else {
		return pipeline_err("Invalid stream index");
	}
	if (!out.stream)
		return pipeline_err("No audio stream found");

	for (unsigned int i = 0; i < out.format_ctx->nb_streams; i++)
		if (out.format_ctx->streams[i] != out.stream)
			out.format_ctx->streams[i]->discard = AVDISCARD_ALL;

	const AVCodec* codec = avcodec_find_decoder(out.stream->codecpar->codec_id);
	if (!codec)
		return pipeline_err("Couldn't find decoder");

	out.codec_ctx = make_unique_ffmpeg<AVCodecContext, AVCodecCtxDeleter>(avcodec_alloc_context3(codec));
	if (!out.codec_ctx)
		return pipeline_err("Couldn't allocate codec context");
	if (avcodec_parameters_to_context(out.codec_ctx.get(), out.stream->codecpar) < 0)
		return pipeline_err("Couldn't initialize codec context");

	out.codec_ctx->request_sample_fmt = out_fmt;
	if (use_multithreading)
		FFmpeg::enable_multithreading(out.codec_ctx.get(), codec); // must precede avcodec_open2
	if (avcodec_open2(out.codec_ctx.get(), codec, nullptr) < 0)
		return pipeline_err("Couldn't open audio codec");

	const int src_rate = out.codec_ctx->sample_rate;
	const int output_rate = (out_sample_rate > 0) ? out_sample_rate : src_rate;

	out.stereo = out.codec_ctx->ch_layout.nb_channels >= 2; // source channel count (for _is_monophonic)
	out.sample_rate = output_rate;
	out.out_channels = out_layout.nb_channels;
	out.bytes_per_sample = av_get_bytes_per_sample(out_fmt);

	SwrContext* temp_swr_ctx = nullptr;
	int r = swr_alloc_set_opts2(&temp_swr_ctx, &out_layout, out_fmt, output_rate, &out.codec_ctx->ch_layout,
								out.codec_ctx->sample_fmt, src_rate, 0, nullptr);
	out.swr_ctx = make_unique_ffmpeg<SwrContext, SwrCtxDeleter>(temp_swr_ctx);
	if (r < 0 || swr_init(out.swr_ctx.get()) < 0)
		return pipeline_err("Failed to initialize SWR");

	return true;
}


int decode_audio_frame(AudioDecodePipeline& pipe, AVFrame* frame, AVPacket* packet, std::vector<uint8_t>& scratch) {
	int r = FFmpeg::get_frame(pipe.format_ctx.get(), pipe.codec_ctx.get(), pipe.stream->index, frame, packet);
	if (r < 0)
		return r; // EOF or read error: stop the loop

	int max_out = swr_get_out_samples(pipe.swr_ctx.get(), frame->nb_samples);
	if (max_out <= 0) {
		av_frame_unref(frame);
		return 0; // decoded, but resampler produced nothing yet
	}

	size_t need = (size_t)max_out * pipe.out_channels * pipe.bytes_per_sample;
	if (scratch.size() < need)
		scratch.resize(need);

	uint8_t* out_ptr = scratch.data();
	int converted = swr_convert(pipe.swr_ctx.get(), &out_ptr, max_out, (const uint8_t**)frame->extended_data,
								frame->nb_samples);
	av_frame_unref(frame);
	if (converted < 0) {
		FFmpeg::print_av_error("Couldn't convert the audio frame!", converted);
		return converted;
	}
	return converted;
}

int flush_audio_resampler(AudioDecodePipeline& pipe, std::vector<uint8_t>& scratch) {
	int max_out = swr_get_out_samples(pipe.swr_ctx.get(), 0);
	if (max_out <= 0)
		return 0;

	size_t need = (size_t)max_out * pipe.out_channels * pipe.bytes_per_sample;
	if (scratch.size() < need)
		scratch.resize(need);

	uint8_t* out_ptr = scratch.data();
	int converted = swr_convert(pipe.swr_ctx.get(), &out_ptr, max_out, nullptr, 0);
	return converted < 0 ? 0 : converted;
}


void FFmpeg::print_av_error(const char* message, int error) {
	char l_error_buffer[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(error, l_error_buffer, sizeof(l_error_buffer));
	UtilityFunctions::printerr(message, l_error_buffer);
}


void FFmpeg::enable_multithreading(AVCodecContext* codec_ctx, const AVCodec* codec) {
	codec_ctx->thread_count = OS::get_singleton()->get_processor_count() - 1;

	if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
		codec_ctx->thread_type = FF_THREAD_FRAME;
	else if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
		codec_ctx->thread_type = FF_THREAD_SLICE;
	else
		codec_ctx->thread_count = 1; // Don't use multithreading
}


int FFmpeg::get_frame(AVFormatContext* format_ctx, AVCodecContext* codec_ctx, int stream_id, AVFrame* frame,
					  AVPacket* packet) {
	int response = 0;
	bool eof = false;

	av_frame_unref(frame);
	while ((response = avcodec_receive_frame(codec_ctx, frame)) == AVERROR(EAGAIN) && !eof) {
		do {
			av_packet_unref(packet);
			response = av_read_frame(format_ctx, packet);
		} while (packet->stream_index != stream_id && response >= 0);

		if (response == AVERROR_EOF) {
			eof = true;
			avcodec_send_packet(codec_ctx, nullptr); // Send null packet to signal end
		} else if (response < 0) {
			if (response != AVERROR_EXIT) // interrupt-callback abort (timeout/cancel): expected, not an error
				UtilityFunctions::printerr("Error reading frame! ", response);
			break;
		} else {
			response = avcodec_send_packet(codec_ctx, packet);
			if (response < 0 && response != AVERROR_INVALIDDATA) {
				UtilityFunctions::printerr("Problem sending package! ", response);
				break;
			}
		}
		av_frame_unref(frame);
	}

	return response;
}


enum AVPixelFormat FFmpeg::get_hw_format(const enum AVPixelFormat* pix_fmt, enum AVPixelFormat* hw_pix_fmt) {
	const enum AVPixelFormat* p;

	for (p = pix_fmt; *p != -1; p++)
		if (*p == *hw_pix_fmt)
			return *p;

	UtilityFunctions::printerr("Failed to get HW surface format!");
	return AV_PIX_FMT_NONE;
}


int FFmpeg::read_buffer_packet(void* opaque, uint8_t* buffer, int buffer_size) {
	BufferData* buffer_data = (BufferData*)opaque;
	size_t remaining = buffer_data->size - buffer_data->offset;

	if (remaining == 0)
		return AVERROR_EOF;

	// Change buffer size if not enough data remaining.
	size_t new_size = (remaining < (size_t)buffer_size ? remaining : (size_t)buffer_size);

	memcpy(buffer, buffer_data->ptr + buffer_data->offset, new_size);
	buffer_data->offset += new_size;
	return (int)new_size;
}


int64_t FFmpeg::seek_buffer(void* opaque, int64_t offset, int where) {
	BufferData* buffer_data = (BufferData*)opaque;
	int64_t new_offset = 0;

	switch (where) {
	case SEEK_SET: // 0
		new_offset = offset;
		break;
	case SEEK_CUR: // 1
		new_offset = buffer_data->offset + offset;
		break;
	case SEEK_END: // 2
		new_offset = buffer_data->size + offset;
		break;
	case AVSEEK_SIZE: // 1
		return buffer_data->size;
	default: // Error
		return -1;
	}

	if (new_offset < 0)
		return -1;

	buffer_data->offset = new_offset;
	return buffer_data->offset;
}


static String md_tag(AVDictionary* meta, const char* key) {
	AVDictionaryEntry* e = av_dict_get(meta, key, nullptr, 0);
	return e ? String(e->value) : String();
}

void populate_media_metadata(AVFormatContext* format_ctx, MediaMetadata& out) {
	out = MediaMetadata{};
	if (!format_ctx)
		return;

	out.format_name = (format_ctx->iformat && format_ctx->iformat->name) ? String(format_ctx->iformat->name)
																		 : String();
	out.duration_us = (format_ctx->duration == AV_NOPTS_VALUE) ? 0 : (int64_t)format_ctx->duration;
	out.bit_rate = (int64_t)format_ctx->bit_rate;

	AVDictionaryEntry* fe = nullptr;
	while ((fe = av_dict_get(format_ctx->metadata, "", fe, AV_DICT_IGNORE_SUFFIX)))
		out.format_tags[String(fe->key)] = String(fe->value);

	out.streams.resize(format_ctx->nb_streams);
	for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
		AVStream* s = format_ctx->streams[i];
		AVCodecParameters* par = s->codecpar;

		StreamInfo si;
		si.index = (int)i;
		si.media_type = par->codec_type;
		si.codec = String(avcodec_get_name(par->codec_id));
		si.title = md_tag(s->metadata, "title");
		si.language = md_tag(s->metadata, "language");

		AVDictionaryEntry* te = nullptr;
		while ((te = av_dict_get(s->metadata, "", te, AV_DICT_IGNORE_SUFFIX)))
			si.tags[String(te->key)] = String(te->value);

		if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
			si.width = par->width;
			si.height = par->height;
			const char* pf = av_get_pix_fmt_name((AVPixelFormat)par->format);
			si.pixel_format = pf ? String(pf) : String();
			double fr = av_q2d(s->avg_frame_rate);
			if (fr <= 0.0)
				fr = av_q2d(s->r_frame_rate);
			si.framerate = (float)fr;
			si.sample_aspect_ratio = (float)av_q2d(par->sample_aspect_ratio);

			AVDictionaryEntry* rot = av_dict_get(s->metadata, "rotate", nullptr, 0);
			si.rotation = rot ? atoi(rot->value) : 0;
			if (si.rotation == 0) {
				for (int j = 0; j < par->nb_coded_side_data; ++j) {
					const AVPacketSideData* sd = &par->coded_side_data[j];
					if (sd->type == AV_PKT_DATA_DISPLAYMATRIX && sd->size == sizeof(int32_t) * 9) {
						double rot_d = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
						if (!std::isnan(rot_d))
							si.rotation = (int)std::round(rot_d);
					}
				}
			}
			const char* cp = av_color_primaries_name(par->color_primaries);
			si.color_primaries = cp ? String(cp) : String();
			out.video_streams.append((int)i);
		} else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
			si.sample_rate = par->sample_rate;
			si.channels = par->ch_layout.nb_channels;
			char buf[128];
			if (av_channel_layout_describe(&par->ch_layout, buf, sizeof(buf)) > 0)
				si.channel_layout = String(buf);
			out.audio_streams.append((int)i);
		} else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			out.subtitle_streams.append((int)i);
		}

		out.streams[i] = si;
	}

	out.chapters.resize(format_ctx->nb_chapters);
	for (unsigned int i = 0; i < format_ctx->nb_chapters; i++) {
		AVChapter* ch = format_ctx->chapters[i];
		ChapterInfo ci;
		ci.start_us = (int64_t)(ch->start * av_q2d(ch->time_base) * 1000000.0);
		ci.end_us = (int64_t)(ch->end * av_q2d(ch->time_base) * 1000000.0);
		AVDictionaryEntry* ce = nullptr;
		while ((ce = av_dict_get(ch->metadata, "", ce, AV_DICT_IGNORE_SUFFIX)))
			ci.tags[String(ce->key)] = String(ce->value);
		out.chapters[i] = ci;
	}

	out.valid = true;
}
