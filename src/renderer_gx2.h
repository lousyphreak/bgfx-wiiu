/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef BGFX_RENDERER_GX2_H_HEADER_GUARD
#define BGFX_RENDERER_GX2_H_HEADER_GUARD

#include <bgfx/platform_gx2.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/displaylist.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gfd.h>
#include <vector>
#include <algorithm>

namespace bgfx { namespace gx2
{
	// One physical allocation owner, shared by uploads, resources and retirement.
	struct Block
	{
		void* data;
		uint32_t size;
		OSTime stamp = 0;
	};

	struct Buffer
	{
		Block* block = NULL;
		std::vector<Block*> spare;
		uint32_t size = 0;
		uint16_t flags = 0;
		VertexLayoutHandle layout = BGFX_INVALID_HANDLE;
	};

	struct Uniform
	{
		UniformHandle handle;
		uint16_t offset;
		uint16_t rows;
		uint8_t type;
		uint8_t count;
	};

	struct Shader
	{
		Block* header = NULL;
		Block* program = NULL;
		std::vector<Uniform> uniforms;
		std::vector<PredefinedUniform> predefined;
		uint32_t uniformSize = 0;
		bool fragment = false;
	};

	struct Fetch
	{
		uint32_t layouts[BGFX_CONFIG_MAX_VERTEX_STREAMS];
		uint16_t instanceStride;
		uint32_t streamMask;
		GX2FetchShader shader;
		Block* program;
	};

	struct Program
	{
		Shader* vertex = NULL;
		Shader* fragment = NULL;
		std::vector<Fetch> fetches;
		PredefinedUniform m_predefined[PredefinedUniform::Count*2];
		uint16_t m_numPredefined = 0;
	};

	struct TextureStage
	{
		Block* block = NULL;
		std::vector<Block*> spare;
		Rect dirty = {};
	};

	struct TextureLevel
	{
		GX2Surface source = {};
		std::vector<TextureStage> slices;
	};

	struct Texture
	{
		GX2Texture texture = {};
		Block* image = NULL;
		std::vector<Block*> spareImages;
		std::vector<TextureLevel> levels;
		uint64_t flags = 0;
		TextureFormat::Enum format = TextureFormat::Unknown;
		uint32_t mipOffset = 0;
		bool pending = false;
	};
} }
#endif
