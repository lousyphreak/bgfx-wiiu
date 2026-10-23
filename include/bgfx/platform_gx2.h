/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef BGFX_PLATFORM_GX2_H_HEADER_GUARD
#define BGFX_PLATFORM_GX2_H_HEADER_GUARD

struct GX2ColorBuffer;
struct GX2ContextState;

namespace bgfx
{
	/// Borrowed SDL/WUT target, queried on the application's main thread each frame.
	/// Returning false (or null pointers) suspends drawing and presentation.
	struct Gx2Target
	{
		GX2ColorBuffer* color;
		GX2ContextState* context;
	};

	/// Pass this descriptor as Init::swapChain.nwh; keep it and userData alive
	/// until shutdown. GX2 must already be initialized. Use inline rendering
	/// (BGFX_CONFIG_MULTITHREADED=0) and BGFX_RESET_FLIP_AFTER_RENDER.
	/// query reads current availability, not a cached foreground generation.
	/// present calls SDL_WIIU_SwapWindow, including its flush/copy/swap sequence.
	/// Process SDL lifecycle events before bgfx::frame; shut bgfx down before SDL.
	struct Gx2PlatformData
	{
		void* userData;
		bool (*query)(void* userData, Gx2Target* target);
		void (*present)(void* userData);
	};
}
#endif
