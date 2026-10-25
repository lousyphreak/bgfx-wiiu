/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef BGFX_PLATFORM_GX2_H_HEADER_GUARD
#define BGFX_PLATFORM_GX2_H_HEADER_GUARD

#include <stdint.h>

struct GX2ColorBuffer;
struct GX2DepthBuffer;
struct GX2Texture;
struct GX2ContextState;

namespace bgfx
{
	/// Optional native render metadata for createTexture*'s _external argument.
	/// Pass getHandle(), rather than this descriptor's address. A plain GX2Texture*
	/// remains valid for texture-only imports. Keep this descriptor, its referenced
	/// descriptors and all allocations alive through destruction and GPU retirement.
	/// Supply color OR depth for an imported render target. Their full surfaces
	/// match the texture's dimensions/format; bgfx attachment views select mip/layers.
	/// Color AA sampling shares image storage with color; an ordinary resolved
	/// texture may instead accompany a separate multisample color buffer. Depth
	/// needing conversion uses a separate texture allocation: matching AA for
	/// MSAA_SAMPLE, or single-sample storage for ordinary resolved depth.
	/// Imported render storage is authoritative at creation and already published
	/// by the caller. bgfx manages transitions for its own subsequent commands.
	/// Caller writes require its own GPU ordering/publication before bgfx consumes
	/// them. Allocation/view metadata is immutable while the texture exists;
	/// bgfx updates the supplied depth descriptor's clear values after full clears
	/// so native users can later restore the matching Hi-Z clear registers.
	/// Layered depth and shared texture/depth storage must be expanded without
	/// Hi-Z before import. bgfx keeps Hi-Z disabled there; native writes to shared
	/// storage cannot update separate Hi-Z metadata. Separate single-layer depth
	/// may use the supplied Hi-Z allocation and clear values.
	struct Gx2ExternalTexture
	{
		const GX2Texture* texture;
		const GX2ColorBuffer* color;
		GX2DepthBuffer* depth;

		uint64_t getHandle() const { return uint64_t(uintptr_t(this)) | 1; }
	};

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
	/// (BGFX_CONFIG_MULTITHREADED=0); Wii U builds always present after rendering.
	/// query reads current availability, not a cached foreground generation.
	/// present must submit preceding bgfx work before returning. SDL_WIIU_SwapWindow
	/// provides that submission before its adapter-owned scan copy/swap sequence.
	/// When using separate TV/DRC windows, touch/render both each frame so SDL can
	/// complete its shared swap. A mirrored SDL window needs one callback.
	/// Process SDL lifecycle events before bgfx::frame; only issue GPU resource
	/// commands while foreground-ready. Shut bgfx down before SDL.
	/// The RGBA8 adapter owns presentation dimensions/format and scan buffers.
	/// bgfx supplies requested 2x/4x/8x MSAA with a retained color target when needed.
	/// For headless rendering, leave Init::swapChain.nwh null and width/height zero.
	/// bgfx owns an aligned GX2 context state, but GX2 must already be initialized
	/// by SDL/the host. Only explicit offscreen framebuffers are drawable; frames
	/// flush without presenting. This does not initialize or shut down the GX2 SDK.
	struct Gx2PlatformData
	{
		void* userData;
		bool (*query)(void* userData, Gx2Target* target);
		void (*present)(void* userData);
	};
}
#endif
