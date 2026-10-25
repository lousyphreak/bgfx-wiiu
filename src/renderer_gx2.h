/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef BGFX_RENDERER_GX2_H_HEADER_GUARD
#define BGFX_RENDERER_GX2_H_HEADER_GUARD

#include <bgfx/platform_gx2.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memorymap.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/debug.h>
#include <gx2/displaylist.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/query.h>
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
		MEMHeapHandle heap = NULL; // NULL is persistent MEM2/default-heap storage.
		OSTime stamp = 0;
	};

	struct SubmissionRing
	{
		struct Epoch { uint64_t end; OSTime stamp; };
		Block* block = NULL;
		uint64_t head = 0, tail = 0, sealed = 0;
		std::vector<Epoch> epochs;
	};

	struct OcclusionSample
	{
		Block* block;
		OcclusionQueryHandle handle;
	};

	struct GpuTimerSample
	{
		Block* block; // Separate 64-byte cache lines for begin/end samples.
		uint32_t frameNum;
		uint16_t view; // BGFX_CONFIG_MAX_VIEWS is the frame interval.
	};

	struct GpuTimerResult
	{
		int64_t begin = 0;
		int64_t end = 0;
		uint32_t frameNum = 0;
	};

	struct TransferOutput
	{
		enum Kind { TextureRead, ScreenShot, CaptureFrame, CaptureEnd };
		Kind kind = TextureRead;
		Block* block = NULL;
		void* destination = NULL;
		uint32_t deadline = 0;
		uint32_t width = 0, height = 0, pitch = 0;
		uint32_t rowBytes = 0, rows = 0;
		TextureFormat::Enum format = TextureFormat::Unknown;
		std::vector<char> path;
	};

	struct Buffer
	{
		Block* block = NULL;
		std::vector<Block*> spare;
		uint32_t size = 0;
		uint16_t flags = 0;
		bool gpuWritten = false;
		bool littleEndian = false; // Byte representation; a raw GPU copy can still carry CPU-endian words.
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

	struct ComputeResource
	{
		uint32_t binding, kind, format, elementBytes, read, write;
	};

	struct ComputeOutput
	{
		uint32_t binding, offset, size, elementBytes;
		GX2Surface surface = {}; // One linear layer; layers occupy imageSize each.
		uint32_t layers = 1;
	};

	struct Shader
	{
		Block* header = NULL;
		Block* program = NULL;
		Block* primitiveHeader = NULL;
		Block* primitiveProgram = NULL;
		std::vector<Uniform> uniforms;
		std::vector<PredefinedUniform> predefined;
		uint32_t uniformSize = 0;
		uint32_t rawSrvMask = 0;
		bool fragment = false;
		bool sampleFrequency = false;
		bool compute = false;
		std::vector<ComputeResource> resources;
		uint8_t samplerDimensions[16] = {}; // TextureDimension + 1; zero is unused.
	};

	struct Fetch
	{
		uint32_t layouts[BGFX_CONFIG_MAX_VERTEX_STREAMS];
		uint16_t instanceStride;
		uint32_t streamMask;
		uint32_t gpuEndianMask;
		GX2FetchShader shader;
		Block* program;
	};

	struct Program
	{
		Shader* vertex = NULL;
		Shader* fragment = NULL;
		Shader* compute = NULL;
		bool primitiveId = false;
		uint32_t samplerMask = 0;
		std::vector<Fetch> fetches;
		std::vector<PredefinedUniform> m_predefined;
		uint16_t m_numPredefined = 0;
	};

	struct TextureStage
	{
		Block* block = NULL;
		std::vector<Block*> spare;
		std::vector<Rect> dirty;
		bool renderDirty = false;
	};

	struct TextureLevel
	{
		GX2Surface source = {};
		std::vector<TextureStage> slices;
	};

	struct RenderView
	{
		uint16_t mip, layer, count;
		bool autoMips = false;
		bool srgb = false;
	};

	struct Texture
	{
		GX2Texture texture = {};
		// Ordinary draws read this prefix; conversion, staging and alias data follow.
		uint64_t flags = 0;
		Block* image = NULL;
		TextureFormat::Enum format = TextureFormat::Unknown;
		TextureFormat::Enum requestedFormat = TextureFormat::Unknown;
		std::vector<RenderView> rendered;
		const Gx2ExternalTexture* external = NULL;
		GX2Texture arrayView = {};
		Block* arrayImage = NULL;
		std::vector<Block*> spareArrayImages;
		bool arrayDirty = true;
		GX2Surface renderSurface = {};
		GX2Surface depthSamples = {}, resolvedDepth = {};
		Block* depthSamplesImage = NULL;
		Block* resolvedDepthImage = NULL;
		Block* renderImage = NULL;
		Block* auxiliary = NULL;
		Block* hiZ = NULL;
		std::vector<Block*> spareImages;
		std::vector<TextureLevel> levels;
		uint32_t mipOffset = 0;
		bool pending = false;
		float depthClear = 1.0f;
		uint32_t stencilClear = 0;
	};
	// Adapter targets are borrowed for this inline frame. Only Blocks are owned.
	struct Window
	{
		SwapChain swapChain;
		Gx2Target target = {};
		GX2ColorBuffer color = {};
		GX2DepthBuffer depth = {};
		Block* colorImage = NULL;
		Block* auxiliary = NULL;
		Block* depthImage = NULL;
		Block* hiZ = NULL;
		bool used = false;
		bool colorDirty = false;
	};

	struct FrameBuffer
	{
		Window window;
		std::vector<Attachment> attachments;
	};
} }
#endif
