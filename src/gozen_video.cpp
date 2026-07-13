#include "gozen_video.hpp"


Error GoZenVideo::open(const String& video_path) {
	if (loaded)
		return _log_err("Already open");

	interrupt.aborted.store(false, std::memory_order_relaxed);

	path = video_path;
	src_resolution = Vector2i(0, 0);

	// For res:// / user:// sources, read the whole file into file_buffer and feed FFmpeg through a
	// custom AVIO. file_buffer MUST stay alive for the whole open (AVIO reads from it lazily).
	if (path.begins_with("res://") || path.begins_with("user://")) {
		file_buffer = FileAccess::get_file_as_bytes(path);
		if (file_buffer.is_empty()) {
			close();
			return _log_err("Couldn't load file from path '" + path + "'");
		}
	}

	arm_deadline(interrupt, network_timeout_us);
	if (!open_format_context(path, file_buffer, headers, StreamMediaType::VIDEO, av_format_ctx, avio_ctx,
							 buffer_data, &interrupt)) {
		close();
		return _log_err("Couldn't open video");
	}

	// Single pass: select the first decodable, non-cover video stream; discard every other stream.
	av_stream = nullptr;

	for (unsigned int i = 0; i < av_format_ctx->nb_streams; i++) {
		AVCodecParameters* av_codec_params = av_format_ctx->streams[i]->codecpar;

		if (!av_stream && av_codec_params->codec_type == AVMEDIA_TYPE_VIDEO &&
			!(av_format_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
			avcodec_find_decoder(av_codec_params->codec_id)) {
			av_stream = av_format_ctx->streams[i];
			src_resolution.x = av_codec_params->width;
			src_resolution.y = av_codec_params->height;
			color_profile = av_codec_params->color_primaries;

			AVDictionaryEntry* rotate_tag = av_dict_get(av_stream->metadata, "rotate", nullptr, 0);
			rotation = rotate_tag ? atoi(rotate_tag->value) : 0;

			if (rotation == 0) { // Modern rotation detection via display-matrix side data.
				for (int j = 0; j < av_stream->codecpar->nb_coded_side_data; ++j) {
					const AVPacketSideData* side_data = &av_stream->codecpar->coded_side_data[j];
					if (side_data->type == AV_PKT_DATA_DISPLAYMATRIX &&
						side_data->size == sizeof(int32_t) * 9) {
						double rot_d = av_display_rotation_get(reinterpret_cast<const int32_t*>(side_data->data));
						if (!std::isnan(rot_d))
							rotation = (int8_t)std::round(rot_d);
					}
				}
			}
			continue; // keep the selected video stream
		}

		av_format_ctx->streams[i]->discard = AVDISCARD_ALL; // discard every non-selected stream
	}

	if (!av_stream) {
		close();
		return _log_err("Couldn't find a decodable video stream");
	}

	// Timebase + start time. stream_time_base_video MUST be set before start_time_video uses it.
	stream_time_base_video = av_q2d(av_stream->time_base) * 1000.0 * 10000.0; // stream units -> 100ns ticks

	if (av_stream->start_time != AV_NOPTS_VALUE)
		start_time_video = (int64_t)(av_stream->start_time * stream_time_base_video);
	else
		start_time_video = 0;

	// Setup decoder codec context.
	const AVCodec* av_codec = nullptr;

	// VP8/VP9 with alpha need libvpx.
	if (av_stream->codecpar->codec_id == AV_CODEC_ID_VP9 || av_stream->codecpar->codec_id == AV_CODEC_ID_VP8) {
		AVDictionaryEntry* alpha_entry = av_dict_get(av_stream->metadata, "alpha_mode", nullptr, 0);
		if (alpha_entry && String(alpha_entry->value).strip_edges() == "1") {
			const char* libvpx_name = (av_stream->codecpar->codec_id == AV_CODEC_ID_VP9) ? "libvpx-vp9" : "libvpx";
			av_codec = avcodec_find_decoder_by_name(libvpx_name);
			if (!av_codec)
				_log_err(String("Detected alpha_mode but '") + libvpx_name + "' decoder is missing!");
		}
	}
	if (!av_codec)
		av_codec = avcodec_find_decoder(av_stream->codecpar->codec_id);
	if (!av_codec) {
		close();
		return _log_err("Couldn't find decoder");
	}

	av_codec_ctx = make_unique_ffmpeg<AVCodecContext, AVCodecCtxDeleter>(avcodec_alloc_context3(av_codec));
	if (!av_codec_ctx) {
		close();
		return _log_err("Failed alloc codec");
	}
	if (avcodec_parameters_to_context(av_codec_ctx.get(), av_stream->codecpar)) {
		close();
		return _log_err("Couldn't init codec");
	}

	FFmpeg::enable_multithreading(av_codec_ctx.get(), av_codec);

	if (avcodec_open2(av_codec_ctx.get(), av_codec, nullptr)) {
		close();
		return _log_err("Couldn't open codec");
	}

	// resolution = decoded/render size (what Godot renders); src_resolution = SAR-adjusted display size.
	sar = av_q2d(av_stream->codecpar->sample_aspect_ratio);
	resolution = src_resolution;

	if (sar > 1.0)
		src_resolution.x *= sar;
	else if (sar != 0.0 && sar != 1.0)
		src_resolution.x /= sar;

	// Allocate working frames and decode the first frame to learn the real pixel format.
	av_packet = make_unique_avpacket();
	av_frame = make_unique_avframe();
	av_sws_frame = make_unique_avframe();

	if (!av_packet || !av_frame || !av_sws_frame) {
		close();
		return _log_err("Couldn't alloc packet/frames");
	}

	avcodec_flush_buffers(av_codec_ctx.get());
	bool duration_from_bitrate = av_format_ctx->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE;
	int response = 0;
	int attempts = 0;

	arm_deadline(interrupt, network_timeout_us); // cover the first-frame reads too
	while (true) {
		response = FFmpeg::get_frame(av_format_ctx.get(), av_codec_ctx.get(), av_stream->index, av_frame.get(),
									 av_packet.get());

		if (response == 0)
			break;
		else if (response == AVERROR(EAGAIN) || response == AVERROR(EWOULDBLOCK)) {
			if (attempts > 10) {
				FFmpeg::print_av_error("Reached max attempts trying to get first frame!", response);
				close();
				return _log_err("Failed to decode first frame (max attempts)");
			}
			attempts++;
		} else if (response == AVERROR_EOF) {
			FFmpeg::print_av_error("Reached EOF trying to get first frame!", response);
			close();
			return _log_err("Failed to decode first frame (EOF)");
		} else {
			FFmpeg::print_av_error("Something went wrong getting first frame!", response);
			close();
			return _log_err("Failed to decode first frame (Invalid Data)");
		}
	}

	// Interlacing.
	if (av_frame->flags & AV_FRAME_FLAG_INTERLACED)
		interlaced = av_frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST ? 1 : 2;

	// Color range.
	full_color_range = av_frame->color_range == AVCOL_RANGE_JPEG;

	// Framerate: average -> real -> guessed.
	if (av_stream->avg_frame_rate.num > 0 && av_stream->avg_frame_rate.den > 0) {
		double avg_rate = av_q2d(av_stream->avg_frame_rate);
		if (avg_rate > 0.1)
			framerate = avg_rate;
	}
	if (framerate <= 0.1 && av_stream->r_frame_rate.num > 0 && av_stream->r_frame_rate.den > 0) {
		double r_rate = av_q2d(av_stream->r_frame_rate);
		if (r_rate > 0.1)
			framerate = r_rate;
	}
	if (framerate <= 0.1) {
		AVRational guessed_rate_q = av_guess_frame_rate(av_format_ctx.get(), av_stream, av_frame.get());
		double guessed_rate = av_q2d(guessed_rate_q);
		if (guessed_rate > 0.1)
			framerate = guessed_rate;
	}
	// Fix for WMV/ASF reporting a false 1000 FPS.
	if (std::abs(framerate - 1000.0) < 0.001) {
		if (av_stream->r_frame_rate.num > 0 && av_stream->r_frame_rate.den > 0) {
			double r_rate = av_q2d(av_stream->r_frame_rate);
			if (r_rate > 0.1 && r_rate < 990.0)
				framerate = r_rate;
		}
	}
	if (framerate <= 0) {
		_log("Framerate could not be determined, defaulting to 24 FPS");
		framerate = 24.0; // Mainly for image formats.
	}

	average_frame_duration = 10000000.0 / framerate; // e.g. 1s / 25fps = 400000 ticks (40ms).

	// Alpha layer detection.
	const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)av_frame->format);
	alpha_layer = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
	if (av_frame->format == AV_PIX_FMT_PAL8) // For GIFs.
		alpha_layer = true;

	pixel_format = av_get_pix_fmt_name((AVPixelFormat)av_frame->format);
	_log(String("Selected pixel format is: ") + pixel_format);

	int uv_width = (resolution.x + 1) / 2;
	int uv_height = (resolution.y + 1) / 2;
	bool is_natively_supported = (av_frame->format == AV_PIX_FMT_YUV420P || av_frame->format == AV_PIX_FMT_YUVJ420P ||
								  av_frame->format == AV_PIX_FMT_YUVA420P);

	if (is_natively_supported) {
		padding = av_frame->linesize[0] - resolution.x;
		y_data = Image::create_empty(resolution.x, resolution.y, false, Image::FORMAT_R8);
		u_data = Image::create_empty(uv_width, uv_height, false, Image::FORMAT_R8);
		v_data = Image::create_empty(uv_width, uv_height, false, Image::FORMAT_R8);
		if (alpha_layer)
			a_data = Image::create_empty(resolution.x, resolution.y, false, Image::FORMAT_R8);
	} else {
		AVPixelFormat new_format = alpha_layer ? AV_PIX_FMT_YUVA420P : AV_PIX_FMT_YUV420P;

		using_sws = true;
		sws_ctx = make_unique_ffmpeg<SwsContext, SwsCtxDeleter>(
			sws_getContext(resolution.x, resolution.y, (AVPixelFormat)av_frame->format,
						   resolution.x, resolution.y, new_format, sws_flag, nullptr, nullptr, nullptr));
		if (!sws_ctx) {
			close();
			return _log_err("Failed to create sws context");
		}

		if (sws_scale_frame(sws_ctx.get(), av_sws_frame.get(), av_frame.get()) < 0) {
			close();
			return _log_err("Failed to scale first frame");
		}

		padding = av_sws_frame->linesize[0] - resolution.x;
		y_data = Image::create_empty(resolution.x, resolution.y, false, Image::FORMAT_R8);
		u_data = Image::create_empty(uv_width, uv_height, false, Image::FORMAT_R8);
		v_data = Image::create_empty(uv_width, uv_height, false, Image::FORMAT_R8);
		if (alpha_layer)
			a_data = Image::create_empty(resolution.x, resolution.y, false, Image::FORMAT_R8);

		av_frame_unref(av_sws_frame.get());
	}

	duration = av_format_ctx->duration;
	if (av_stream->duration == AV_NOPTS_VALUE || duration_from_bitrate) {
		if (duration == AV_NOPTS_VALUE || duration_from_bitrate) {
			_log("Invalid video duration, assuming continuous stream (Common for GIF/WebP)");
			duration = 0; // Rely on EOF instead of frame_count (for images).
		} else {
			AVRational temp_rational = AVRational{1, AV_TIME_BASE};
			if (temp_rational.num != av_stream->time_base.num || temp_rational.den != av_stream->time_base.den)
				duration =
					std::ceil(static_cast<double>(duration) * av_q2d(temp_rational) / av_q2d(av_stream->time_base));
		}
		av_stream->duration = duration;
	}

	if (av_stream->nb_frames > 0)
		frame_count = av_stream->nb_frames;
	else
		frame_count = static_cast<int64_t>(
			std::round((static_cast<double>(duration) / static_cast<double>(AV_TIME_BASE)) * framerate));

	if (av_packet)
		av_packet_unref(av_packet.get());
	if (av_frame)
		av_frame_unref(av_frame.get());

	loaded = true;
	seek_frame(0);

	return OK;
}

