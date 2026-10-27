/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef ENTRY_SDL3_H_HEADER_GUARD
#define ENTRY_SDL3_H_HEADER_GUARD
#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <bx/platform.h>
#if BX_PLATFORM_WIIU
#include <bgfx/platform_gx2.h>
#endif

namespace entry { namespace sdl3
{
	// Called before each bgfx initialization, including example restarts.
	inline void prepareBgfx()
	{
#if !defined(BGFX_CONFIG_MULTITHREADED) || BGFX_CONFIG_MULTITHREADED
		if (!SDL_GetHintBoolean("BGFX_RENDER_THREAD", false)) bgfx::renderFrame();
#endif
	}

	inline bool lifecycle(void*, SDL_Event* _event)
	{
#if BX_PLATFORM_WIIU
		if (_event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || _event->type == SDL_EVENT_TERMINATING)
			bgfx::gx2Suspend(true);
		else if (_event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
			bgfx::gx2Suspend(false);
#else
		(void)_event;
#endif
		return true;
	}
} }
#endif
