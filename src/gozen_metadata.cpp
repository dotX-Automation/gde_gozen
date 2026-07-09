#include "gozen_metadata.hpp"


Error GoZenMetadata::open(const String& path) {
	close();

	PackedByteArray bytes;
	if (path.begins_with("res://") || path.begins_with("user://"))
		bytes = FileAccess::get_file_as_bytes(path);

	UniqueAVFormatCtxInput format_ctx;
	UniqueAVIOContext avio_ctx;
	std::unique_ptr<BufferData> buffer_data;

	arm_deadline(interrupt, network_timeout_us);
	if (!open_format_context(path, bytes, headers, StreamMediaType::ALL, format_ctx, avio_ctx, buffer_data,
							 &interrupt)) {
		_log_err("Couldn't open media for metadata"); // open_format_context already logged the cause
		return FAILED;
	}

	populate_media_metadata(format_ctx.get(), metadata);
	// format_ctx / avio_ctx released here (RAII); the snapshot lives on in `metadata`.
	return metadata.valid ? OK : FAILED;
}

void GoZenMetadata::close() { metadata = MediaMetadata{}; }

bool GoZenMetadata::_valid_stream(int index) const {
	if (!metadata.valid) {
		_log_err("file is not open");
		return false;
	}
	if (index < 0 || index >= (int)metadata.streams.size()) {
		_log_err("invalid stream index");
		return false;
	}
	return true;
}

bool GoZenMetadata::_valid_video(int index) const {
	if (!_valid_stream(index))
		return false;
	if (metadata.streams[index].media_type != AVMEDIA_TYPE_VIDEO) {
		_log_err("stream is not a video stream");
		return false;
	}
	return true;
}

bool GoZenMetadata::_valid_audio(int index) const {
	if (!_valid_stream(index))
		return false;
	if (metadata.streams[index].media_type != AVMEDIA_TYPE_AUDIO) {
		_log_err("stream is not an audio stream");
		return false;
	}
	return true;
}

bool GoZenMetadata::_valid_chapter(int index) const {
	if (!metadata.valid) {
		_log_err("file is not open");
		return false;
	}
	if (index < 0 || index >= (int)metadata.chapters.size()) {
		_log_err("invalid chapter index");
		return false;
	}
	return true;
}

String GoZenMetadata::get_stream_codec(int index) const {
	return _valid_stream(index) ? metadata.streams[index].codec : String();
}
String GoZenMetadata::get_stream_title(int index) const {
	return _valid_stream(index) ? metadata.streams[index].title : String();
}
String GoZenMetadata::get_stream_language(int index) const {
	return _valid_stream(index) ? metadata.streams[index].language : String();
}
Dictionary GoZenMetadata::get_stream_metadata(int index) const {
	return _valid_stream(index) ? metadata.streams[index].tags : Dictionary();
}

Vector2i GoZenMetadata::get_video_resolution(int index) const {
	if (!_valid_video(index))
		return Vector2i();
	return Vector2i(metadata.streams[index].width, metadata.streams[index].height);
}
float GoZenMetadata::get_video_framerate(int index) const {
	return _valid_video(index) ? metadata.streams[index].framerate : 0.f;
}
String GoZenMetadata::get_video_pixel_format(int index) const {
	return _valid_video(index) ? metadata.streams[index].pixel_format : String();
}
int GoZenMetadata::get_video_rotation(int index) const {
	return _valid_video(index) ? metadata.streams[index].rotation : 0;
}
float GoZenMetadata::get_video_sample_aspect_ratio(int index) const {
	return _valid_video(index) ? metadata.streams[index].sample_aspect_ratio : 0.f;
}
String GoZenMetadata::get_video_color_primaries(int index) const {
	return _valid_video(index) ? metadata.streams[index].color_primaries : String();
}

int GoZenMetadata::get_audio_sample_rate(int index) const {
	return _valid_audio(index) ? metadata.streams[index].sample_rate : 0;
}
int GoZenMetadata::get_audio_channels(int index) const {
	return _valid_audio(index) ? metadata.streams[index].channels : 0;
}
String GoZenMetadata::get_audio_channel_layout(int index) const {
	return _valid_audio(index) ? metadata.streams[index].channel_layout : String();
}