void GoZenVideo::close() {
	_log("Closing video file on path: " + path);

	loaded = false;
	current_frame = -1;

	av_packet.reset();
	av_frame.reset();
	av_sws_frame.reset();
	sws_ctx.reset();

	if (av_codec_ctx) {
		avcodec_flush_buffers(av_codec_ctx.get());
	}

	av_codec_ctx.reset();
	av_format_ctx.reset();
	av_stream = nullptr; // borrowed from format_ctx; reset so a re-open re-selects cleanly

	avio_ctx.reset();
	buffer_data.reset(); // reset AFTER avio_ctx (AVIO opaque pointed at it)
	file_buffer.clear();

	using_sws = false; // re-open may use a natively-supported format
	alpha_layer = false;
}

Error GoZenVideo::seek_frame(int frame_nr) {
	if (!loaded)
		return _log_err("Video is not open");

	arm_deadline(interrupt, network_timeout_us);

	int response = 0;
	int attempts = 0;
	int eof_attempts = 0;

	// Video seeking.
	if ((response = _seek_frame(frame_nr)) < 0)
		return _log_err("Couldn't seek video");

	while (true) {
		if ((response = FFmpeg::get_frame(av_format_ctx.get(), av_codec_ctx.get(), av_stream->index, av_frame.get(),
										  av_packet.get()))) {
			if (response == AVERROR(EAGAIN) || response == AVERROR(EWOULDBLOCK)) {
				if (attempts > 10) {
					FFmpeg::print_av_error("Reached max attempts trying to get first frame!", response);
					break;
				}

				attempts++;
				continue;
			} else if (response == AVERROR_EOF) {
				if (eof_attempts > 10) {
					FFmpeg::print_av_error("Reached max attempts recovering from EOF in seek_frame!", response);
					break;
				}

				_log_err("End of file reached! Going back 1 frame!");

				eof_attempts++;
				if (frame_nr > 0)
					frame_nr--;

				if ((response = _seek_frame(frame_nr)) < 0)
					return _log_err("Couldn't seek");

				continue;
			}

			FFmpeg::print_av_error("Problem happened getting frame in seek_frame! ", response);
			response = 1;
			break;
		}

		// Get frame pts.
		if (av_frame->best_effort_timestamp == AV_NOPTS_VALUE)
			current_pts = av_frame->pts;
		else
			current_pts = av_frame->best_effort_timestamp;

		if (current_pts == AV_NOPTS_VALUE)
			continue;

		// Skip to actual requested frame.
		if ((int64_t)(current_pts * stream_time_base_video) / 10000 >= frame_timestamp / 10000) {
			_copy_frame_data();
			break;
		}
	}

	current_frame = frame_nr;

	av_frame_unref(av_frame.get());
	av_packet_unref(av_packet.get());

	return OK;
}

