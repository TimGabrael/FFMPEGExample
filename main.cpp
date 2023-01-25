extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavdevice/avdevice.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
}
#include "wrapper.h"
#include <iostream>
#include <Windows.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <chrono>

MediaContext* ctx = nullptr;
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
	float* out = (float*)pOutput;
	if (ctx)
	{
		Med_PollAudio(ctx, out, frameCount);
	}
}

const char* FILE_NAME = "test.mp4";
int main()
{
	av_register_all();
	avformat_network_init();
	
	ctx = Med_CreateContext(FILE_NAME);

	ma_result result;
	ma_decoder decoder;
	ma_device_config deviceConfig;
	ma_device device;
	deviceConfig = ma_device_config_init(ma_device_type_playback);
	deviceConfig.playback.format = ma_format::ma_format_f32;
	deviceConfig.playback.channels = 1;
	deviceConfig.sampleRate = 44100;
	deviceConfig.dataCallback = data_callback;
	deviceConfig.pUserData = nullptr;


	if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
		printf("Failed to open playback device.\n");
		ma_decoder_uninit(&decoder);
		return -3;
	}

	if (ma_device_start(&device) != MA_SUCCESS) {
		printf("Failed to start playback device.\n");
		ma_device_uninit(&device);
		ma_decoder_uninit(&decoder);
		return -4;
	}

	int w, h = 0;
	Med_GetVideoDimensions(ctx, &w, &h);

	char* imageBuffer = new char[w * h * 4];

	auto start = std::chrono::high_resolution_clock::now();
	while (!Med_IsFinished(ctx))
	{
		auto now = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration<float>(now - start).count();
		Med_PollVideo(ctx, imageBuffer, dt);

		HBITMAP hbmp = CreateBitmap(w, h, 1, 32, imageBuffer);
		
		HDC dc = GetDC(NULL);
		
		HDC hdc = CreateCompatibleDC(dc);
		auto oldBitmap = SelectObject(hdc, hbmp);
		
		BitBlt(dc, 600, 600, w, h, hdc, 0, 0, SRCCOPY);
		
		SelectObject(hdc, oldBitmap);
		DeleteDC(hdc);
		
		DeleteObject(hbmp);
		DeleteDC(hdc);
		ReleaseDC(NULL, dc);

		Sleep(1);
	}


}