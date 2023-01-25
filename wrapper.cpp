#include "wrapper.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavdevice/avdevice.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
}
#include <thread>

#define NUM_VIDEO_FRAME_BUFFERS 20
#define AUDIO_BUFFER_SIZE 60000


#define MIN_AVAILABLE_AUDIO 2000
#define MIN_AVAILABLE_VIDEO 1

struct CircBuf
{
	void* data;
	int elementSize;
	int numElements;
	int curWrite;
	int curRead;
};

static void _CircBuf_Init(CircBuf* buf, int numElements, int elementSize)
{
	buf->data = malloc(numElements * elementSize);
	buf->elementSize = elementSize;
	buf->numElements = numElements;
	buf->curRead = 0;
	buf->curWrite = 0;
}
static void _CircBuf_Free(CircBuf* buf)
{
	if(buf->data) free(buf->data);
	buf->data = nullptr;
	buf->curRead = 0;
	buf->curWrite = 0;
	buf->elementSize = 0;
	buf->numElements = 0;
}
static void _Circbuf_Read(CircBuf* buf, void* data, int numRead, int* outRead)
{
	int dist = (buf->curWrite - buf->curRead);
	if (dist == 0) {
		if (outRead) *outRead = 0;
		return;
	}

	if (dist < 0) {
		dist = buf->numElements - buf->curRead + buf->curWrite;

		const int numToRead = numRead > dist ? dist : numRead;
		if (outRead) *outRead = numToRead;

		int distRead = buf->numElements - (buf->curRead + numToRead);
		if (distRead < 0)
		{
			const int destination = (buf->curRead + numToRead);
			const int remainingRead = destination - buf->numElements;
			const int toEnd = buf->numElements - buf->curRead;
			memcpy(data, (char*)buf->data + buf->curRead * buf->elementSize, toEnd * buf->elementSize);

			memcpy((char*)data + toEnd * buf->elementSize, (char*)buf->data, remainingRead * buf->elementSize);
			
			buf->curRead = remainingRead;
		}
		else
		{
			memcpy(data, (char*)buf->data + buf->curRead * buf->elementSize, numToRead * buf->elementSize);
			buf->curRead = buf->curRead + numToRead;
		}
	}
	else
	{
		const int numToRead = numRead > dist ? dist : numRead;
		if (outRead) *outRead = numToRead;

		memcpy(data, (char*)buf->data + buf->curRead * buf->elementSize, numToRead * buf->elementSize);
		buf->curRead = buf->curRead + numToRead;
	}
}
static bool _Circbuf_Write(CircBuf* buf, const void* data, int numWrite)
{
	int dist = (buf->curRead - buf->curWrite);

	if (dist <= 0)
	{
		dist = buf->numElements - buf->curWrite + buf->curRead;
		if ((dist-1) < numWrite) return false;


		int distWrite = buf->numElements - (buf->curWrite + numWrite);
		if (distWrite < 0)
		{
			const int destination = (buf->curWrite + numWrite);
			const int remainingWrite = destination - buf->numElements;
			const int toEnd = buf->numElements - buf->curWrite;
			memcpy((char*)buf->data + buf->curWrite * buf->elementSize, data, toEnd * buf->elementSize);

			memcpy((char*)buf->data, (const char*)data + toEnd * buf->elementSize, remainingWrite * buf->elementSize);
			buf->curWrite = remainingWrite;
		}
		else
		{
			memcpy((char*)buf->data + buf->curWrite * buf->elementSize, data, numWrite * buf->elementSize);
			buf->curWrite = buf->curWrite + numWrite;
		}
	}
	else
	{
		if ((dist - 1) < numWrite) return false;

		memcpy((char*)buf->data + buf->curWrite * buf->elementSize, data, numWrite * buf->elementSize);

		buf->curWrite += numWrite;
	}
	return true;
}
static int _Circbuf_GetNumAvailableElements(CircBuf* buf)
{
	int dist = (buf->curRead - buf->curWrite);

	if (dist <= 0)
	{
		dist = buf->numElements - buf->curWrite + buf->curRead;
	}
	return dist;
}
static int _Circbuf_Size(CircBuf* buf)
{
	return buf->numElements - _Circbuf_GetNumAvailableElements(buf);
}
static void* _Circbuf_GetIndex(CircBuf* buf, int idx)
{
	if(idx < buf->numElements) return (char*)buf->data + buf->elementSize * idx;
	return nullptr;
}



struct MediaContext
{
	AVFormatContext* fmt_ctx;

	AVStream* video_stream;
	AVStream* audio_stream;

	AVCodec* video_decoder;
	AVCodec* audio_decoder;

	AVFrame* frame;
	SwsContext* sws_scaler_ctx;

	AVPacket packet;
	CircBuf video_buf;
	CircBuf audio_buf;

	int64_t* video_pts_buf;
	int64_t pts_timer;
	int64_t audio_write_pts;

	std::thread* thread;
	bool thread_should_close;
};

