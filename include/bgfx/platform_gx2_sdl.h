/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef BGFX_PLATFORM_GX2_SDL_H_HEADER_GUARD
#define BGFX_PLATFORM_GX2_SDL_H_HEADER_GUARD

#include <bgfx/platform_gx2.h>
#include <SDL3/SDL_wiiu.h>

namespace bgfx
{
	/// Install the native text-input graphics bridge for an SDL window created
	/// with SDL_HINT_VIDEO_EXTERNAL_GRAPHICS. Call after bgfx init, on its API
	/// thread (also the SDL main thread). Keep bgfx alive until text input stops;
	/// clear this window property before shutdown. Normal SDL activation/auto
	/// policy applies. Foreground event handling still uses gx2Suspend().
	inline bool gx2SetSDLTextInputWindow(SDL_Window* _window)
	{
		static SDL_WiiUTextInputGraphics graphics = {
			[](void*, SDL_WiiUTextInputGraphicsOperation operation, SDL_WiiUTextInputCallback callback, void* userdata) -> bool
			{
				return gx2RunNativeUI(static_cast<Gx2NativeUIOperation>(operation), callback, userdata);
			},
			[](void*, SDL_WiiUTextInputDrawCallback tv, SDL_WiiUTextInputDrawCallback drc) -> bool
			{
				return gx2DrawNativeUI(tv, drc);
			},
			nullptr
		};
		return SDL_SetPointerProperty(SDL_GetWindowProperties(_window),
			SDL_PROP_WINDOW_WIIU_TEXT_INPUT_GRAPHICS_POINTER, &graphics);
	}
}
#endif