bool GoZenVideo::next_frame(bool skip) {
	if (!loaded)
		return false;

	arm_deadline(interrupt, network_timeout_us);

	int response = FFmpeg::get_frame(av_format_ctx.get(), av_codec_ctx.get(), av_stream->index, av_frame.get(),
									 av_packet.get());
	bool got_frame = (response == 0);

	if (got_frame) {
		if (!skip)
			_copy_frame_data();
		current_frame++;
	}

	av_frame_unref(av_frame.get());
	av_packet_unref(av_packet.get());

	return got_frame;
}

void GoZenVideo::_copy_frame_data() {
	if (av_frame->data[0] == nullptr) {
		_log("Frame is empty");
		return;
	}

	int uv_width = (resolution.x + 1) / 2;
	int uv_height = (resolution.y + 1) / 2;

	AVFrame* src = av_frame.get();
	if (using_sws) {
		if (sws_scale_frame(sws_ctx.get(), av_sws_frame.get(), av_frame.get()) < 0) {
			_log_err("Failed to scale frame");
			return;
		}
		src = av_sws_frame.get();
	}

	uint8_t* y = y_data->ptrw();
	for (int i = 0; i < resolution.y; i++)
		memcpy(y + i * resolution.x, src->data[0] + i * src->linesize[0], resolution.x);

	uint8_t* u = u_data->ptrw();
	uint8_t* v = v_data->ptrw();
	for (int i = 0; i < uv_height; i++) {
		memcpy(u + i * uv_width, src->data[1] + i * src->linesize[1], uv_width);
		memcpy(v + i * uv_width, src->data[2] + i * src->linesize[2], uv_width);
	}

	if (alpha_layer) {
		uint8_t* a = a_data->ptrw();
		for (int i = 0; i < resolution.y; i++)
			memcpy(a + i * resolution.x, src->data[3] + i * src->linesize[3], resolution.x);
	}

	if (using_sws)
		av_frame_unref(av_sws_frame.get());
}