static AVPixelFormat correct_for_deprecated_pixel_format(AVPixelFormat pix_fmt) {
	// Fix swscaler deprecated pixel format warning
	// (YUVJ has been deprecated, change pixel format to regular YUV)
	switch (pix_fmt) {
	case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
	case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
	case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
	case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
	default:                  return pix_fmt;
	}
}

static bool _IsContextFillable(MediaContext* ctx)
{
	
	if (ctx->video_stream && ctx->audio_stream)
	{
		const int availVideo = _Circbuf_GetNumAvailableElements(&ctx->video_buf);
		const int availAudio = _Circbuf_GetNumAvailableElements(&ctx->audio_buf);
		return availVideo > MIN_AVAILABLE_VIDEO && availAudio > MIN_AVAILABLE_AUDIO;
	}
	else if (ctx->video_stream)
	{
		const int availVideo = _Circbuf_GetNumAvailableElements(&ctx->video_buf);
		return availVideo > MIN_AVAILABLE_VIDEO;
	}
	else if (ctx->audio_stream)
	{
		const int availAudio = _Circbuf_GetNumAvailableElements(&ctx->audio_buf);
		return availAudio > MIN_AVAILABLE_AUDIO;
	}
	return false;
}

static void _DecodeRoutine(MediaContext* ctx)
{
	while (!ctx->thread_should_close)
	{
		if (_IsContextFillable(ctx))
		{
			int frame_finished = -1;
			bool isAudio = false;
			while (av_read_frame(ctx->fmt_ctx, &ctx->packet) >= 0)
			{
				AVCodecContext* codec_ctx = nullptr;
				if (ctx->packet.stream_index == ctx->video_stream->index)
				{
					codec_ctx = ctx->video_stream->codec;
				}
				else if (ctx->packet.stream_index == ctx->audio_stream->index)
				{
					isAudio = true;
					codec_ctx = ctx->audio_stream->codec;
				}
				else
				{
					av_packet_unref(&ctx->packet);
					continue;
				}

				frame_finished = avcodec_send_packet(codec_ctx, &ctx->packet);
				if (frame_finished < 0)
				{
					break;
				}

				
				frame_finished = avcodec_receive_frame(codec_ctx, ctx->frame);
				if (frame_finished == AVERROR(EAGAIN) || frame_finished == AVERROR_EOF) {
					av_packet_unref(&ctx->packet);
					continue;
				}
				else if (frame_finished < 0) {
					av_packet_unref(&ctx->packet);
					break;
				}
				
				
				av_packet_unref(&ctx->packet);
				break;
			}



			if (frame_finished != -1)
			{
				if (isAudio)
				{
					// convert to custom audio format maybe?
					// swr_convert_frame();
					_Circbuf_Write(&ctx->audio_buf, ctx->frame->data[0], ctx->frame->nb_samples);
				}
				else
				{
					uint8_t* dest[4] = { (uint8_t*)_Circbuf_GetIndex(&ctx->video_buf, ctx->video_buf.curWrite), NULL, NULL, NULL };
					ctx->video_pts_buf[ctx->video_buf.curWrite] = ctx->frame->pts;
					auto oldWrite = ctx->video_buf.curWrite;
					ctx->video_buf.curWrite = (ctx->video_buf.curWrite + 1) % ctx->video_buf.numElements;
					
					int dest_linesize[4] = { ctx->frame->width * 4, 0, 0, 0 };
					sws_scale(ctx->sws_scaler_ctx, ctx->frame->data, ctx->frame->linesize, 0, ctx->frame->height, dest, dest_linesize);
				}

			}
			else
			{
				ctx->thread_should_close = true;
			}
		}
		else
		{
			_sleep(1);
		}
	}
}

