// Host implementation lives in test_vita_sound_worker.cpp; game builds use SDL2.
#pragma once
#include <cstddef>
#define SDLCALL
struct SDL_Thread;
struct SDL_mutex;
struct SDL_cond;
SDL_mutex *SDL_CreateMutex();
void SDL_DestroyMutex(SDL_mutex *);
int SDL_LockMutex(SDL_mutex *);
int SDL_UnlockMutex(SDL_mutex *);
SDL_cond *SDL_CreateCond();
void SDL_DestroyCond(SDL_cond *);
int SDL_CondWait(SDL_cond *, SDL_mutex *);
int SDL_CondSignal(SDL_cond *);
SDL_Thread *SDL_CreateThreadWithStackSize(int (*)(void *), const char *, size_t, void *);
void SDL_WaitThread(SDL_Thread *, int *);

using Uint8 = unsigned char;
using Uint16 = unsigned short;
using Uint32 = unsigned int;
using SDL_AudioDeviceID = Uint32;
constexpr Uint16 AUDIO_F32SYS = 1, AUDIO_S16SYS = 2;
struct SDL_AudioStream;
struct SDL_AudioSpec {
    int freq = 0;
    Uint16 format = 0;
    Uint8 channels = 0;
    Uint16 samples = 0;
    void (*callback)(void *, Uint8 *, int) = nullptr;
    void *userdata = nullptr;
};
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
void SDL_CloseAudioDevice(SDL_AudioDeviceID);
void SDL_LockAudioDevice(SDL_AudioDeviceID);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID);
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int);
SDL_AudioStream *SDL_NewAudioStream(Uint16, Uint8, int, Uint16, Uint8, int);
void SDL_FreeAudioStream(SDL_AudioStream *);
int SDL_AudioStreamAvailable(SDL_AudioStream *);
int SDL_AudioStreamGet(SDL_AudioStream *, void *, int);
int SDL_AudioStreamPut(SDL_AudioStream *, const void *, int);
void SDL_AudioStreamClear(SDL_AudioStream *);
const char *SDL_GetError();
