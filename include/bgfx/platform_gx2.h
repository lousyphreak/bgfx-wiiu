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

	/// Pass one descriptor per native window as SwapChain::nwh, including the
	/// main Init::swapChain; keep it and userData alive
	/// until shutdown. GX2 must already be initialized. Use inline rendering
	/// (BGFX_CONFIG_MULTITHREADED=0) and BGFX_RESET_FLIP_AFTER_RENDER.
	/// query reads current availability, not a cached foreground generation.
	/// present must submit preceding bgfx work before returning. SDL_WIIU_SwapWindow
	/// provides that submission before its adapter-owned scan copy/swap sequence.
	/// When using separate TV/DRC windows, touch/render both each frame so SDL can
	/// complete its shared swap. A mirrored SDL window needs one callback.
	/// Process SDL lifecycle events before bgfx::frame; only issue GPU resource
	/// commands while foreground-ready. Shut bgfx down before SDL.
	/// The RGBA8 adapter owns presentation dimensions/format and scan buffers.
	/// bgfx supplies requested 2x/4x/8x MSAA with a retained color target when needed.
	struct Gx2PlatformData
	{
		void* userData;
		bool (*query)(void* userData, Gx2Target* target);
		void (*present)(void* userData);
	};
}
#endif