MediaContext* Med_CreateContext(const char* file)
{
	MediaContext* ctx = (MediaContext*)malloc(sizeof(MediaContext));
	if (!ctx) return nullptr;

	memset(ctx, 0, sizeof(MediaContext));
	ctx->fmt_ctx = avformat_alloc_context();
	if (avformat_open_input(&ctx->fmt_ctx, file, NULL, NULL) < 0)
	{
		Med_FreeContext(ctx);
		return nullptr;
	}

	if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0)
	{
		Med_FreeContext(ctx);
		return nullptr;
	}
	int audioStreamIdx = -1;
	int videoStreamIdx = -1;
	for (int i = 0; i < ctx->fmt_ctx->nb_streams; i++)
	{
		AVMediaType type = ctx->fmt_ctx->streams[i]->codec->codec_type;
		if (type == AVMEDIA_TYPE_AUDIO)
		{
			audioStreamIdx = i;
		}
		else if (type == AVMEDIA_TYPE_VIDEO)
		{
			videoStreamIdx = i;
		}
		if (audioStreamIdx != -1 && videoStreamIdx != -1)
			break;
	}

	if (audioStreamIdx == -1 && videoStreamIdx == -1)
	{
		Med_FreeContext(ctx);
		return nullptr;
	}
	if (videoStreamIdx != -1)
	{
		ctx->video_stream = ctx->fmt_ctx->streams[videoStreamIdx];
		ctx->video_decoder = avcodec_find_decoder(ctx->video_stream->codec->codec_id);
	}
	if (audioStreamIdx != -1)
	{
		ctx->audio_stream = ctx->fmt_ctx->streams[audioStreamIdx];
		ctx->audio_decoder = avcodec_find_decoder(ctx->audio_stream->codec->codec_id);
	}
	
	ctx->frame = av_frame_alloc();
	if (ctx->video_decoder)
	{
		if (avcodec_open2(ctx->video_stream->codec, ctx->video_decoder, NULL) < 0)
		{
			Med_FreeContext(ctx);
			return nullptr;
		}
		_CircBuf_Init(&ctx->video_buf, NUM_VIDEO_FRAME_BUFFERS, ctx->video_stream->codec->width * ctx->video_stream->codec->height * 4);

		auto source_pix_fmt = correct_for_deprecated_pixel_format(ctx->video_stream->codec->pix_fmt);
		ctx->sws_scaler_ctx = sws_getContext(ctx->video_stream->codec->width, ctx->video_stream->codec->height, source_pix_fmt,
			ctx->video_stream->codec->width, ctx->video_stream->codec->height, AV_PIX_FMT_BGR0,
			SWS_BILINEAR, NULL, NULL, NULL);
		ctx->video_pts_buf = (int64_t*)malloc(sizeof(int64_t) * NUM_VIDEO_FRAME_BUFFERS);
		if (!ctx->video_pts_buf) {
			Med_FreeContext(ctx);
			return nullptr;
		}
		memset(ctx->video_pts_buf, 0, sizeof(int64_t) * NUM_VIDEO_FRAME_BUFFERS);
	}
	if (ctx->audio_decoder)
	{
		if (avcodec_open2(ctx->audio_stream->codec, ctx->audio_decoder, NULL) < 0)
		{
			Med_FreeContext(ctx);
			return nullptr;
		}
		
		int data_size = av_get_bytes_per_sample(ctx->audio_stream->codec->sample_fmt);
		_CircBuf_Init(&ctx->audio_buf, AUDIO_BUFFER_SIZE, data_size);
	}

	av_init_packet(&ctx->packet);

	
	ctx->thread = new std::thread(_DecodeRoutine, ctx);
	return ctx;
}


void Med_PollAudio(MediaContext* ctx, float* buf, int num)
{
	_Circbuf_Read(&ctx->audio_buf, buf, num, nullptr);
}
void Med_GetVideoDimensions(MediaContext* ctx, int* w, int* h)
{
	if (w) *w = ctx->video_stream->codec->width;
	if (h) *h = ctx->video_stream->codec->height;
}
bool Med_PollVideo(MediaContext* ctx, char* buf, float time)
{
	const int sz = _Circbuf_Size(&ctx->video_buf);

	if (sz <= 0) return false;
	AVRational timebase = ctx->video_stream->codec->pkt_timebase;

	
	ctx->pts_timer = (int64_t)(time * (timebase.den));
	
	int reading = -1;
	for (int i = 0; i < sz; i++)
	{
		const int curRead = (ctx->video_buf.curRead + i) % ctx->video_buf.numElements;
		const int64_t curFrame = ctx->video_pts_buf[curRead];

		if (curFrame < ctx->pts_timer)
		{
			reading = curRead;
		}
	
	}
	
	if (reading != -1)
	{
		void* outBuf = _Circbuf_GetIndex(&ctx->video_buf, reading);
		memcpy(buf, outBuf, ctx->video_buf.elementSize);
		ctx->video_buf.curRead = reading;
		return true;
	}

	return false;
}

bool Med_IsFinished(MediaContext* ctx)
{
	if (ctx->thread_should_close)
	{
		if (ctx->thread->joinable()) ctx->thread->join();
		return ctx->thread_should_close;
	}
	return false;
}


void Med_FreeContext(struct MediaContext* ctx)
{
	if (ctx->thread)
	{
		ctx->thread_should_close = true;
		ctx->thread->join();
		delete ctx->thread;
		ctx->thread_should_close = false;
	}

	if (ctx->fmt_ctx) { avformat_free_context(ctx->fmt_ctx); ctx->fmt_ctx = nullptr; }
	ctx->video_stream = nullptr;
	ctx->video_decoder = nullptr;
	ctx->audio_stream = nullptr;
	ctx->audio_decoder = nullptr;

	if (ctx->frame) { av_frame_free(&ctx->frame); ctx->frame = nullptr; }
	
	_CircBuf_Free(&ctx->video_buf);
	_CircBuf_Free(&ctx->audio_buf);

	if (ctx->sws_scaler_ctx)
	{
		sws_freeContext(ctx->sws_scaler_ctx);
		ctx->sws_scaler_ctx = nullptr;
	}
	if (ctx->video_pts_buf) { free(ctx->video_pts_buf); ctx->video_pts_buf = nullptr; }

	av_free_packet(&ctx->packet);



	free(ctx);
}