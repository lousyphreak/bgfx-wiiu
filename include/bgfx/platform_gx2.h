/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef BGFX_PLATFORM_GX2_H_HEADER_GUARD
#define BGFX_PLATFORM_GX2_H_HEADER_GUARD

#include <stdint.h>
#include <stddef.h>

struct GX2ColorBuffer;
struct GX2DepthBuffer;
struct GX2Texture;

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

	/// Immutable native output identity supplied as SwapChain::nwh. SDL and other
	/// hosts retain window/input metadata only; bgfx owns the entire GX2 session.
	/// display 0 is TV, display 1 is DRC. Without a DRC swapchain, TV is mirrored.
	/// renderCore selects the internal render worker's core at thread creation.
	/// -1 selects core 0 for a worker; inline mode stays on the calling core.
	struct Gx2PlatformData
	{
		uint32_t display = 0;
		int32_t renderCore = -1;
	};

	/// API-thread foreground transition. Suspension completes queued CPU/GPU work
	/// and releases foreground storage before returning. Resume clears suspension;
	/// the next rendering frame recreates its presentation resources.
	void gx2Suspend(bool _suspend);
}
#endif
