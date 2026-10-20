// Test-only drawing recorder. Never include this directory in a game build.
#pragma once
struct SDL_Renderer {};
struct SDL_Rect { int x, y, w, h; };
int SDL_RenderFillRects(SDL_Renderer *, const SDL_Rect *, int);
