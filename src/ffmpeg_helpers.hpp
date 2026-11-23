#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}


// Closes an input AVFormatContext.
struct AVFormatCtxInputDeleter {
	void operator()(AVFormatContext* ctx) const {
		if (ctx)
			avformat_close_input(&ctx);
	}
};
using UniqueAVFormatCtxInput = std::unique_ptr<AVFormatContext, AVFormatCtxInputDeleter>;


// Closes the file of an output AVFormatContext, if it owns one, and frees the context.
struct AVFormatCtxOutputDeleter {
	void operator()(AVFormatContext* ctx) const {
		if (!ctx)
			return;
		if (ctx->pb && !(ctx->oformat->flags & AVFMT_NOFILE))
			avio_closep(&ctx->pb);
		avformat_free_context(ctx);
	}
};
using UniqueAVFormatCtxOutput = std::unique_ptr<AVFormatContext, AVFormatCtxOutputDeleter>;


// Frees an AVCodecContext.
struct AVCodecCtxDeleter {
	void operator()(AVCodecContext* ctx) const {
		if (ctx)
			avcodec_free_context(&ctx);
	}
};
using UniqueAVCodecCtx = std::unique_ptr<AVCodecContext, AVCodecCtxDeleter>;


// Frees an AVFrame.
struct AVFrameDeleter {
	void operator()(AVFrame* frame) const {
		if (frame)
			av_frame_free(&frame);
	}
};
using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;


// Frees an AVPacket.
struct AVPacketDeleter {
	void operator()(AVPacket* packet) const {
		if (packet)
			av_packet_free(&packet);
	}
};
using UniqueAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;


// Frees an AVIOContext and its buffer.
struct AVIOContextDeleter {
	void operator()(AVIOContext* avio_ctx) const {
		if (!avio_ctx)
			return;
		if (avio_ctx->buffer)
			av_free(avio_ctx->buffer);
		avio_context_free(&avio_ctx);
	}
};
using UniqueAVIOContext = std::unique_ptr<AVIOContext, AVIOContextDeleter>;


// Frees a SwrContext.
struct SwrCtxDeleter {
	void operator()(SwrContext* ctx) const {
		if (ctx)
			swr_free(&ctx);
	}
};
using UniqueSwrCtx = std::unique_ptr<SwrContext, SwrCtxDeleter>;


// Frees a SwsContext.
struct SwsCtxDeleter {
	void operator()(SwsContext* ctx) const {
		if (ctx)
			sws_freeContext(ctx);
	}
};
using UniqueSwsCtx = std::unique_ptr<SwsContext, SwsCtxDeleter>;


// Takes ownership of ptr in a std::unique_ptr with the given deleter.
template <typename T_FFmpeg, typename T_Deleter>
std::unique_ptr<T_FFmpeg, T_Deleter> make_unique_ffmpeg(T_FFmpeg* ptr) {
	return std::unique_ptr<T_FFmpeg, T_Deleter>(ptr);
}


// Allocates an owned AVFrame, null on failure.
inline UniqueAVFrame make_unique_avframe() { return make_unique_ffmpeg<AVFrame, AVFrameDeleter>(av_frame_alloc()); }


// Allocates an owned AVPacket, null on failure.
inline UniqueAVPacket make_unique_avpacket() {
	return make_unique_ffmpeg<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

// Abort and timeout signal of the blocking FFmpeg opens and reads on one AVFormatContext, read by
// the interrupt callback from any thread.
// The context stores only its address in interrupt_callback.opaque: it must outlive the context.
struct InterruptState {
	std::atomic<bool> aborted{false};    // Set by cancel(), permanently.
	std::atomic<int64_t> deadline_us{0}; // av_gettime_relative() cutoff in µs, 0 for none.
};

// Read cursor over an in-memory file, the AVIO opaque of a res:// or user:// source.
struct BufferData {
	uint8_t* ptr;
	size_t size;
	size_t offset;
};

// Opened demux, decode and resample chain of one audio stream, producing interleaved samples in
// a target format. Move-only, owning its FFmpeg contexts.
// Shared by AudioStreamFFmpeg (streaming) and GoZenAudio (batch decode).
struct AudioDecodePipeline {
	UniqueAVFormatCtxInput format_ctx;
	UniqueAVCodecCtx codec_ctx;
	UniqueSwrCtx swr_ctx;
	UniqueAVIOContext avio_ctx;              // Memory sources only.
	std::unique_ptr<BufferData> buffer_data; // Memory sources only. The AVIO opaque points at it across moves.
	AVStream* stream = nullptr;              // Borrowed from format_ctx.
	int bytes_per_sample = 0;                // Output bytes per sample of one channel.
	int sample_rate = 0;                     // Output sample rate in Hz.
	int out_channels = 0;                    // Output channel count.
	bool stereo = true;                      // The source has two or more channels.
	// True when every context and the stream are set.
	bool valid() const { return format_ctx && codec_ctx && swr_ctx && stream; }
};
