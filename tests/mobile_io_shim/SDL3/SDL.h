// Narrow host declarations for rom_file.h tests; not an Android/SDL emulator.
#pragma once
#include <cstddef>
#include <cstdint>
struct SDL_IOStream;
enum SDL_IOStatus { SDL_IO_STATUS_READY, SDL_IO_STATUS_ERROR, SDL_IO_STATUS_EOF };
char *SDL_GetPrefPath(const char *, const char *);
void SDL_free(void *);
const char *SDL_GetError();
std::uint64_t SDL_GetTicksNS();
SDL_IOStream *SDL_IOFromFile(const char *, const char *);
std::size_t SDL_ReadIO(SDL_IOStream *, void *, std::size_t);
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream *);
bool SDL_CloseIO(SDL_IOStream *);
