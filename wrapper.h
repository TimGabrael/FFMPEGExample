#pragma once



struct MediaContext* Med_CreateContext(const char* file);

void Med_PollAudio(MediaContext* ctx, float* buf, int num);
void Med_GetVideoDimensions(MediaContext* ctx, int* w, int* h);
bool Med_PollVideo(MediaContext* ctx, char* buf);

void Med_SetPlaybackTime(MediaContext* ctx, float time);
void Med_SetPlaybackState(MediaContext* ctx, bool paused);

bool Med_IsFinished(MediaContext* ctx);

void Med_FreeContext(struct MediaContext* ctx);