int GoZenVideo::_seek_frame(int frame_nr) {
	avcodec_flush_buffers(av_codec_ctx.get());

	frame_timestamp = (int64_t)(frame_nr * average_frame_duration);
	return av_seek_frame(av_format_ctx.get(), -1, (start_time_video + frame_timestamp) / 10, AVSEEK_FLAG_BACKWARD);
}

void GoZenVideo::_bind_methods() {
	// Methods
	ClassDB::bind_method(D_METHOD("open", "video_path"), &GoZenVideo::open);
	ClassDB::bind_method(D_METHOD("close"), &GoZenVideo::close);

	ClassDB::bind_method(D_METHOD("seek_frame", "frame_nr"), &GoZenVideo::seek_frame);
	ClassDB::bind_method(D_METHOD("next_frame", "skip"), &GoZenVideo::next_frame);

	ClassDB::bind_method(D_METHOD("cancel"), &GoZenVideo::cancel);

	ClassDB::bind_method(D_METHOD("enable_debug"), &GoZenVideo::enable_debug);
	ClassDB::bind_method(D_METHOD("disable_debug"), &GoZenVideo::disable_debug);

	// Getters
	ClassDB::bind_method(D_METHOD("is_open"), &GoZenVideo::is_open);
	ClassDB::bind_method(D_METHOD("get_path"), &GoZenVideo::get_path);

	ClassDB::bind_method(D_METHOD("get_y_data"), &GoZenVideo::get_y_data);
	ClassDB::bind_method(D_METHOD("get_u_data"), &GoZenVideo::get_u_data);
	ClassDB::bind_method(D_METHOD("get_v_data"), &GoZenVideo::get_v_data);
	ClassDB::bind_method(D_METHOD("get_a_data"), &GoZenVideo::get_a_data);

	ClassDB::bind_method(D_METHOD("get_resolution"), &GoZenVideo::get_resolution);
	ClassDB::bind_method(D_METHOD("get_width"), &GoZenVideo::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &GoZenVideo::get_height);
	ClassDB::bind_method(D_METHOD("get_padding"), &GoZenVideo::get_padding);
	ClassDB::bind_method(D_METHOD("get_rotation"), &GoZenVideo::get_rotation);
	ClassDB::bind_method(D_METHOD("get_interlaced"), &GoZenVideo::get_interlaced);
	ClassDB::bind_method(D_METHOD("get_aspect_ratio"), &GoZenVideo::get_aspect_ratio);
	ClassDB::bind_method(D_METHOD("get_pixel_format"), &GoZenVideo::get_pixel_format);
	ClassDB::bind_method(D_METHOD("get_color_profile"), &GoZenVideo::get_color_profile);
	ClassDB::bind_method(D_METHOD("has_alpha"), &GoZenVideo::has_alpha);
	ClassDB::bind_method(D_METHOD("is_full_color_range"), &GoZenVideo::is_full_color_range);
	ClassDB::bind_method(D_METHOD("is_using_sws"), &GoZenVideo::is_using_sws);

	ClassDB::bind_method(D_METHOD("get_duration_us"), &GoZenVideo::get_duration_us);
	ClassDB::bind_method(D_METHOD("get_frame_count"), &GoZenVideo::get_frame_count);
	ClassDB::bind_method(D_METHOD("get_current_frame"), &GoZenVideo::get_current_frame);
	ClassDB::bind_method(D_METHOD("get_framerate"), &GoZenVideo::get_framerate);

	ClassDB::bind_method(D_METHOD("get_headers"), &GoZenVideo::get_headers);
	ClassDB::bind_method(D_METHOD("get_debug_enabled"), &GoZenVideo::get_debug_enabled);

	// Setters
	ClassDB::bind_method(D_METHOD("set_headers", "headers"), &GoZenVideo::set_headers);

	ClassDB::bind_method(D_METHOD("set_network_timeout", "seconds"), &GoZenVideo::set_network_timeout);
	ClassDB::bind_method(D_METHOD("get_network_timeout"), &GoZenVideo::get_network_timeout);

	ClassDB::bind_method(D_METHOD("set_sws_flag_bilinear"), &GoZenVideo::set_sws_flag_bilinear);
	ClassDB::bind_method(D_METHOD("set_sws_flag_bicubic"), &GoZenVideo::set_sws_flag_bicubic);

	// Propeties
	ClassDB::add_property(get_class_static(), PropertyInfo(Variant::STRING, "headers"), "set_headers", "get_headers");
	ClassDB::add_property(get_class_static(), PropertyInfo(Variant::FLOAT, "network_timeout"),
						 "set_network_timeout", "get_network_timeout");
}