int64_t GoZenMetadata::get_chapter_start_us(int chapter_index) const {
	return _valid_chapter(chapter_index) ? metadata.chapters[chapter_index].start_us : 0;
}
int64_t GoZenMetadata::get_chapter_end_us(int chapter_index) const {
	return _valid_chapter(chapter_index) ? metadata.chapters[chapter_index].end_us : 0;
}
Dictionary GoZenMetadata::get_chapter_metadata(int chapter_index) const {
	return _valid_chapter(chapter_index) ? metadata.chapters[chapter_index].tags : Dictionary();
}

void GoZenMetadata::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_headers", "headers"), &GoZenMetadata::set_headers);
	ClassDB::bind_method(D_METHOD("get_headers"), &GoZenMetadata::get_headers);
	ClassDB::add_property(get_class_static(), PropertyInfo(Variant::STRING, "headers"), "set_headers", "get_headers");

	ClassDB::bind_method(D_METHOD("set_network_timeout", "seconds"), &GoZenMetadata::set_network_timeout);
	ClassDB::bind_method(D_METHOD("get_network_timeout"), &GoZenMetadata::get_network_timeout);

	ClassDB::bind_method(D_METHOD("open", "path"), &GoZenMetadata::open);
	ClassDB::bind_method(D_METHOD("close"), &GoZenMetadata::close);
	ClassDB::bind_method(D_METHOD("is_open"), &GoZenMetadata::is_open);

	ClassDB::bind_method(D_METHOD("get_duration_us"), &GoZenMetadata::get_duration_us);
	ClassDB::bind_method(D_METHOD("get_bit_rate"), &GoZenMetadata::get_bit_rate);
	ClassDB::bind_method(D_METHOD("get_format_name"), &GoZenMetadata::get_format_name);
	ClassDB::bind_method(D_METHOD("get_format_tags"), &GoZenMetadata::get_format_tags);

	ClassDB::bind_method(D_METHOD("get_stream_count"), &GoZenMetadata::get_stream_count);
	ClassDB::bind_method(D_METHOD("get_video_streams"), &GoZenMetadata::get_video_streams);
	ClassDB::bind_method(D_METHOD("get_audio_streams"), &GoZenMetadata::get_audio_streams);
	ClassDB::bind_method(D_METHOD("get_subtitle_streams"), &GoZenMetadata::get_subtitle_streams);

	ClassDB::bind_method(D_METHOD("get_stream_codec", "index"), &GoZenMetadata::get_stream_codec);
	ClassDB::bind_method(D_METHOD("get_stream_title", "index"), &GoZenMetadata::get_stream_title);
	ClassDB::bind_method(D_METHOD("get_stream_language", "index"), &GoZenMetadata::get_stream_language);
	ClassDB::bind_method(D_METHOD("get_stream_metadata", "index"), &GoZenMetadata::get_stream_metadata);

	ClassDB::bind_method(D_METHOD("get_video_resolution", "index"), &GoZenMetadata::get_video_resolution);
	ClassDB::bind_method(D_METHOD("get_video_framerate", "index"), &GoZenMetadata::get_video_framerate);
	ClassDB::bind_method(D_METHOD("get_video_pixel_format", "index"), &GoZenMetadata::get_video_pixel_format);
	ClassDB::bind_method(D_METHOD("get_video_rotation", "index"), &GoZenMetadata::get_video_rotation);
	ClassDB::bind_method(D_METHOD("get_video_sample_aspect_ratio", "index"),
						 &GoZenMetadata::get_video_sample_aspect_ratio);
	ClassDB::bind_method(D_METHOD("get_video_color_primaries", "index"), &GoZenMetadata::get_video_color_primaries);

	ClassDB::bind_method(D_METHOD("get_audio_sample_rate", "index"), &GoZenMetadata::get_audio_sample_rate);
	ClassDB::bind_method(D_METHOD("get_audio_channels", "index"), &GoZenMetadata::get_audio_channels);
	ClassDB::bind_method(D_METHOD("get_audio_channel_layout", "index"), &GoZenMetadata::get_audio_channel_layout);

	ClassDB::bind_method(D_METHOD("get_chapter_count"), &GoZenMetadata::get_chapter_count);
	ClassDB::bind_method(D_METHOD("get_chapter_start_us", "chapter_index"), &GoZenMetadata::get_chapter_start_us);
	ClassDB::bind_method(D_METHOD("get_chapter_end_us", "chapter_index"), &GoZenMetadata::get_chapter_end_us);
	ClassDB::bind_method(D_METHOD("get_chapter_metadata", "chapter_index"), &GoZenMetadata::get_chapter_metadata);
}
