/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#include "bgfx_p.h"

#if BGFX_CONFIG_RENDERER_GX2
#include "renderer.h"
#include "renderer_gx2.h"
#include "renderer_gx2_native.h"
#include "renderer_gx2_profiler.h"
#include "cs_gx2_indirect.gx2.bin.h"
#include "cs_gx2_indirect_indexed.gx2.bin.h"
#include "cs_gx2_indirect_dispatch.gx2.bin.h"

namespace bgfx { namespace gx2
{
	static const OSTime kPending = INT64_MAX;
	// Four outstanding swaps trigger native hang detection, even at interval zero.
	static constexpr uint32_t kQueuedFrames = BGFX_CONFIG_MAX_FRAME_LATENCY < 3 ? BGFX_CONFIG_MAX_FRAME_LATENCY : 3;
	static constexpr uint32_t kTextureSamplers = BGFX_CONFIG_MAX_TEXTURE_SAMPLERS < 16 ? BGFX_CONFIG_MAX_TEXTURE_SAMPLERS : 16;
	static constexpr uint32_t kColorTargets = BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS < 8 ? BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS : 8;

	// Budget queued frames plus the frame being recorded, using ordinary draw sizes.
	// Existing draw/latency limits shrink small builds without new configuration.
	static constexpr uint32_t kBufferedDraws = BGFX_CONFIG_MAX_DRAW_CALLS < 768 ? BGFX_CONFIG_MAX_DRAW_CALLS : 768;
	static constexpr uint32_t kListReservation = 256*1024;
	static constexpr uint32_t kMaxUniformSnapshot = 2*64*1024;
	static constexpr uint32_t kListRingSize = kListReservation + kBufferedDraws*256*(BGFX_CONFIG_MAX_FRAME_LATENCY+1);
	static constexpr uint32_t kUniformRingSize = kMaxUniformSnapshot + kBufferedDraws*512*(BGFX_CONFIG_MAX_FRAME_LATENCY+1);
	static constexpr GX2ShaderMode kGraphicsShaderMode = BGFX_CONFIG_GX2_UNIFORM_REGISTERS
		? GX2_SHADER_MODE_UNIFORM_REGISTER : GX2_SHADER_MODE_UNIFORM_BLOCK;

	static void unsupported(const char* _operation)
	{
		BGFX_FATAL(false, Fatal::UnableToInitialize, "GX2: %s is not supported by the graphics backend.", _operation);
	}

	static uint32_t readLE(const uint8_t*& _ptr, uint32_t _bytes)
	{
		uint32_t value = 0;
		for (uint32_t ii = 0; ii < _bytes; ++ii) value |= uint32_t(*_ptr++) << (ii*8);
		return value;
	}

	template<bool _registers = false>
	static void writeFloats(void* _dst, const void* _src, uint32_t _count)
	{
		uint32_t* dst = (uint32_t*)_dst;
		const uint8_t* src = (const uint8_t*)_src;
		// Snapshots are vec4-heavy; reduce branches without a separate copy path.
#pragma GCC unroll 4
		for (uint32_t ii = 0; ii < _count; ++ii)
		{
			uint32_t bits;
			// Registers consume native words; GPU uniform memory is little endian.
			// Keep this fixed-size bit copy visible to the compiler.
			__builtin_memcpy(&bits, src+ii*4, 4);
			dst[ii] = _registers ? bits : bx::toLittleEndian(bits);
		}
	}

	// Full row-major 4x4 product: two output columns per paired-single operation.
	// GQR0 is the ABI's unquantized float setting; only volatile FPRs are used.
	static void multiplyMatrixPaired(float* _dst, const float* _a, const float* _b)
	{
		__asm__ volatile (
			".machine push; .machine gekko;"
			"psq_l 0, 0(%2), 0, 0; psq_l 1, 8(%2), 0, 0;"
			"psq_l 2, 16(%2), 0, 0; psq_l 3, 24(%2), 0, 0;"
			"psq_l 4, 32(%2), 0, 0; psq_l 5, 40(%2), 0, 0;"
			"psq_l 6, 48(%2), 0, 0; psq_l 7, 56(%2), 0, 0;"
			".set .LmatrixRow%=, 0; .rept 4;"
			"psq_l 8, .LmatrixRow%=(%1), 0, 0;"
			"psq_l 9, .LmatrixRow%=+8(%1), 0, 0;"
			"ps_muls0 10, 0, 8; ps_muls0 11, 1, 8;"
			"ps_madds1 10, 2, 8, 10; ps_madds1 11, 3, 8, 11;"
			"ps_madds0 10, 4, 9, 10; ps_madds0 11, 5, 9, 11;"
			"ps_madds1 10, 6, 9, 10; ps_madds1 11, 7, 9, 11;"
			"psq_st 10, .LmatrixRow%=(%0), 0, 0;"
			"psq_st 11, .LmatrixRow%=+8(%0), 0, 0;"
			".set .LmatrixRow%=, .LmatrixRow%=+16; .endr; .machine pop;"
			: : "b" (_dst), "b" (_a), "b" (_b)
			: "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "memory");
	}

	static void prepareCpuOverwrite(void* _data, uint32_t _bytes)
	{
		uint8_t* data = (uint8_t*)_data;
		// Partial cache blocks retain their surrounding CPU-written bytes.
		const uint32_t head = uint32_t(-uintptr_t(data)) & 31;
		if (_bytes >= head+32) DCZeroRange(data+head, (_bytes-head) & ~31u);
	}

	static void copyTextureRow(void* _dst, const void* _src, uint32_t _bytes, uint8_t _swapWordSize)
	{
		uint8_t* dst = (uint8_t*)_dst;
		const uint8_t* src = (const uint8_t*)_src;
		if (_swapWordSize > 1) prepareCpuOverwrite(dst, _bytes);
		switch (_swapWordSize)
		{
		case 2:
			for (uint32_t ii = 0; ii < _bytes; ii += 2)
				bx::storeUnaligned(dst+ii, bx::endianSwap(bx::loadUnaligned<uint16_t>(src+ii)));
			break;
		case 3:
			for (uint32_t ii = 0; ii < _bytes; ii += 3)
			{
				dst[ii] = src[ii+2]; dst[ii+1] = src[ii+1]; dst[ii+2] = src[ii];
			}
			break;
		case 4:
			for (uint32_t ii = 0; ii < _bytes; ii += 4)
				bx::storeUnaligned(dst+ii, bx::endianSwap(bx::loadUnaligned<uint32_t>(src+ii)));
			break;
		default:
			// Keep preparation close to the copy, even for multi-megabyte buffers.
			while (_bytes)
			{
				const uint32_t bytes = bx::min(_bytes, 3u*1024);
				prepareCpuOverwrite(dst, bytes);
				bx::memCopy(dst, src, bytes);
				dst += bytes; src += bytes; _bytes -= bytes;
			}
			break;
		}
	}

	static uint32_t textureStorageBlockSize(TextureFormat::Enum _format)
	{
		// bimg packs D24/D24F into three bytes; GX2's R24_X8/D24_S8 use four.
		return _format == TextureFormat::D24 || _format == TextureFormat::D24F
			? 4 : bimg::getBlockInfo(bimg::TextureFormat::Enum(_format)).blockSize;
	}

	static const char* s_attribName[] =
	{
		"a_position",
		"a_normal",
		"a_tangent",
		"a_bitangent",
		"a_color0",
		"a_color1",
		"a_color2",
		"a_color3",
		"a_indices",
		"a_weight",
		"a_texcoord0",
		"a_texcoord1",
		"a_texcoord2",
		"a_texcoord3",
		"a_texcoord4",
		"a_texcoord5",
		"a_texcoord6",
		"a_texcoord7",
		"a_texcoord8",
		"a_texcoord9",
		"a_texcoord10",
		"a_texcoord11",
		"a_texcoord12",
		"a_texcoord13",
		"a_texcoord14",
		"a_texcoord15",
	};

	static const GX2CompareFunction s_compare[] =
	{
		GX2_COMPARE_FUNC_ALWAYS, GX2_COMPARE_FUNC_LESS, GX2_COMPARE_FUNC_LEQUAL,
		GX2_COMPARE_FUNC_EQUAL, GX2_COMPARE_FUNC_GEQUAL, GX2_COMPARE_FUNC_GREATER,
		GX2_COMPARE_FUNC_NOT_EQUAL, GX2_COMPARE_FUNC_NEVER, GX2_COMPARE_FUNC_ALWAYS,
	};
	static const GX2StencilFunction s_stencil[] =
	{
		GX2_STENCIL_FUNCTION_ZERO, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_REPLACE,
		GX2_STENCIL_FUNCTION_INCR_WRAP, GX2_STENCIL_FUNCTION_INCR_CLAMP,
		GX2_STENCIL_FUNCTION_DECR_WRAP, GX2_STENCIL_FUNCTION_DECR_CLAMP, GX2_STENCIL_FUNCTION_INV,
	};
	static const GX2BlendMode s_blend[] =
	{
		GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE,
		GX2_BLEND_MODE_SRC_COLOR, GX2_BLEND_MODE_INV_SRC_COLOR,
		GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
		GX2_BLEND_MODE_DST_ALPHA, GX2_BLEND_MODE_INV_DST_ALPHA,
		GX2_BLEND_MODE_DST_COLOR, GX2_BLEND_MODE_INV_DST_COLOR,
		GX2_BLEND_MODE_SRC_ALPHA_SAT, GX2_BLEND_MODE_BLEND_FACTOR, GX2_BLEND_MODE_INV_BLEND_FACTOR,
	};
	static const GX2BlendCombineMode s_equation[] =
	{
		GX2_BLEND_COMBINE_MODE_ADD, GX2_BLEND_COMBINE_MODE_SUB,
		GX2_BLEND_COMBINE_MODE_REV_SUB, GX2_BLEND_COMBINE_MODE_MIN, GX2_BLEND_COMBINE_MODE_MAX,
	};
	static const GX2PrimitiveMode s_primitive[] =
	{
		GX2_PRIMITIVE_MODE_TRIANGLES, GX2_PRIMITIVE_MODE_TRIANGLE_STRIP,
		GX2_PRIMITIVE_MODE_LINES, GX2_PRIMITIVE_MODE_LINE_STRIP, GX2_PRIMITIVE_MODE_POINTS,
	};

	static uint32_t colorWriteMask(uint64_t _state)
	{
		return (_state & BGFX_STATE_WRITE_R ? 1 : 0) | (_state & BGFX_STATE_WRITE_G ? 2 : 0)
			| (_state & BGFX_STATE_WRITE_B ? 4 : 0) | (_state & BGFX_STATE_WRITE_A ? 8 : 0);
	}

	static bool supportsMipAutogen(TextureFormat::Enum _format)
	{
		switch (_format)
		{
		case TextureFormat::R8:
		case TextureFormat::R8S:
		case TextureFormat::R16:
		case TextureFormat::R16S:
		case TextureFormat::R16F:
		case TextureFormat::R32F:
		case TextureFormat::RG8:
		case TextureFormat::RG8S:
		case TextureFormat::RG16:
		case TextureFormat::RG16S:
		case TextureFormat::RG16F:
		case TextureFormat::RG32F:
		case TextureFormat::RGBA8S:
		case TextureFormat::RGBA16S:
		case TextureFormat::A8:
		case TextureFormat::R5G6B5:
		case TextureFormat::B5G6R5:
		case TextureFormat::RGBA4:
		case TextureFormat::BGRA4:
		case TextureFormat::RGB5A1:
		case TextureFormat::BGR5A1:
		case TextureFormat::RGB10A2:
		case TextureFormat::RG11B10F:
			return true;
		default:
			return MipGen::isSupported(_format);
		}
	}

	static GX2SurfaceFormat textureFormat(TextureFormat::Enum _format, bool _srgb, uint32_t& _compMap)
	{
		_compMap = GX2_COMP_MAP(0, 1, 2, 3);
		if (_srgb)
		{
			if (_format == TextureFormat::RGBA8 || _format == TextureFormat::BGRA8)
			{
				if (_format == TextureFormat::BGRA8) _compMap = GX2_COMP_MAP(2, 1, 0, 3);
				return GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8;
			}
			if (_format >= TextureFormat::BC1 && _format <= TextureFormat::BC3)
				return GX2SurfaceFormat(uint32_t(GX2_SURFACE_FORMAT_SRGB_BC1) + uint32_t(_format)-uint32_t(TextureFormat::BC1));
		}
		switch (_format)
		{
		case TextureFormat::R8: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_UNORM_R8;
		case TextureFormat::R8S: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_SNORM_R8;
		case TextureFormat::R8I: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_SINT_R8;
		case TextureFormat::R8U: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_UINT_R8;
		case TextureFormat::R16: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_UNORM_R16;
		case TextureFormat::R16S: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_SNORM_R16;
		case TextureFormat::R16I: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_SINT_R16;
		case TextureFormat::R16U: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_UINT_R16;
		case TextureFormat::R16F: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_FLOAT_R16;
		case TextureFormat::R32I: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_SINT_R32;
		case TextureFormat::R32U: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_UINT_R32;
		case TextureFormat::R32F: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_FLOAT_R32;
		case TextureFormat::RG8: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_UNORM_R8_G8;
		case TextureFormat::RG8S: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_SNORM_R8_G8;
		case TextureFormat::RG8I: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_SINT_R8_G8;
		case TextureFormat::RG8U: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_UINT_R8_G8;
		case TextureFormat::RG16: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_UNORM_R16_G16;
		case TextureFormat::RG16S: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_SNORM_R16_G16;
		case TextureFormat::RG16I: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_SINT_R16_G16;
		case TextureFormat::RG16U: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_UINT_R16_G16;
		case TextureFormat::RG16F: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_FLOAT_R16_G16;
		case TextureFormat::RG32I: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_SINT_R32_G32;
		case TextureFormat::RG32U: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_UINT_R32_G32;
		case TextureFormat::RG32F: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_FLOAT_R32_G32;
		case TextureFormat::RGBA8: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
		case TextureFormat::RGBA8S: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_SNORM_R8_G8_B8_A8;
		case TextureFormat::RGBA8I: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_SINT_R8_G8_B8_A8;
		case TextureFormat::RGBA8U: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UINT_R8_G8_B8_A8;
		case TextureFormat::RGBA16: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R16_G16_B16_A16;
		case TextureFormat::RGBA16S: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_SNORM_R16_G16_B16_A16;
		case TextureFormat::RGBA16I: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_SINT_R16_G16_B16_A16;
		case TextureFormat::RGBA16U: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UINT_R16_G16_B16_A16;
		case TextureFormat::RGBA16F: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_FLOAT_R16_G16_B16_A16;
		case TextureFormat::RGBA32I: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_SINT_R32_G32_B32_A32;
		case TextureFormat::RGBA32U: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UINT_R32_G32_B32_A32;
		case TextureFormat::RGBA32F: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_FLOAT_R32_G32_B32_A32;
		case TextureFormat::A8: _compMap = GX2_COMP_MAP(5, 5, 5, 0); return GX2_SURFACE_FORMAT_UNORM_R8;
		case TextureFormat::BGRA8: _compMap = GX2_COMP_MAP(2, 1, 0, 3); return GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
		case TextureFormat::BC1: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_BC1;
		case TextureFormat::BC2: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_BC2;
		case TextureFormat::BC3: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_BC3;
		case TextureFormat::BC4: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_UNORM_BC4;
		case TextureFormat::BC5: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_UNORM_BC5;
		case TextureFormat::BC4S: _compMap = GX2_COMP_MAP(0, 4, 4, 5); return GX2_SURFACE_FORMAT_SNORM_BC4;
		case TextureFormat::BC5S: _compMap = GX2_COMP_MAP(0, 1, 4, 5); return GX2_SURFACE_FORMAT_SNORM_BC5;
		case TextureFormat::R5G6B5: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R5_G6_B5;
		case TextureFormat::RGBA4: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R4_G4_B4_A4;
		case TextureFormat::RGB5A1: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1;
		case TextureFormat::RGB10A2: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R10_G10_B10_A2;
		case TextureFormat::RGB10A2U: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UINT_R10_G10_B10_A2;
		case TextureFormat::RG11B10F: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_FLOAT_R11_G11_B10;
		case TextureFormat::D16: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R16;
		case TextureFormat::D24: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R24_X8;
		case TextureFormat::D24S8: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_R24_X8;
		case TextureFormat::D24F: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_FLOAT_D24_S8;
		case TextureFormat::D32FS8: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_FLOAT_X8_X24;
		case TextureFormat::D32F: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_FLOAT_R32;
		case TextureFormat::B5G6R5: _compMap = GX2_COMP_MAP(2, 1, 0, 3); return GX2_SURFACE_FORMAT_UNORM_R5_G6_B5;
		case TextureFormat::BGRA4: _compMap = GX2_COMP_MAP(2, 1, 0, 3); return GX2_SURFACE_FORMAT_UNORM_R4_G4_B4_A4;
		case TextureFormat::BGR5A1: _compMap = GX2_COMP_MAP(2, 1, 0, 3); return GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1;
		default: return GX2_SURFACE_FORMAT_INVALID;
		}
	}

	struct RendererContextGX2 : public RendererContextI
	{
		RendererContextGX2(const Init& _init)
		{
			m_native.start();
			m_native.acquire(_init.swapChain.height);
			m_context = m_native.context;
			GX2_PROFILE(init());
			m_defaultVertex = allocate(16);
			bx::memSet(m_defaultVertex->data, 0, 16);
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, m_defaultVertex->data, 16);
			m_mainWindow.swapChain = _init.swapChain;
			g_caps.supported = BGFX_CAPS_IMAGE_RW | BGFX_CAPS_DRAW_INDIRECT | BGFX_CAPS_DRAW_INDIRECT_COUNT | BGFX_CAPS_SWAP_CHAIN | BGFX_CAPS_INDEX32 | BGFX_CAPS_BLEND_INDEPENDENT | BGFX_CAPS_VERTEX_ATTRIB_UINT10 | BGFX_CAPS_TEXTURE_EXTERNAL | BGFX_CAPS_TEXTURE_CUBE_ARRAY | BGFX_CAPS_VIEWPORT_LAYER_ARRAY | BGFX_CAPS_PRIMITIVE_ID;
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			g_caps.supported &= ~BGFX_CAPS_IMAGE_RW; // Graphics images require uniform block 15.
#endif
			g_caps.homogeneousDepth = false;
			g_caps.originBottomLeft = false;
			bx::memSet(g_caps.formats, 0, sizeof(g_caps.formats));
			for (uint32_t ff = 0; ff < TextureFormat::Count; ++ff)
			{
				uint32_t comp;
				const TextureFormat::Enum format = TextureFormat::Enum(ff);
				if (textureFormat(format, false, comp) == GX2_SURFACE_FORMAT_INVALID) continue;
				if (format == TextureFormat::D24F) { g_caps.formats[ff] = BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER; continue; }
				g_caps.formats[ff] = BGFX_CAPS_FORMAT_TEXTURE_2D | BGFX_CAPS_FORMAT_TEXTURE_3D | BGFX_CAPS_FORMAT_TEXTURE_CUBE | BGFX_CAPS_FORMAT_TEXTURE_VERTEX;
				if (!bimg::isCompressed(bimg::TextureFormat::Enum(format)))
				{
					g_caps.formats[ff] |= BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
					const uint32_t type = uint32_t(textureFormat(format, false, comp)) & 0x300;
					if (type != 0x100 && type != 0x300)
						g_caps.formats[ff] |= BGFX_CAPS_FORMAT_TEXTURE_MSAA | BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER_MSAA_X2 | BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER_MSAA_X4 | BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER_MSAA_X8;
					if (supportsMipAutogen(format)) g_caps.formats[ff] |= BGFX_CAPS_FORMAT_TEXTURE_MIP_AUTOGEN;
				}
				if (format == TextureFormat::R8 || format == TextureFormat::RG8 || format == TextureFormat::RGBA8
				|| format == TextureFormat::R16F || format == TextureFormat::R32F || format == TextureFormat::RGBA16F
				|| format == TextureFormat::RG16F || format == TextureFormat::RG32F
				|| format == TextureFormat::RGBA32F || format == TextureFormat::R32U || format == TextureFormat::RGBA32U)
					g_caps.formats[ff] |= BGFX_CAPS_FORMAT_TEXTURE_IMAGE_READ | BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE;
				if (format == TextureFormat::RGBA8 || format == TextureFormat::BGRA8 || (format >= TextureFormat::BC1 && format <= TextureFormat::BC3))
					g_caps.formats[ff] |= BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB | BGFX_CAPS_FORMAT_TEXTURE_3D_SRGB | BGFX_CAPS_FORMAT_TEXTURE_CUBE_SRGB;
			}
			g_caps.formats[TextureFormat::RGBA8] |= BGFX_CAPS_FORMAT_TEXTURE_BACKBUFFER;
			g_caps.limits.maxTextureSize = 8192;
			g_caps.limits.maxTextureLayers = 1365; // Cube views encode face ranges in 13 bits.
			g_caps.limits.maxFBAttachments = kColorTargets;
			g_caps.limits.maxVertexStreams = BGFX_CONFIG_MAX_VERTEX_STREAMS;
			g_caps.limits.maxVertexAttributes = 32;
			g_caps.limits.maxTextureSamplers = kTextureSamplers;
			g_caps.limits.maxComputeBindings = 16;
			g_caps.limits.maxBlits = BGFX_CONFIG_MAX_BLIT_ITEMS;
			g_caps.limits.blitRowPitchAlign = 1024;
			g_caps.limits.blitOffsetAlign = 256;
			g_caps.limits.maxOcclusionQueries = BGFX_CONFIG_MAX_OCCLUSION_QUERIES;
			g_caps.limits.maxInstanceData = 16;
		}

		~RendererContextGX2()
		{
			if (!m_suspended) suspend();
			GX2_PROFILE(finish());
			transferPublish(true);
			for (Block* block : m_allocations)
			{
				freeBlock(block);
				bx::deleteObject(g_allocator, block);
			}
			m_native.shutdown();
		}

		RendererType::Enum getRendererType() const override { return RendererType::Gx2; }
		const char* getRendererName() const override { return "WiiU"; }
		bool isDeviceRemoved() override { return false; }

		Block* allocate(uint32_t _size, uint32_t _alignment = 64)
		{
			Block* block = BX_NEW(g_allocator, Block);
			block->size = bx::alignUp(_size, 64);
			block->data = MEMAllocFromDefaultHeapEx(block->size, bx::max(64u, _alignment));
			m_allocations.push_back(block);
			return block;
		}

		void freeBlock(Block* _block)
		{
			if (_block->heap) MEMFreeToExpHeap(_block->heap, _block->data);
			else MEMFreeToDefaultHeap(_block->data);
		}

		Block* allocateFrameBuffer(uint32_t _size, uint32_t _alignment)
		{
			MEMHeapHandle heap = m_native.mem1;
			if (heap)
			{
				void* data = MEMAllocFromExpHeapEx(heap, bx::alignUp(_size, 64), bx::max(64u, _alignment));
				if (data)
				{
					Block* block = BX_NEW(g_allocator, Block);
					block->data = data; block->size = bx::alignUp(_size, 64); block->heap = heap;
					m_allocations.push_back(block);
					return block;
				}
			}
			return allocate(_size, _alignment);
		}

		void touch(Block* _block)
		{
			if (_block->stamp != kPending)
			{
				_block->stamp = kPending;
				m_touched.push_back(_block);
			}
		}

		void retire(Block*& _block)
		{
			if (NULL == _block) return;
			// Attach destruction after draws to the open epoch, never the previous seal.
			touch(_block);
			m_retired.push_back(_block);
			_block = NULL;
		}

		void collect()
		{
			GX2_PROFILE_SCOPE(Collect);
			const OSTime completed = GX2GetRetiredTimeStamp();
			for (size_t ii = 0; ii < m_retired.size();)
			{
				Block* block = m_retired[ii];
				if (block->stamp > completed) { ++ii; continue; }
				auto it = std::find(m_allocations.begin(), m_allocations.end(), block);
				m_allocations.erase(it);
				freeBlock(block);
				bx::deleteObject(g_allocator, block);
				m_retired.erase(m_retired.begin()+ii);
			}
		}

		OSTime seal()
		{
			const OSTime stamp = GX2GetLastSubmittedTimeStamp();
			for (Block* block : m_touched) block->stamp = stamp;
			m_touched.clear();
			for (SubmissionRing* ring : { &m_lists, &m_uploads })
				if (ring->head != ring->sealed)
				{
					ring->epochs.push_back({ ring->head, stamp });
					ring->sealed = ring->head;
				}
			return stamp;
		}

		void endFrame(bool _willPresent) override
		{
			GX2_PROFILE_COARSE(EndFrame);
			if (m_suspended) { GX2_PROFILE(end()); return; }
			transferPublish();
			const bool present = _willPresent && preparePresent();
			if (m_frameGpuTimer != UINT32_MAX)
			{
				endGpuTimer(m_frameGpuTimer); m_frameGpuTimer = UINT32_MAX;
			}
			GX2_PROFILE(end());
			if (!present && (!m_touched.empty() || m_lists.head != m_lists.sealed || m_uploads.head != m_uploads.sealed))
			{
				GX2Flush(); seal();
			}
		}

		void flip() override
		{
			GX2_PROFILE_COARSE(Flip);
			bool present = m_mainWindow.used;
			for (FrameBuffer& fb : m_frameBuffers) present |= fb.window.used;
			if (!present) return;
			// Interval zero must wait for GPU progress, never a vertical sync.
			// Bound pending swaps independently of how little command storage a frame uses.
			if (0 != GX2GetSwapInterval())
			{
				uint32_t swaps, flips;
				OSTime lastFlip, lastVsync;
				GX2GetSwapStatus(&swaps, &flips, &lastFlip, &lastVsync);
				if (swaps - flips >= 2) { GX2_PROFILE_SCOPE(FlipWait); GX2WaitForFlip(); }
			}
			else
			{
				const OSTime stamp = m_presentStamps[m_presentCursor];
				if (stamp > GX2GetRetiredTimeStamp())
					{ GX2_PROFILE_SCOPE(PresentGpuWait); while (!GX2WaitTimeStamp(stamp)) {} }
			}
			bool independentDrc = false;
			for (FrameBuffer& fb : m_frameBuffers)
				independentDrc |= fb.window.swapChain.nwh && ((const Gx2PlatformData*)fb.window.swapChain.nwh)->display == 1;
			presentWindow(m_mainWindow, independentDrc);
			for (FrameBuffer& fb : m_frameBuffers) presentWindow(fb.window, independentDrc);
			GX2SwapScanBuffers(); GX2Flush();
			GX2SetTVEnable(TRUE); GX2SetDRCEnable(TRUE);
			// Retirement includes all scan copies submitted by this frame.
			m_presentStamps[m_presentCursor] = seal();
			m_presentCursor = (m_presentCursor+1)%kQueuedFrames;
		}

		uint32_t acquire(SubmissionRing& _ring, uint32_t _size)
		{
			GX2_PROFILE_SCOPE_SELECT(&_ring == &m_lists ? Gx2Profiler::ListAcquire : Gx2Profiler::UniformAcquire);
			if (!_ring.block)
			{
				_ring.block = allocate(&_ring == &m_lists ? kListRingSize : kUniformRingSize, 256);
				GX2_PROFILE_ADD(ListAllocated, &_ring == &m_lists ? _ring.block->size : 0);
				GX2_PROFILE_ADD(UniformAllocated, &_ring == &m_uploads ? _ring.block->size : 0);
				_ring.epochs.reserve(BGFX_CONFIG_MAX_FRAME_LATENCY*2);
				// Display-list payloads are subsequently written only by write-gather.
				if (&_ring == &m_lists) DCFlushRange(_ring.block->data, _ring.block->size);
			}
			const uint32_t capacity = _ring.block->size;
			const uint32_t offset = uint32_t(_ring.head%capacity);
			const bool wrap = capacity-offset < _size;
			GX2_PROFILE_ADD(ListWrapBytes, &_ring == &m_lists && wrap ? capacity-offset : 0);
			const uint64_t start = _ring.head + (wrap ? capacity-offset : 0);
			if (start+_size > _ring.tail+capacity)
			{
				auto reclaim = [&]()
				{
					const OSTime completed = GX2GetRetiredTimeStamp();
					auto end = _ring.epochs.begin();
					while (end != _ring.epochs.end() && end->stamp <= completed)
					{
						_ring.tail = end->end;
						++end;
					}
					_ring.epochs.erase(_ring.epochs.begin(), end);
					// An empty ring can skip its unused tail without a GPU epoch.
					if (_ring.tail == _ring.head) _ring.tail = start;
				};
				reclaim();
				if (start+_size > _ring.tail+capacity)
				{
					// Publish queued ranges before waiting; never overwrite live bytes.
					GX2_PROFILE_ADD(ListPressureSubmits, &_ring == &m_lists);
					GX2Flush(); seal();
					do
					{
						const OSTime stamp = _ring.epochs.front().stamp;
						{ GX2_PROFILE_SCOPE_SELECT(&_ring == &m_lists ? Gx2Profiler::ListWait : Gx2Profiler::UniformWait); while (!GX2WaitTimeStamp(stamp)) {} }
						reclaim();
					} while (start+_size > _ring.tail+capacity);
				}
			}
			_ring.head = start;
			return wrap ? 0 : offset;
		}

		Block* reuse(std::vector<Block*>& _spare, uint32_t _size, uint32_t _alignment = 64, bool _gpuDestination = false)
		{
			const OSTime completed = GX2GetRetiredTimeStamp();
			for (auto it = _spare.begin(); it != _spare.end(); ++it)
				if ((*it)->stamp <= completed)
				{
					Block* block = *it;
					_spare.erase(it);
					return block;
				}
			Block* block = allocate(_size, _alignment);
			// GPU-only destinations never acquire CPU cache lines after allocation.
			if (_gpuDestination) DCInvalidateRange(block->data, block->size);
			return block;
		}


		// Raw views are at most 8192 x 8192 R8. Larger buffers use consecutive views.
		// Every allocation includes the native imageSize of its last view, rather
		// than describing padding that the allocation does not actually own.
		uint32_t gpuBufferCapacity(uint32_t _size)
		{
			const uint32_t chunk = GX2_MAX_TEXTURE_WIDTH*GX2_MAX_TEXTURE_HEIGHT;
			const uint32_t tail = (_size-1)%chunk+1;
			const uint32_t width = bx::min<uint32_t>(GX2_MAX_TEXTURE_WIDTH, bx::alignUp(tail, 256));
			const GX2Surface raw = transferLinear(GX2_SURFACE_FORMAT_UNORM_R8,
				width, (tail+width-1)/width);
			return _size-tail+raw.imageSize;
		}

		Block* gpuBufferAllocate(uint32_t _size, uint32_t _alignment = 256)
		{
			return allocate(gpuBufferCapacity(_size), bx::max(256u, _alignment));
		}

		void createTransferBuffer(Buffer& _buffer, uint32_t _size, uint16_t _flags)
		{
			_buffer.size = _size;
			_buffer.flags = _flags;
			m_needExportPrepass |= 0 != (_flags & BGFX_BUFFER_DRAW_INDIRECT);
			_buffer.gpuWritten = false;
			_buffer.littleEndian = false;
			_buffer.block = (_flags & (BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_DRAW_INDIRECT))
				? gpuBufferAllocate(_size) : allocate(_size);
			// COMPUTE_READ/WRITE are also the existing frontend's transfer eligibility
			// flags. Accepting them does not advertise compute shader execution.
		}

		// A CPU consumer alone transfers ownership back. There is no separate fence,
		// dirty-range registry, allocation owner or completion state.
		void bufferCpuRead(Buffer& _buffer)
		{
			if (!_buffer.gpuWritten) return;
			if (_buffer.block->stamp == kPending) { GX2Flush(); seal(); }
			if (_buffer.block->stamp > GX2GetRetiredTimeStamp())
				{ GX2_PROFILE_SCOPE(BufferReadWait); while (!GX2WaitTimeStamp(_buffer.block->stamp)) {} }
			DCInvalidateRange(_buffer.block->data, _buffer.block->size);
			_buffer.gpuWritten = false;
		}

		void update(Buffer& _buffer, uint32_t _offset, uint32_t _size, const void* _data,
			bool _vertex, bool _discard = false)
		{
			GX2_PROFILE_SCOPE_SELECT(_vertex ? Gx2Profiler::VertexUpload : Gx2Profiler::IndexUpload);
			_size = bx::min(_size, _buffer.size-_offset);
			GX2_PROFILE_ADD(VertexBytes, _vertex ? _size : 0);
			GX2_PROFILE_ADD(IndexBytes, _vertex ? 0 : _size);
			const bool preserve = !_discard && (_offset != 0 || _size != _buffer.size);
			// Preserving GPU-written bytes is an actual CPU read. Full overwrites and
			// explicit discard rename live storage without waiting for it.
			const bool littleEndian = preserve && _buffer.littleEndian;
			if (preserve) bufferCpuRead(_buffer);
			const bool rename = _buffer.block->stamp > GX2GetRetiredTimeStamp();
			if (rename)
			{
				GX2_PROFILE_ADD(BufferRenames, 1);
				Block* replacement = reuse(_buffer.spare, _buffer.block->size,
					(_buffer.flags & BGFX_BUFFER_COMPUTE_READ_WRITE) ? 256 : 64);
				if (preserve) GX2_PROFILE_ADD(PreserveBytes, _buffer.size);
				if (preserve) { GX2_PROFILE_SCOPE(BufferPreserve); bx::memCopy(replacement->data, _buffer.block->data, _buffer.size); }
				_buffer.spare.push_back(_buffer.block);
				_buffer.block = replacement;
			}
			_buffer.gpuWritten = false;
			_buffer.littleEndian = littleEndian;
			// A partial CPU upload into a native GPU result keeps its representation;
			// only the newly supplied native CPU words need endian conversion.
			{ GX2_PROFILE_SCOPE(BufferCopy); copyTextureRow((uint8_t*)_buffer.block->data+_offset, _data, _size,
				littleEndian ? (!_vertex && !(_buffer.flags & BGFX_BUFFER_INDEX32) ? 2 : 4) : 0); }
			// A preserved rename wrote the whole buffer; otherwise only this range was
			// CPU-written. CPU publication must never run on GPU-owned stale contents.
			const bool whole = rename && preserve;
			const GX2InvalidateMode consumer = GX2InvalidateMode((_vertex ? GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER : GX2_INVALIDATE_MODE_CPU)
				| (_buffer.flags & BGFX_BUFFER_COMPUTE_READ ? GX2_INVALIDATE_MODE_TEXTURE : 0));
			{ GX2_PROFILE_SCOPE(BufferPublish); GX2Invalidate(consumer,
				(uint8_t*)_buffer.block->data+(whole ? 0 : _offset), whole ? _buffer.size : _size); }
		}

		// Resource helpers run outside drawing; the next submit restores context.
		void readBuffer(Handle _handle, void* _data, uint32_t _offset, uint32_t _size) override
		{
			Buffer& buffer = transferBuffer(_handle);
			if (!buffer.gpuWritten)
			{
				bx::memCopy(_data, (const uint8_t*)buffer.block->data+_offset, _size);
				return;
			}
			// Preserve the explicit read's point-in-command-stream snapshot without
			// stalling this frame. Reuse the existing staging publication/deadline.
			gpuBufferSource(buffer);
			Block* block = gpuBufferAllocate(_size);
			DCInvalidateRange(block->data, block->size);
			gpuCopyBytes(buffer.block, _offset, block, 0, _size);
			GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, block->data, block->size);
			touch(block);
			TransferOutput output;
			output.block = block;
			output.destination = _data;
			output.deadline = m_transferFrame+1;
			output.rowBytes = _size; output.pitch = _size; output.rows = 1;
			m_transferOutputs.push_back(output);
		}

		void gpuBufferSource(Buffer& _buffer)
		{
			// CPU updates publish their written range; previous native writes publish
			// COLOR_BUFFER below. Invalidate this copy reader even after CPU updates
			// that previously only invalidated vertex/index attribute input.
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _buffer.block->data, _buffer.block->size);
			touch(_buffer.block);
		}

		void gpuBufferDestination(Buffer& _buffer)
		{
			// CPU-written ranges have already been flushed by update(). No CPU access
			// occurs between this invalidation and a real bufferCpuRead handoff.
			if (!_buffer.gpuWritten) DCInvalidateRange(_buffer.block->data, _buffer.block->size);
		}

		void gpuCopyWritten(Block* _block)
		{
			GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _block->data, _block->size);
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _block->data, _block->size);
			touch(_block);
		}

		void gpuBufferWritten(Buffer& _buffer)
		{
			gpuCopyWritten(_buffer.block);
			GX2Invalidate(GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, _buffer.block->data, _buffer.block->size);
			_buffer.gpuWritten = true;
		}

		GX2Surface gpuRawView(Block* _block, uint32_t _offset)
		{
			const uint32_t chunk = GX2_MAX_TEXTURE_WIDTH*GX2_MAX_TEXTURE_HEIGHT;
			const uint32_t base = _offset/chunk*chunk;
			const uint32_t bytes = bx::min(chunk, _block->size-base);
			const uint32_t width = bx::min<uint32_t>(GX2_MAX_TEXTURE_WIDTH, bytes);
			GX2Surface view = transferLinear(GX2_SURFACE_FORMAT_UNORM_R8, width, (bytes+width-1)/width);
			view.image = (uint8_t*)_block->data+base;
			return view;
		}

		// Both Blocks come from gpuBufferAllocate: the complete native R8 imageSize
		// is owned, including the final padded row. R8 copies carry individual bytes;
		// there is no 16/32-bit component endian conversion or CPU reinterpretation.
		void gpuCopyRows(Block* _src, uint32_t _srcOffset, uint32_t _srcPitch,
			Block* _dst, uint32_t _dstOffset, uint32_t _dstPitch, uint32_t _rowBytes, uint32_t _rows)
		{
			if (_srcPitch == _rowBytes && _dstPitch == _rowBytes)
			{
				_rowBytes *= _rows;
				_rows = 1;
			}
			const uint32_t chunk = GX2_MAX_TEXTURE_WIDTH*GX2_MAX_TEXTURE_HEIGHT;
			uint32_t row = 0, remaining = _rowBytes;
			uint32_t srcOffset = _srcOffset, dstOffset = _dstOffset;
			while (row < _rows)
			{
				GX2Surface src = gpuRawView(_src, srcOffset);
				GX2Surface dst = gpuRawView(_dst, dstOffset);
				const uint32_t srcBase = srcOffset/chunk, dstBase = dstOffset/chunk;
				const uint32_t srcPitch = GX2GetSurfaceMipPitch(&src, 0);
				const uint32_t dstPitch = GX2GetSurfaceMipPitch(&dst, 0);
				GX2Rect rects[GX2_MAX_COPY_SURFACE_EX_RECTS];
				GX2Point points[GX2_MAX_COPY_SURFACE_EX_RECTS];
				uint32_t count = 0;
				do
				{
					const uint32_t sx = (srcOffset%chunk)%srcPitch, sy = (srcOffset%chunk)/srcPitch;
					const uint32_t dx = (dstOffset%chunk)%dstPitch, dy = (dstOffset%chunk)/dstPitch;
					const uint32_t width = bx::min(remaining, srcPitch-sx, dstPitch-dx);
					uint32_t height = 1;
					if (_rows == 1 && sx == 0 && dx == 0 && srcPitch == dstPitch && width == srcPitch)
						height = bx::min(remaining/width, src.height-sy, dst.height-dy);
					rects[count] = { int32_t(sx), int32_t(sy), int32_t(sx+width), int32_t(sy+height) };
					points[count] = { int32_t(dx), int32_t(dy) };
					++count;
					const uint32_t bytes = width*height;
					srcOffset += bytes; dstOffset += bytes; remaining -= bytes;
					if (!remaining)
					{
						++row;
						srcOffset = _srcOffset+row*_srcPitch;
						dstOffset = _dstOffset+row*_dstPitch;
						remaining = _rowBytes;
					}
				}
				while (row < _rows && count < GX2_MAX_COPY_SURFACE_EX_RECTS
					&& srcOffset/chunk == srcBase && dstOffset/chunk == dstBase);
				GX2CopySurfaceEx(&src, 0, 0, &dst, 0, 0, count, rects, points);
			}
			touch(_src); touch(_dst);
		}

		void gpuCopyBytes(Block* _src, uint32_t _srcOffset, Block* _dst,
			uint32_t _dstOffset, uint32_t _size)
		{
			gpuCopyRows(_src, _srcOffset, _size, _dst, _dstOffset, _size, _size, 1);
		}

		// A native typed view is optional, not a fabricated descriptor for arbitrary
		// pitches/offsets. Native imageSize must fit actual physical capacity, even
		// where the frontend only requires the payload through the final row.
		bool gpuBufferView(Buffer& _buffer, GX2SurfaceFormat _nativeFormat,
			TextureFormat::Enum _format, uint32_t _offset, uint32_t _rowPitch,
			uint32_t _height, GX2Surface& _view)
		{
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(_format));
			// Packed depth pixels cannot be described by a native four-byte view.
			if (textureStorageBlockSize(_format) != info.blockSize) return false;
			if (_rowPitch%info.blockSize) return false;
			const uint32_t width = (_rowPitch/info.blockSize)*info.blockWidth;
			if (width > GX2_MAX_TEXTURE_WIDTH) return false;
			_view = transferLinear(_nativeFormat, width, _height);
			if (_offset%_view.alignment
			|| GX2GetSurfaceMipPitch(&_view, 0)*info.blockSize != _rowPitch
			|| _view.imageSize > _buffer.block->size-_offset) return false;
			_view.image = (uint8_t*)_buffer.block->data+_offset;
			return true;
		}

		void transferBufferToBuffer(const BlitItem& _blit)
		{
			Buffer& src = transferBuffer(_blit.m_src);
			Buffer& dst = transferBuffer(_blit.m_dst);
			// Frontend guarantees different physical buffer handles and valid ranges.
			gpuBufferSource(src);
			gpuBufferDestination(dst);
			gpuCopyBytes(src.block, _blit.m_srcOffset, dst.block, _blit.m_dstOffset, _blit.m_size);
			dst.littleEndian = src.littleEndian;
			gpuBufferWritten(dst);
		}

		void transferTextureToBuffer(const BlitItem& _blit)
		{
			Texture& src = m_textures[_blit.m_src.idx];
			Buffer& dst = transferBuffer(_blit.m_dst);
			const uint16_t count = bx::max<uint16_t>(1, _blit.m_depth);
			GX2Surface source = transferSource(src, _blit.m_srcMip, _blit.m_srcZ, count);
			const uint32_t mipWidth = bx::max(1u, source.width >> _blit.m_srcMip);
			const uint32_t mipHeight = bx::max(1u, source.height >> _blit.m_srcMip);
			const uint32_t width = bx::min<uint32_t>(_blit.m_width, mipWidth-_blit.m_srcX);
			const uint32_t height = bx::min<uint32_t>(_blit.m_height, mipHeight-_blit.m_srcY);
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(src.format));
			const uint32_t blockSize = textureStorageBlockSize(src.format);
			const bool bc = bimg::isCompressed(bimg::TextureFormat::Enum(src.format));
			if (bc && ((_blit.m_srcX | _blit.m_srcY) & 3))
			{
				unsupported("BC blit origin must address complete compressed blocks");
				return;
			}
			const bool edge = bc && ((width | height) & 3);
			const uint32_t rowBytes = calcTextureRegionPitch(src.format, width);
			const uint32_t rows = calcTextureRegionNumRows(src.format, height);
			gpuBufferDestination(dst);
			for (uint16_t slice = 0; slice < count; ++slice)
			{
				const uint32_t offset = _blit.m_dstOffset+slice*_blit.m_slicePitch;
				GX2Surface view;
				if (!edge && gpuBufferView(dst, source.format, src.format, offset, _blit.m_rowPitch, height, view))
					transferCopy(source, _blit.m_srcMip, _blit.m_srcZ+slice, _blit.m_srcX, _blit.m_srcY,
						view, 0, 0, 0, 0, width, height, src.format);
				else
				{
					// For tiny BC tails/non-multiple edges, full-mip CopySurface is legal
					// while an Ex rect is not. Crop whole encoded blocks using raw R8.
					GX2Surface staging = transferLinear(source.format, edge ? mipWidth : width, edge ? mipHeight : height);
					Block* block = gpuBufferAllocate(staging.imageSize, staging.alignment);
					DCInvalidateRange(block->data, block->size);
					staging.image = block->data;
					transferCopy(source, _blit.m_srcMip, _blit.m_srcZ+slice,
						edge ? 0 : _blit.m_srcX, edge ? 0 : _blit.m_srcY,
						staging, 0, 0, 0, 0, staging.width, staging.height, src.format);
					gpuCopyWritten(block);
					const uint32_t pitch = GX2GetSurfaceMipPitch(&staging, 0)*blockSize;
					const uint32_t start = edge
						? (_blit.m_srcY/info.blockHeight)*pitch+(_blit.m_srcX/info.blockWidth)*info.blockSize : 0;
					if (blockSize != info.blockSize)
					{
						// Strip X8 from each encoded depth pixel, entirely on the GPU.
						for (uint32_t row = 0; row < rows; ++row)
							gpuCopyRows(block, row*pitch, blockSize, dst.block, offset+row*_blit.m_rowPitch,
								info.blockSize, info.blockSize, width);
					}
					else gpuCopyRows(block, start, pitch, dst.block, offset, _blit.m_rowPitch, rowBytes, rows);
					retire(block);
				}
			}
			dst.littleEndian = true;
			gpuBufferWritten(dst);
		}

		void transferBufferToTexture(const BlitItem& _blit)
		{
			Buffer& src = transferBuffer(_blit.m_src);
			Texture& dst = m_textures[_blit.m_dst.idx];
			const uint16_t count = bx::max<uint16_t>(1, _blit.m_depth);
			GX2Surface destination = transferDestination(dst, _blit.m_dstMip, _blit.m_dstZ, count,
				{int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), _blit.m_width, _blit.m_height});
			const uint32_t mipWidth = bx::max(1u, destination.width >> _blit.m_dstMip);
			const uint32_t mipHeight = bx::max(1u, destination.height >> _blit.m_dstMip);
			const uint32_t width = bx::min<uint32_t>(_blit.m_width, mipWidth-_blit.m_dstX);
			const uint32_t height = bx::min<uint32_t>(_blit.m_height, mipHeight-_blit.m_dstY);
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(dst.format));
			const uint32_t blockSize = textureStorageBlockSize(dst.format);
			const bool bc = bimg::isCompressed(bimg::TextureFormat::Enum(dst.format));
			if (bc && ((_blit.m_dstX | _blit.m_dstY) & 3))
			{
				unsupported("BC blit origin must address complete compressed blocks");
				return;
			}
			const bool edge = bc && ((width | height) & 3);
			const uint32_t rowBytes = calcTextureRegionPitch(dst.format, width);
			const uint32_t rows = calcTextureRegionNumRows(dst.format, height);
			gpuBufferSource(src);
			for (uint16_t slice = 0; slice < count; ++slice)
			{
				const uint32_t offset = _blit.m_srcOffset+slice*_blit.m_slicePitch;
				GX2Surface view;
				if (!edge && gpuBufferView(src, destination.format, dst.format, offset, _blit.m_rowPitch, height, view))
					transferCopy(view, 0, 0, 0, 0, destination, _blit.m_dstMip,
						_blit.m_dstZ+slice, _blit.m_dstX, _blit.m_dstY, width, height, dst.format);
				else
				{
					GX2Surface staging = transferLinear(destination.format, edge ? mipWidth : width, edge ? mipHeight : height);
					Block* block = gpuBufferAllocate(staging.imageSize, staging.alignment);
					DCInvalidateRange(block->data, block->size);
					staging.image = block->data;
					const uint32_t pitch = GX2GetSurfaceMipPitch(&staging, 0)*blockSize;
					const uint32_t start = edge
						? (_blit.m_dstY/info.blockHeight)*pitch+(_blit.m_dstX/info.blockWidth)*info.blockSize : 0;
					if (edge && (_blit.m_dstX || _blit.m_dstY || width != mipWidth || height != mipHeight))
					{
						// Preserve blocks outside an edge region, patch raw bytes on GPU,
						// then copy the entire matching mip back through CopySurface.
						transferCopy(destination, _blit.m_dstMip, _blit.m_dstZ+slice, 0, 0,
							staging, 0, 0, 0, 0, mipWidth, mipHeight, dst.format);
						gpuCopyWritten(block);
					}
					if (blockSize != info.blockSize)
					{
						// X8 is not part of the API's D24/D24F payload.
						for (uint32_t row = 0; row < rows; ++row)
							gpuCopyRows(src.block, offset+row*_blit.m_rowPitch, info.blockSize,
								block, row*pitch, blockSize, info.blockSize, width);
					}
					else gpuCopyRows(src.block, offset, _blit.m_rowPitch, block, start, pitch, rowBytes, rows);
					gpuCopyWritten(block);
					transferCopy(staging, 0, 0, 0, 0, destination, _blit.m_dstMip, _blit.m_dstZ+slice,
						edge ? 0 : _blit.m_dstX, edge ? 0 : _blit.m_dstY,
						staging.width, staging.height, dst.format);
					retire(block);
				}
			}
			transferWritten(dst, _blit.m_dstMip, _blit.m_dstZ, count);
		}

		void transferBlit(const BlitItem& _blit)
		{
			if (_blit.m_src.isBuffer() && _blit.m_dst.isBuffer()) transferBufferToBuffer(_blit);
			else if (_blit.m_src.isBuffer()) transferBufferToTexture(_blit);
			else if (_blit.m_dst.isBuffer()) transferTextureToBuffer(_blit);
			else transferTextureBlit(_blit);
		}

		void destroyBuffer(Buffer& _buffer)
		{
			retire(_buffer.block);
			for (Block*& block : _buffer.spare) retire(block);
			_buffer.spare.clear();
		}

		void createIndexBuffer(IndexBufferHandle _handle, const Memory* _mem, uint16_t _flags) override
		{
			createDynamicIndexBuffer(_handle, _mem->size, _flags);
			update(m_indexBuffers[_handle.idx], 0, _mem->size, _mem->data, false);
		}
		void createDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _size, uint16_t _flags) override
		{
			Buffer& buffer = m_indexBuffers[_handle.idx];
			createTransferBuffer(buffer, _size, _flags);
		}
		void updateDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _offset, uint32_t _size, const Memory* _mem) override
		{ update(m_indexBuffers[_handle.idx], _offset, bx::min(_size, _mem->size), _mem->data, false); }
		void destroyIndexBuffer(IndexBufferHandle _handle) override { destroyBuffer(m_indexBuffers[_handle.idx]); }
		void destroyDynamicIndexBuffer(IndexBufferHandle _handle) override { destroyIndexBuffer(_handle); }
		void createVertexLayout(VertexLayoutHandle _handle, const VertexLayout& _layout) override { m_layouts[_handle.idx] = _layout; }
		void destroyVertexLayout(VertexLayoutHandle) override {}
		void createVertexBuffer(VertexBufferHandle _handle, const Memory* _mem, VertexLayoutHandle _layout, uint16_t _flags) override
		{
			createDynamicVertexBuffer(_handle, _mem->size, _flags);
			m_vertexBuffers[_handle.idx].layout = _layout;
			update(m_vertexBuffers[_handle.idx], 0, _mem->size, _mem->data, true);
		}
		void createDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _size, uint16_t _flags) override
		{
			Buffer& buffer = m_vertexBuffers[_handle.idx];
			createTransferBuffer(buffer, _size, _flags);
		}
		void updateDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _offset, uint32_t _size, const Memory* _mem) override
		{ update(m_vertexBuffers[_handle.idx], _offset, bx::min(_size, _mem->size), _mem->data, true); }
		void destroyVertexBuffer(VertexBufferHandle _handle) override { destroyBuffer(m_vertexBuffers[_handle.idx]); }
		void destroyDynamicVertexBuffer(VertexBufferHandle _handle) override { destroyVertexBuffer(_handle); }

		uint32_t computeGfdSize(const void* _gfd, GFDBlockType _type, uint32_t _bytes)
		{
			const uint8_t* file = (const uint8_t*)_gfd;
			for (uint32_t offset = ((const GFDHeader*)file)->headerSize; offset < _bytes;)
			{
				const GFDBlockHeader* block = (const GFDBlockHeader*)(file+offset);
				if (block->type == _type) return block->dataSize;
				offset += block->headerSize+block->dataSize;
			}
			return 0;
		}

		void loadComputeShader(Shader& _shader, const void* _gfd, uint32_t _bytes)
		{
			const uint8_t* file = (const uint8_t*)_gfd;
			const GFDHeader* header = (const GFDHeader*)file;
			for (uint32_t offset = header->headerSize; offset < _bytes;)
			{
				const GFDBlockHeader* block = (const GFDBlockHeader*)(file+offset);
				const uint8_t* data = file+offset+block->headerSize;
				if (block->type == GFD_BLOCK_COMPUTE_SHADER_HEADER)
				{
					uint8_t* dst = (uint8_t*)_shader.header->data;
					bx::memCopy(dst, data, block->dataSize);
					const GFDRelocationHeader* reloc = (const GFDRelocationHeader*)(data+block->dataSize-sizeof(GFDRelocationHeader));
					const uint32_t* patches = (const uint32_t*)(data+(reloc->patchOffset & ~GFD_PATCH_MASK));
					for (uint32_t ii = 0; ii < reloc->patchCount; ++ii)
					{
						if (!patches[ii]) continue;
						uint32_t* target = (uint32_t*)(dst+(patches[ii] & ~GFD_PATCH_MASK));
						*target = uint32_t(uintptr_t(dst+(*target & ~GFD_PATCH_MASK)));
					}
				}
				else if (block->type == GFD_BLOCK_COMPUTE_SHADER_PROGRAM)
					bx::memCopy(_shader.program->data, data, block->dataSize);
				offset += block->headerSize+block->dataSize;
			}
			((GX2ComputeShader*)_shader.header->data)->program = _shader.program->data;
		}

		void createShader(ShaderHandle _handle, const Memory* _mem) override
		{ loadShader(m_shaders[_handle.idx], _mem->data); }

		void loadShader(Shader& shader, const uint8_t* _data, bool _internal = false)
		{
			const uint8_t* ptr = _data;
			const uint32_t magic = readLE(ptr, 4);
			shader.fragment = isShaderType(magic, 'F');
			shader.compute = isShaderType(magic, 'C');
			readLE(ptr, 4); readLE(ptr, 4); // interface hashes
			shader.rawSrvMask = readLE(ptr, 4);
			readLE(ptr, 4); // Writable bindings are described by the resource layout below.
			const uint16_t count = uint16_t(readLE(ptr, 2));
			for (uint32_t ii = 0; ii < count; ++ii)
			{
				const uint8_t length = uint8_t(readLE(ptr, 1));
				char name[256];
				bx::memCopy(name, ptr, length); name[length] = '\0'; ptr += length;
				const uint8_t type = uint8_t(readLE(ptr, 1)) & ~kUniformMask;
				const uint8_t num = uint8_t(readLE(ptr, 1));
				const uint16_t offset = uint16_t(readLE(ptr, 2));
				const uint16_t rows = uint16_t(readLE(ptr, 2));
				readLE(ptr, 1); const uint8_t dimension = uint8_t(idToTextureDimension(uint8_t(readLE(ptr, 1))))+1; readLE(ptr, 2);
				if (type == UniformType::Sampler)
				{
					for (uint32_t unit = offset; unit < uint32_t(offset)+num; ++unit) shader.samplerDimensions[unit] = dimension;
					continue;
				}
				shader.uniformSize = bx::max(shader.uniformSize, (uint32_t(offset)+uint32_t(rows)*4)*4);
				const PredefinedUniform::Enum predefined = nameToPredefinedUniformEnum(name);
				if (predefined != PredefinedUniform::Count)
				{
					PredefinedUniform binding;
					binding.m_loc = offset; binding.m_count = rows;
					binding.m_type = uint8_t(predefined) | (shader.fragment ? kUniformFragmentBit : 0);
					shader.predefined.push_back(binding);
				}
				else if (!_internal)
				{
					const UniformRegInfo* info = m_uniformReg.find(name);
					BGFX_FATAL(NULL != info, Fatal::InvalidShader, "GX2 uniform %s is not registered.", name);
					Uniform uniform = { info->m_handle, offset, rows, type, num };
					shader.uniforms.push_back(uniform);
				}
			}
			uint32_t gfdSize = readLE(ptr, 4);
			if (shader.compute || gfdSize == UINT32_C(0x31495347))
			{
				BGFX_FATAL(gfdSize == (shader.compute ? UINT32_C(0x31435347) : UINT32_C(0x31495347)), Fatal::InvalidShader, "Invalid GX2 resource ABI.");
				const uint32_t resources = readLE(ptr, 4);
				for (uint32_t ii = 0; ii < resources; ++ii)
				{
					ComputeResource resource;
					resource.binding = readLE(ptr, 4); resource.kind = readLE(ptr, 4);
					resource.format = readLE(ptr, 4); resource.elementBytes = readLE(ptr, 4);
					resource.read = readLE(ptr, 4); resource.write = readLE(ptr, 4);
					shader.resources.push_back(resource);
				}
				gfdSize = readLE(ptr, 4);
			}
			// The envelope is byte-packed; GFD reads native words and needs alignment.
			void* gfd = bx::alloc(g_allocator, gfdSize, 4);
			bx::memCopy(gfd, ptr, gfdSize);
			const uint32_t headerSize = shader.compute ? computeGfdSize(gfd, GFD_BLOCK_COMPUTE_SHADER_HEADER, gfdSize) : shader.fragment ? GFDGetPixelShaderHeaderSize(0, gfd) : GFDGetVertexShaderHeaderSize(0, gfd);
			const uint32_t programSize = shader.compute ? computeGfdSize(gfd, GFD_BLOCK_COMPUTE_SHADER_PROGRAM, gfdSize) : shader.fragment ? GFDGetPixelShaderProgramSize(0, gfd) : GFDGetVertexShaderProgramSize(0, gfd);
			shader.header = allocate(headerSize);
			shader.program = allocate(programSize, GX2_SHADER_PROGRAM_ALIGNMENT);
			BOOL loaded;
			GX2UniformBlock* block;
			if (shader.compute)
			{
				loadComputeShader(shader, gfd, gfdSize);
				const GX2ComputeShader* native = (const GX2ComputeShader*)shader.header->data;
				block = GX2GetComputeUniformBlock(native, "__cafe_loose_uniforms");
				loaded = TRUE;
			}
			else if (shader.fragment)
			{
				GX2PixelShader* native = (GX2PixelShader*)shader.header->data;
				loaded = GFDGetPixelShader(native, shader.program->data, 0, gfd);
				block = loaded ? GX2GetPixelUniformBlock(native, "__cafe_loose_uniforms") : NULL;
			}
			else
			{
				GX2VertexShader* native = (GX2VertexShader*)shader.header->data;
				loaded = GFDGetVertexShader(native, shader.program->data, 0, gfd);
				block = loaded ? GX2GetVertexUniformBlock(native, "__cafe_loose_uniforms") : NULL;
			}
			BGFX_FATAL(loaded, Fatal::InvalidShader, "GX2 GFD loader: %s", GFDGetLastErrorString());
			if (shader.fragment)
			{
				auto& regs = ((GX2PixelShader*)shader.header->data)->regs;
				for (const ComputeResource& resource : shader.resources) if (resource.write)
				{
					// Image side effects survive unused exports; late tests also survive Hi-Z rejection.
					regs.db_shader_control |= 1u << 11; // EXEC_ON_NOOP.
					if (!(regs.db_shader_control & 0x30)) regs.db_shader_control |= 1u << 10; // EXEC_ON_HIER_FAIL.
					break;
				}
				shader.sampleFrequency = 0 != (regs.spi_ps_in_control_1 & 0x01000000) // SampleID / sample mask.
					|| 0 != (regs.spi_ps_in_control_0 & 0x40000000); // Sample-qualified position.
				for (uint32_t input = 0; input < regs.num_spi_ps_input_cntl; ++input)
					shader.sampleFrequency |= 0 != (regs.spi_ps_input_cntls[input] & 0x00040000);
			}
			if (block) shader.uniformSize = block->size;
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			if (!shader.compute)
			{
				const GX2VertexShader* vertex = (const GX2VertexShader*)shader.header->data;
				const GX2PixelShader* fragment = (const GX2PixelShader*)shader.header->data;
				const uint32_t count = shader.fragment ? fragment->initialValueCount : vertex->initialValueCount;
				const GX2UniformInitialValue* initial = shader.fragment ? fragment->initialValues : vertex->initialValues;
				for (uint32_t ii = 0; ii < count; ++ii)
					shader.uniformSize = bx::max(shader.uniformSize, (initial[ii].offset+4)*4);
			}
#endif
			if (shader.uniformSize > 65536) unsupported("uniform blocks larger than 64 KiB");
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.program->data, programSize);
			if (!shader.fragment && !shader.compute && GFDGetVertexShaderCount(gfd) > 1)
			{
				const uint32_t size = GFDGetVertexShaderProgramSize(1, gfd);
				shader.primitiveHeader = allocate(GFDGetVertexShaderHeaderSize(1, gfd));
				shader.primitiveProgram = allocate(size, GX2_SHADER_PROGRAM_ALIGNMENT);
				loaded = GFDGetVertexShader((GX2VertexShader*)shader.primitiveHeader->data, shader.primitiveProgram->data, 1, gfd);
				BGFX_FATAL(loaded, Fatal::InvalidShader, "GX2 primitive-ID GFD loader: %s", GFDGetLastErrorString());
				GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.primitiveProgram->data, size);
			}
			bx::free(g_allocator, gfd, 4);
		}

		void destroyShader(ShaderHandle _handle) override
		{
			Shader& shader = m_shaders[_handle.idx];
			retire(shader.program); retire(shader.header);
			retire(shader.primitiveProgram); retire(shader.primitiveHeader);
			shader.uniforms.clear(); shader.predefined.clear(); shader.resources.clear(); shader.uniformSize = 0; shader.rawSrvMask = 0;
			shader.compute = shader.fragment = false;
			bx::memSet(shader.samplerDimensions, 0, sizeof(shader.samplerDimensions));
		}

		void createProgram(ProgramHandle _handle, ShaderHandle _vsh, ShaderHandle _fsh) override
		{
			Program& program = m_programs[_handle.idx];
			if (!isValid(_fsh))
			{
				program.compute = &m_shaders[_vsh.idx];
				m_needExportPrepass = true;
				program.m_predefined = program.compute->predefined;
				program.m_numPredefined = uint16_t(program.m_predefined.size());
				return;
			}
			program.vertex = &m_shaders[_vsh.idx]; program.fragment = &m_shaders[_fsh.idx];
			m_needExportPrepass |= !program.vertex->resources.empty() || !program.fragment->resources.empty();
			program.samplerMask = (program.vertex->rawSrvMask | program.fragment->rawSrvMask) & ((1u << kTextureSamplers)-1);
			for (uint32_t unit = 0; unit < kTextureSamplers; ++unit)
				if (program.vertex->samplerDimensions[unit] || program.fragment->samplerDimensions[unit]) program.samplerMask |= 1u << unit;
			const GX2PixelShader* fragment = (const GX2PixelShader*)program.fragment->header->data;
			for (uint32_t ii = 0; ii < fragment->regs.num_spi_ps_input_cntl; ++ii)
				if ((fragment->regs.spi_ps_input_cntls[ii] & 0xff) == 0xc9) program.primitiveId = true; // CafeGLSL PrimitiveID semantic.
			BGFX_FATAL(!program.primitiveId || program.vertex->primitiveHeader, Fatal::InvalidShader,
				"GX2 PrimitiveID requires a vertex shader rebuilt with both native variants.");
			program.m_predefined = program.vertex->predefined;
			program.m_predefined.insert(program.m_predefined.end(), program.fragment->predefined.begin(), program.fragment->predefined.end());
			program.m_numPredefined = uint16_t(program.m_predefined.size());
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			// This one matrix overwrites every word of both stages' uniform storage.
			program.mvpOnly = program.m_numPredefined == 1
				&& program.m_predefined[0].m_type == PredefinedUniform::ModelViewProj
				&& program.m_predefined[0].m_loc == 0 && program.m_predefined[0].m_count == 4
				&& program.vertex->uniformSize == 64 && program.fragment->uniformSize == 0;
#endif
		}

		void destroyProgram(ProgramHandle _handle) override
		{
			Program& program = m_programs[_handle.idx];
			if (m_boundProgram == &program) m_boundProgram = NULL;
			for (Fetch& fetch : program.fetches) retire(fetch.program);
			program.fetches.clear(); program.vertex = NULL; program.fragment = NULL; program.compute = NULL;
			program.primitiveId = false; program.mvpOnly = false; program.samplerMask = 0;
			program.m_predefined.clear(); program.m_numPredefined = 0;
		}

		void textureStorage(Texture& _texture, Block* _block)
		{
			GX2Surface& surface = _texture.texture.surface;
			surface.image = _block->data;
			surface.mipmaps = surface.mipmapSize ? (uint8_t*)_block->data+_texture.mipOffset : NULL;
			GX2InitTextureRegs(&_texture.texture);
		}

		void allocateTexture(Texture& _texture)
		{
			GX2Surface& surface = _texture.texture.surface;
			GX2CalcSurfaceSizeAndAlignment(&surface);
			_texture.mipOffset = surface.mipmapSize ? surface.mipLevelOffset[0] : surface.imageSize;
			_texture.image = allocate(_texture.mipOffset+surface.mipmapSize, surface.alignment);
			DCInvalidateRange(_texture.image->data, _texture.image->size);
			textureStorage(_texture, _texture.image);
		}

		TextureStage& textureStage(Texture& _texture, uint32_t _mip, uint32_t _slice)
		{
			GX2_PROFILE_SCOPE(StageReuse);
			TextureLevel& level = _texture.levels[_mip];
			TextureStage& stage = level.slices[_slice];
			if (!stage.block) { stage.block = allocate(level.source.imageSize, level.source.alignment); GX2_PROFILE_ADD(StageAllocated, stage.block->size); }
			else if (stage.block->stamp > GX2GetRetiredTimeStamp())
			{
				GX2_PROFILE_ADD(StageRenames, 1);
				Block* replacement = reuse(stage.spare, level.source.imageSize, level.source.alignment);
				stage.spare.push_back(stage.block); stage.block = replacement;
			}
			return stage;
		}

		void stageTexture(Texture& _texture, uint32_t _mip, uint32_t _slice, const Rect& _rect, const void* _data, uint32_t _pitch, bool _littleEndian = BX_CPU_ENDIAN_LITTLE)
		{
			GX2_PROFILE_SCOPE(TextureStage);
			TextureLevel& level = _texture.levels[_mip];
			TextureStage& stage = textureStage(_texture, _mip, _slice);
			const bimg::TextureFormat::Enum requested = bimg::TextureFormat::Enum(_texture.requestedFormat);
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(_texture.format));
			const bimg::ImageBlockInfo& sourceInfo = bimg::getBlockInfo(requested);
			const uint32_t blockSize = textureStorageBlockSize(_texture.format);
			const uint32_t blocksX = bx::max<uint32_t>(sourceInfo.minBlockX, (_rect.m_width+sourceInfo.blockWidth-1)/sourceInfo.blockWidth);
			const uint32_t rows = bx::max<uint32_t>(sourceInfo.minBlockY, (_rect.m_height+sourceInfo.blockHeight-1)/sourceInfo.blockHeight);
			const uint32_t rowBytes = blocksX*sourceInfo.blockSize;
			const uint32_t pitch = _pitch == UINT32_MAX ? rowBytes : _pitch;
			const uint32_t offset = (_rect.m_y/info.blockHeight*level.source.pitch+_rect.m_x/info.blockWidth)*blockSize;
			uint8_t* dst = (uint8_t*)stage.block->data+offset;
			const uint32_t dstPitch = level.source.pitch*blockSize;
			const uint8_t wordSize = bimg::getPixelWordSize(requested);
			Rect rect = _rect;
			if (blockSize != info.blockSize)
			{
				// Keep all 24 encoded bits, normalizing native CPU words to GX2 LE.
				for (uint32_t yy = 0; yy < rows; ++yy)
					for (uint32_t xx = 0; xx < blocksX; ++xx)
					{
						uint8_t* pixel = dst+yy*dstPitch+xx*blockSize;
						copyTextureRow(pixel, (const uint8_t*)_data+yy*pitch+xx*sourceInfo.blockSize,
							sourceInfo.blockSize, _littleEndian ? 0 : wordSize);
						pixel[3] = 0;
					}
			}
			else if (_texture.format == _texture.requestedFormat)
			{
				// GX2 consumes little-endian words; byte channels and encoded blocks copy verbatim.
				for (uint32_t yy = 0; yy < rows; ++yy)
					copyTextureRow(dst+yy*dstPitch, (const uint8_t*)_data+yy*pitch, rowBytes, _littleEndian ? 0 : wordSize);
			}
			else
			{
				// Compressed updates may include a complete edge block beyond the logical mip.
				rect.m_width = uint16_t(bx::min<uint32_t>(rect.m_width, level.source.width-rect.m_x));
				rect.m_height = uint16_t(bx::min<uint32_t>(rect.m_height, level.source.height-rect.m_y));
				const bool compressed = bimg::isCompressed(requested);
				const bool swap = wordSize > 1 && _littleEndian != bool(BX_CPU_ENDIAN_LITTLE);
				const uint8_t* src = (const uint8_t*)_data;
				uint8_t* packed = NULL;
				uint32_t srcPitch = pitch;
				// Block decoders require tight rows; CPU scalar conversion requires native words.
				if (swap || (compressed && pitch != rowBytes))
				{
					packed = (uint8_t*)bx::alloc(g_allocator, rowBytes*rows);
					for (uint32_t yy = 0; yy < rows; ++yy)
						copyTextureRow(packed+yy*rowBytes, src+yy*pitch, rowBytes, swap ? wordSize : 0);
					src = packed; srcPitch = rowBytes;
				}
				if (compressed)
				{
					const uint32_t width = blocksX*sourceInfo.blockWidth;
					const uint32_t height = rows*sourceInfo.blockHeight;
					// Small mips and edge blocks decode whole blocks, then crop to the changed rectangle.
					if (width != rect.m_width || height != rect.m_height)
					{
						uint8_t* decoded = (uint8_t*)bx::alloc(g_allocator, width*height*4);
						bimg::imageDecodeToBgra8(g_allocator, decoded, src, width, height, width*4, requested);
						bx::memCopy(dst, dstPitch, decoded, width*4, rect.m_width*4, rect.m_height);
						bx::free(g_allocator, decoded);
					}
					else
						bimg::imageDecodeToBgra8(g_allocator, dst, src, width, height, dstPitch, requested);
				}
				else
					bimg::imageConvert(g_allocator, dst, bimg::TextureFormat::BGRA8, src, requested, rect.m_width, rect.m_height, 1, srcPitch, dstPitch);
				bx::free(g_allocator, packed);
			}
			Rect merged = rect;
			for (size_t ii = 0; ii < stage.dirty.size();)
			{
				const Rect& old = stage.dirty[ii];
				Rect overlap = old; overlap.intersect(merged);
				const uint16_t left = bx::min(old.m_x, merged.m_x), top = bx::min(old.m_y, merged.m_y);
				const uint16_t right = bx::max<uint16_t>(old.m_x+old.m_width, merged.m_x+merged.m_width);
				const uint16_t bottom = bx::max<uint16_t>(old.m_y+old.m_height, merged.m_y+merged.m_height);
				// Merge only an entirely covered rectangle; a CPU shadow cannot fill GPU-owned gaps.
				if (uint32_t(right-left)*(bottom-top) == uint32_t(old.m_width)*old.m_height + uint32_t(merged.m_width)*merged.m_height - uint32_t(overlap.m_width)*overlap.m_height)
				{
					merged = {int16_t(left), int16_t(top), uint16_t(right-left), uint16_t(bottom-top)};
					stage.dirty.erase(stage.dirty.begin()+ii); ii = 0;
				}
				else ++ii;
			}
			stage.dirty.push_back(merged);
			if (!_texture.pending) m_pendingTextures.push_back(&_texture);
			_texture.pending = true;
		}

		bool separateRenderStorage(const Texture& _texture)
		{
			return _texture.renderImage || (_texture.external
				&& (_texture.renderSurface.image != _texture.texture.surface.image
					|| _texture.renderSurface.mipmaps != _texture.texture.surface.mipmaps));
		}

		void invalidateTexture(Texture& _texture, GX2InvalidateMode _mode, bool _render = false)
		{
			Block* block = _render && _texture.renderImage ? _texture.renderImage : _texture.image;
			if (block) GX2Invalidate(_mode, block->data, block->size);
			else
			{
				// Borrowed GX2 textures may keep mip storage in a separate allocation.
				const GX2Surface& surface = _render ? _texture.renderSurface : _texture.texture.surface;
				GX2Invalidate(_mode, surface.image, surface.imageSize);
				if (surface.mipmapSize) GX2Invalidate(_mode, surface.mipmaps, surface.mipmapSize);
			}
		}

		void uploadTextures()
		{
			GX2_PROFILE_SCOPE(TextureUpload);
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
			const int profileUpload = m_pendingTextures.empty() ? -1 : s_gx2Profiler.beginUpload();
#endif
			bool published = false;
			for (Texture* entry : m_pendingTextures)
			{
				Texture& texture = *entry;
				if (!texture.pending) continue;
				const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(texture.format));
				const uint32_t blockSize = textureStorageBlockSize(texture.format);
				for (TextureLevel& level : texture.levels)
					for (TextureStage& stage : level.slices)
					{
						for (const Rect& rect : stage.dirty)
						{
							const uint32_t offset = (rect.m_y/info.blockHeight*level.source.pitch+rect.m_x/info.blockWidth)*blockSize;
							const uint32_t rows = (rect.m_height+info.blockHeight-1)/info.blockHeight;
							const uint32_t rowBytes = (rect.m_width+info.blockWidth-1)/info.blockWidth*blockSize;
							const uint32_t pitch = level.source.pitch*blockSize;
							uint8_t* data = (uint8_t*)stage.block->data+offset;
							// Sub-cache-line gaps cannot contain a cache line worth skipping.
							if (pitch-rowBytes < BX_CACHE_LINE_SIZE)
								{ GX2_PROFILE_SCOPE(TexturePublish); DCStoreRangeNoSync(data, (rows-1)*pitch+rowBytes); }
							else
								for (uint32_t row = 0; row < rows; ++row)
									{ GX2_PROFILE_SCOPE(TexturePublish); DCStoreRangeNoSync(data+row*pitch, rowBytes); }
							published = true;
						}
					}
			}
			if (!published) { m_pendingTextures.clear(); return; }
			OSMemoryBarrier();
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, NULL, UINT32_MAX);
			for (Texture* entry : m_pendingTextures)
			{
				Texture& texture = *entry;
				if (!texture.pending) continue;
				resolveTexture(texture);
				// Native writes cannot rebuild separate Hi-Z metadata on shared depth storage.
				if (texture.hiZ && !texture.renderImage) retire(texture.hiZ);
				// Only the CPU-written staging needs renaming. Destination writes are
				// ordered GPU copies after earlier readers; keep untouched subresources
				// in place instead of copying them into another complete texture.
				for (uint32_t mip = 0; mip < texture.levels.size(); ++mip)
				{
					TextureLevel& level = texture.levels[mip];
					for (uint32_t slice = 0; slice < level.slices.size(); ++slice)
					{
						TextureStage& stage = level.slices[slice];
						if (stage.dirty.empty()) continue;
						level.source.image = stage.block->data;
						const Rect& first = stage.dirty.front();
						if (stage.dirty.size() == 1 && !first.m_x && !first.m_y && first.m_width == level.source.width && first.m_height == level.source.height)
							GX2CopySurface(&level.source, 0, 0, &texture.texture.surface, mip, slice);
						else for (size_t start = 0; start < stage.dirty.size(); start += GX2_MAX_COPY_SURFACE_EX_RECTS)
						{
							GX2Rect source[GX2_MAX_COPY_SURFACE_EX_RECTS]; GX2Point dest[GX2_MAX_COPY_SURFACE_EX_RECTS];
							const uint32_t count = bx::min<size_t>(GX2_MAX_COPY_SURFACE_EX_RECTS, stage.dirty.size()-start);
							for (uint32_t rr = 0; rr < count; ++rr)
							{
								const Rect& rect = stage.dirty[start+rr];
								source[rr] = {rect.m_x, rect.m_y, rect.m_x+rect.m_width, rect.m_y+rect.m_height}; dest[rr] = {rect.m_x, rect.m_y};
							}
							GX2CopySurfaceEx(&level.source, 0, 0, &texture.texture.surface, mip, slice, count, source, dest);
						}
						touch(stage.block); stage.dirty.clear(); stage.renderDirty = separateRenderStorage(texture);
					}
				}
				invalidateTexture(texture, GX2_INVALIDATE_MODE_COLOR_BUFFER);
				if (texture.image) touch(texture.image); texture.pending = false; texture.arrayDirty = true;
			}
			// Every copy producer is published above. Ordinary sampling starts only
			// after this group; invalidate its input cache once for the whole group.
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, NULL, UINT32_MAX);
			m_pendingTextures.clear();
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
			s_gx2Profiler.endUpload(profileUpload);
#endif
		}

		void textureLevels(Texture& _texture)
		{
			const GX2Surface& surface = _texture.texture.surface;
			_texture.levels.resize(surface.mipLevels);
			for (uint32_t mip = 0; mip < surface.mipLevels; ++mip)
			{
				TextureLevel& level = _texture.levels[mip];
				level.source = surface;
				level.source.dim = GX2_SURFACE_DIM_TEXTURE_2D;
				level.source.width = bx::max(1u, surface.width >> mip); level.source.height = bx::max(1u, surface.height >> mip);
				level.source.depth = 1; level.source.mipLevels = 1; level.source.aa = GX2_AA_MODE1X;
				level.source.use = GX2_SURFACE_USE_TEXTURE;
				level.source.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED; level.source.swizzle = 0;
				GX2CalcSurfaceSizeAndAlignment(&level.source);
				level.slices.resize(surface.dim == GX2_SURFACE_DIM_TEXTURE_3D ? bx::max(1u, surface.depth >> mip) : surface.depth);
			}
		}

		void* createTexture(TextureHandle _handle, const Memory* _mem, uint64_t _flags, uint8_t _skip, uint64_t _external) override
		{
			bimg::ImageContainer image;
			if (!bimg::imageParse(image, _mem->data, _mem->size)) return NULL;
			Texture& texture = m_textures[_handle.idx];
			texture.flags = _flags; texture.requestedFormat = TextureFormat::Enum(image.m_format);
			texture.format = bimg::isDepth(image.m_format) ? texture.requestedFormat : getViableTextureFormat(image);
			if (texture.format == TextureFormat::D24F && !(_flags & BGFX_TEXTURE_RT_WRITE_ONLY)) { unsupported("sampled D24F encoding"); return NULL; }
			GX2Surface& surface = texture.texture.surface;
			surface = {};
			surface.format = textureFormat(texture.format, 0 != (_flags & BGFX_TEXTURE_SRGB), texture.texture.compMap);
			if (surface.format == GX2_SURFACE_FORMAT_INVALID) { unsupported("texture format"); return NULL; }
			_skip = bx::min<uint8_t>(_skip, image.m_numMips-1);
			surface.width = bx::max(1u, image.m_width >> _skip); surface.height = bx::max(1u, image.m_height >> _skip);
			surface.depth = image.m_cubeMap ? image.m_numLayers*6 : image.m_depth > 1 ? bx::max(1u, image.m_depth >> _skip) : image.m_numLayers;
			surface.dim = image.m_cubeMap ? GX2_SURFACE_DIM_TEXTURE_CUBE : image.m_depth > 1 ? GX2_SURFACE_DIM_TEXTURE_3D
				: image.m_numLayers > 1 ? GX2_SURFACE_DIM_TEXTURE_2D_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D;
			surface.mipLevels = image.m_numMips-_skip;
			surface.aa = GX2_AA_MODE1X;
			surface.use = GX2SurfaceUse(GX2_SURFACE_USE_TEXTURE | ((_flags & BGFX_TEXTURE_RT_MASK)
				? (bimg::isDepth(bimg::TextureFormat::Enum(texture.format)) ? GX2_SURFACE_USE_DEPTH_BUFFER : GX2_SURFACE_USE_COLOR_BUFFER) : 0));
			surface.tileMode = GX2_TILE_MODE_DEFAULT;
			texture.texture.viewFirstMip = 0; texture.texture.viewNumMips = surface.mipLevels;
			texture.texture.viewFirstSlice = 0; texture.texture.viewNumSlices = surface.depth;
			if (_external)
			{
				texture.external = (_external & 1) ? (const Gx2ExternalTexture*)(uintptr_t)(_external & ~UINT64_C(1)) : NULL;
				texture.texture = texture.external ? *texture.external->texture : *(const GX2Texture*)(uintptr_t)_external;
				GX2InitTextureRegs(&texture.texture);
				texture.renderSurface = texture.external && texture.external->color ? texture.external->color->surface
					: texture.external && texture.external->depth ? texture.external->depth->surface : texture.texture.surface;
				if (texture.external && texture.external->depth)
				{
					texture.depthClear = texture.external->depth->depthClear;
					texture.stencilClear = texture.external->depth->stencilClear;
				}
				textureLevels(texture);
				if (texture.external && (_flags & BGFX_TEXTURE_RT_MASK))
				{
					for (uint16_t mip = 0; mip < texture.renderSurface.mipLevels; ++mip)
						markRendered(texture, {mip, 0, uint16_t(texture.levels[mip].slices.size()), false, 0 != (uint32_t(texture.renderSurface.format) & 0x400)});
				}
				else if ((_flags & BGFX_TEXTURE_RT_MASK) && (texture.texture.surface.aa != GX2_AA_MODE1X || texture.format == TextureFormat::D24 || texture.format == TextureFormat::D24S8 || texture.format == TextureFormat::D32FS8))
					unsupported("external render target requires Gx2ExternalTexture metadata");
				return NULL;
			}
			const uint32_t nativeType = uint32_t(surface.format) & 0x300;
			if ((_flags & BGFX_TEXTURE_RT_MSAA_MASK) && (nativeType == 0x100 || nativeType == 0x300))
			{ unsupported("integer color MSAA"); return NULL; }
			const uint32_t samples = uint32_t((_flags & BGFX_TEXTURE_RT_MSAA_MASK) >> BGFX_TEXTURE_RT_MSAA_SHIFT);
			if (samples > 4) { unsupported("16x MSAA"); return NULL; }
			const GX2AAMode aa = GX2AAMode(samples > 1 ? samples-1 : 0);
			const bool depth = bimg::isDepth(bimg::TextureFormat::Enum(texture.format));
			const bool separateDepth = depth && !(_flags & BGFX_TEXTURE_RT_WRITE_ONLY) && (texture.format == TextureFormat::D24 || texture.format == TextureFormat::D24S8 || texture.format == TextureFormat::D24F || texture.format == TextureFormat::D32FS8 || aa != GX2_AA_MODE1X);
			GX2Surface render = surface;
			if ((_flags & BGFX_TEXTURE_RT_MASK) && (aa != GX2_AA_MODE1X || separateDepth))
			{
				render.aa = aa;
				if (aa != GX2_AA_MODE1X)
				{
					render.mipLevels = 1;
					render.dim = render.depth > 1 ? GX2_SURFACE_DIM_TEXTURE_2D_MSAA_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D_MSAA;
					if (_flags & BGFX_TEXTURE_MSAA_SAMPLE) surface = render;
				}
				if (depth) surface.use = GX2_SURFACE_USE_TEXTURE;
				if (_flags & BGFX_TEXTURE_RT_WRITE_ONLY) surface = render;
			}
			texture.texture.viewNumMips = surface.mipLevels;
			allocateTexture(texture); textureLevels(texture);
			texture.renderSurface = surface;
			if ((_flags & BGFX_TEXTURE_RT_MASK) && !(_flags & BGFX_TEXTURE_RT_WRITE_ONLY)
			&& (separateDepth || (aa != GX2_AA_MODE1X && !(_flags & BGFX_TEXTURE_MSAA_SAMPLE))))
			{
				texture.renderSurface = render;
				GX2CalcSurfaceSizeAndAlignment(&texture.renderSurface);
				const uint32_t offset = texture.renderSurface.mipmapSize ? texture.renderSurface.mipLevelOffset[0] : texture.renderSurface.imageSize;
				texture.renderImage = allocate(offset+texture.renderSurface.mipmapSize, texture.renderSurface.alignment);
				DCInvalidateRange(texture.renderImage->data, texture.renderImage->size);
				texture.renderSurface.image = texture.renderImage->data;
				texture.renderSurface.mipmaps = texture.renderSurface.mipmapSize ? (uint8_t*)texture.renderImage->data+offset : NULL;
			}
			if ((_flags & BGFX_TEXTURE_RT_MASK) && aa != GX2_AA_MODE1X && !depth)
			{
				GX2ColorBuffer color = {}; color.surface = texture.renderSurface; color.viewNumSlices = color.surface.depth;
				uint32_t bytes, alignment; GX2CalcColorBufferAuxInfo(&color, &bytes, &alignment);
				texture.auxiliary = allocate(bytes, alignment);
				bx::memSet(texture.auxiliary->data, GX2_AUX_BUFFER_CLEAR_VALUE, bytes);
				DCFlushRange(texture.auxiliary->data, bytes);
			}
			if ((_flags & BGFX_TEXTURE_RT_MASK) && depth && texture.renderSurface.mipLevels == 1 && texture.renderSurface.depth == 1)
			{
				GX2DepthBuffer buffer = {}; buffer.surface = texture.renderSurface; buffer.viewNumSlices = buffer.surface.depth;
				uint32_t bytes, alignment; GX2CalcDepthBufferHiZInfo(&buffer, &bytes, &alignment);
				if (bytes) { texture.hiZ = allocate(bytes, alignment); DCInvalidateRange(texture.hiZ->data, bytes); }
			}
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(texture.requestedFormat));
			const bool littleEndian = image.m_offset == UINT32_MAX ? bool(BX_CPU_ENDIAN_LITTLE)
				: !image.m_ktx || bx::toLittleEndian(bx::loadUnaligned<uint32_t>(_mem->data+12)) == 0x04030201;
			for (uint32_t mip = 0; mip < surface.mipLevels; ++mip)
			{
				const Rect rect = {0, 0, uint16_t(bx::max(1u, surface.width >> mip)), uint16_t(bx::max(1u, surface.height >> mip))};
				for (uint32_t slice = 0; slice < texture.levels[mip].slices.size(); ++slice)
				{
					bimg::ImageMip raw;
					const bool volume = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D;
					if (bimg::imageGetRawData(image, volume ? 0 : slice, mip+_skip, _mem->data, _mem->size, raw))
					{
						const uint32_t rowBytes = raw.m_width/info.blockWidth*info.blockSize;
						const uint32_t sliceBytes = rowBytes*(raw.m_height/info.blockHeight);
						stageTexture(texture, mip, slice, rect, raw.m_data+(volume ? slice*sliceBytes : 0), rowBytes, littleEndian);
					}
				}
			}
			return NULL;
		}

		void updateTexture(TextureHandle _handle, uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem) override
		{
			Texture& texture = m_textures[_handle.idx];
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(texture.requestedFormat));
			const uint32_t rowBytes = bx::max<uint32_t>(info.minBlockX, (_rect.m_width+info.blockWidth-1)/info.blockWidth)*info.blockSize;
			const uint32_t pitch = _pitch == UINT16_MAX ? rowBytes : _pitch;
			const uint32_t sliceBytes = pitch*bx::max<uint32_t>(info.minBlockY, (_rect.m_height+info.blockHeight-1)/info.blockHeight);
			for (uint32_t slice = 0; slice < _depth; ++slice)
				stageTexture(texture, _mip, (texture.texture.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE ? _z*6+_side : _z)+slice, _rect, _mem->data+slice*sliceBytes, pitch);
		}

		void destroyTexture(TextureHandle _handle) override
		{
			Texture& texture = m_textures[_handle.idx];
			if (texture.pending) m_pendingTextures.erase(std::remove(m_pendingTextures.begin(), m_pendingTextures.end(), &texture), m_pendingTextures.end());
			retire(texture.image); retire(texture.renderImage); retire(texture.auxiliary); retire(texture.hiZ);
			retire(texture.depthSamplesImage); retire(texture.resolvedDepthImage); retire(texture.arrayImage);
			for (Block*& block : texture.spareArrayImages) retire(block);
			for (Block*& block : texture.spareImages) retire(block);
			for (TextureLevel& level : texture.levels)
				for (TextureStage& stage : level.slices)
				{
					retire(stage.block);
					for (Block*& block : stage.spare) retire(block);
				}
			texture = {};
		}
		uintptr_t getInternal(TextureHandle _handle) override { return (uintptr_t)&m_textures[_handle.idx].texture; }
		void clearTexture(TextureHandle _handle, uint8_t _mip, uint8_t _numMips, uint16_t _layer, uint16_t _numLayers) override
		{
			Texture& texture = m_textures[_handle.idx];
			const GX2Surface& surface = texture.texture.surface;
			const uint32_t end = bx::min<uint32_t>(surface.mipLevels, uint32_t(_mip)+_numMips);
			const bool volume = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D;
			const bool render = 0 != (texture.flags & BGFX_TEXTURE_RT_MASK);
			if (render) uploadTextures();
			for (uint32_t mip = _mip; mip < end; ++mip)
			{
				TextureLevel& level = texture.levels[mip];
				const uint32_t first = volume ? 0 : _layer;
				const uint32_t count = volume ? level.slices.size() : bx::min<uint32_t>(_numLayers, level.slices.size()-first);
				if (render)
				{
					const RenderView view = {uint16_t(mip), uint16_t(first), uint16_t(count)};
					GX2ColorBuffer target = colorBuffer(texture, view);
					GX2ClearColor(&target, 0, 0, 0, 0);
					for (uint32_t slice = first; slice < first+count; ++slice) level.slices[slice].renderDirty = false;
					if (!(texture.flags & BGFX_TEXTURE_RT_WRITE_ONLY) || texture.renderSurface.aa != GX2_AA_MODE1X) markRendered(texture, view);
				}
				else
				{
					const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(texture.format));
					const uint32_t blockSize = textureStorageBlockSize(texture.format);
					const uint32_t rowBytes = ((level.source.width+info.blockWidth-1)/info.blockWidth)*blockSize;
					const uint32_t rows = (level.source.height+info.blockHeight-1)/info.blockHeight;
					for (uint32_t slice = first; slice < first+count; ++slice)
					{
						TextureStage& stage = textureStage(texture, mip, slice);
						for (uint32_t row = 0; row < rows; ++row)
						{
							uint8_t* dst = (uint8_t*)stage.block->data+row*level.source.pitch*blockSize;
							bx::memSet(dst, 0, rowBytes);
						}
						stage.dirty.assign(1, {0, 0, uint16_t(level.source.width), uint16_t(level.source.height)});
					}
				}
			}
			if (render)
			{
				invalidateTexture(texture, GX2_INVALIDATE_MODE_COLOR_BUFFER, true);
				Block* block = texture.renderImage ? texture.renderImage : texture.image;
				if (block) touch(block);
				if (texture.auxiliary) touch(texture.auxiliary);
			}
			else
			{
				if (!texture.pending) m_pendingTextures.push_back(&texture);
				texture.pending = true;
			}
		}
		void resizeTexture(TextureHandle _handle, uint16_t _width, uint16_t _height, uint8_t _numMips, uint16_t _numLayers) override
		{
			const Texture& texture = m_textures[_handle.idx];
			const uint64_t flags = texture.flags;
			TextureCreate create = {}; create.m_format = texture.requestedFormat; create.m_width = _width; create.m_height = _height;
			create.m_numMips = _numMips; create.m_numLayers = _numLayers;
			uint8_t data[sizeof(uint32_t)+sizeof(create)]; bx::StaticMemoryBlockWriter writer(data, sizeof(data));
			bx::write(&writer, kChunkMagicTex, bx::ErrorAssert{}); bx::write(&writer, create, bx::ErrorAssert{});
			const Memory mem = {data, sizeof(data)}; destroyTexture(_handle); createTexture(_handle, &mem, flags, 0, 0);
		}
		void createFrameBuffer(FrameBufferHandle _handle, uint8_t _num, const Attachment* _attachments) override
		{ m_frameBuffers[_handle.idx].attachments.assign(_attachments, _attachments+_num); }
		void createFrameBuffer(FrameBufferHandle _handle, const SwapChain& _swapChain) override
		{ m_frameBuffers[_handle.idx].window.swapChain = _swapChain; }
		void destroyFrameBuffer(FrameBufferHandle _handle) override
		{
			FrameBuffer& fb = m_frameBuffers[_handle.idx];
			destroyWindow(fb.window); fb.attachments.clear();
		}
		void invalidateOcclusionQuery(OcclusionQueryHandle _handle) override
		{
			// A reused bgfx handle must never receive an old allocation's result.
			for (OcclusionSample& sample : m_pendingOcclusion)
				if (sample.handle.idx == _handle.idx) sample.handle = BGFX_INVALID_HANDLE;
		}

		void resolveOcclusion(Frame* _render)
		{
			GX2_PROFILE_SCOPE(OcclusionResolve);
			const OSTime completed = GX2GetRetiredTimeStamp();
			size_t consumed = 0;
			for (; consumed < m_pendingOcclusion.size(); ++consumed)
			{
				const OcclusionSample& sample = m_pendingOcclusion[consumed];
				if (sample.block->stamp > completed) break;
				if (isValid(sample.handle))
				{
					// Imported query calls can touch their payload on the CPU. Discard
					// cached initialization only after the GPU writer has completed.
					DCInvalidateRange(sample.block->data, sample.block->size);
					uint64_t pixels;
					if (!GX2QueryGetOcclusionResult((const GX2QueryData*)sample.block->data, &pixels)) break;
					// bgfx's INT32_MIN is NoResult; retain zero and positive counts.
					_render->m_occlusion[sample.handle.idx] = int32_t(bx::min<uint64_t>(pixels, INT32_MAX));
					GX2_PROFILE_ADD(OcclusionResults, 1);
				}
				m_freeQueryBlocks.push_back(sample.block);
			}
			m_pendingOcclusion.erase(m_pendingOcclusion.begin(), m_pendingOcclusion.begin() + consumed);
		}

		bool isVisible(Frame* _render, OcclusionQueryHandle _handle, bool _visible)
		{
			// Use the submit-start result snapshot, as in Vulkan/WebGPU.
			// INT32_MIN (NoResult) is nonzero, so a positive condition draws initially.
			return _visible == (0 != _render->m_occlusion[_handle.idx]);
		}

		Block* beginOcclusion(OcclusionQueryHandle _handle)
		{
			// reuse() allocates when no retired spare exists; acquire() can wait.
			Block* block = reuse(m_freeQueryBlocks, sizeof(GX2QueryData), 64, true);
			touch(block);
			GX2QueryBegin(GX2_QUERY_TYPE_OCCLUSION_CPU, (GX2QueryData*)block->data);
			m_pendingOcclusion.push_back({ block, _handle });
			return block;
		}

		void endOcclusion(Block* _block)
		{
			GX2QueryEnd(GX2_QUERY_TYPE_OCCLUSION_CPU, (GX2QueryData*)_block->data);
			// The end must belong to the final epoch even if a batch split sealed begin.
			touch(_block);
		}

		uint32_t beginGpuTimer(uint16_t _view, uint32_t _frameNum)
		{
			Block* block = reuse(m_freeTimerBlocks, 128, 64, true);
			// Each native sample publishes its sentinel in a separate CPU cache line.
			GX2SampleBottomGPUCycle((uint64_t*)block->data);
			touch(block);
			const uint32_t index = uint32_t(m_pendingGpuTimers.size());
			m_pendingGpuTimers.push_back({ block, _frameNum, _view });
			return index;
		}

		void endGpuTimer(uint32_t _index)
		{
			GpuTimerSample& sample = m_pendingGpuTimers[_index];
			GX2SampleBottomGPUCycle((uint64_t*)((uint8_t*)sample.block->data+64));
			// A batch may have sealed begin already. Retirement must cover end too.
			touch(sample.block);
		}

		void resolveGpuTimers(Frame* _render)
		{
			GX2_PROFILE_SCOPE(ResolveTimers);
			const OSTime completed = GX2GetRetiredTimeStamp();
			size_t consumed = 0;
			for (; consumed < m_pendingGpuTimers.size(); ++consumed)
			{
				const GpuTimerSample& sample = m_pendingGpuTimers[consumed];
				// All intervals are closed before the next submit's resolver. The owner
				// is touched at both ends, including across batch splits/seals.
				if (sample.block->stamp > completed) break;
				DCInvalidateRange(sample.block->data, sample.block->size);
				// GX2's cycle API exposes native u64 values, unlike raw query regs.
				const uint64_t begin = *(const uint64_t*)sample.block->data;
				const uint64_t end = *(const uint64_t*)((const uint8_t*)sample.block->data+64);
				if (begin != GX2_GPU_TIMESTAMP_INVALID && end != GX2_GPU_TIMESTAMP_INVALID)
				{
					GX2_PROFILE(gpu(sample.frameNum, sample.view, end-begin));
					GpuTimerResult& result = m_gpuTimerResults[sample.view];
					result.begin = int64_t(begin);
					result.end = int64_t(end);
					result.frameNum = sample.frameNum;
					if (sample.view == BGFX_CONFIG_MAX_VIEWS)
						m_maxGpuLatency = bx::max(m_maxGpuLatency, _render->m_frameNum - sample.frameNum);
				}
				// An invalid retired timestamp cannot become valid later: keep the
				// last valid stats, and recycle without a readiness spin or a wait.
				m_freeTimerBlocks.push_back(sample.block);
			}
			m_pendingGpuTimers.erase(m_pendingGpuTimers.begin(), m_pendingGpuTimers.begin() + consumed);
			const GpuTimerResult& result = m_gpuTimerResults[BGFX_CONFIG_MAX_VIEWS];
			Stats& stats = _render->m_perfStats;
			stats.gpuTimeBegin = result.begin;
			stats.gpuTimeEnd = result.end;
			stats.gpuFrameNum = result.frameNum;
			stats.gpuTimerFreq = GX2_GPU_TIMESTAMP_FREQUENCY;
			stats.maxGpuLatency = m_maxGpuLatency;
		}

		uint32_t beginViewTimer(Frame* _render, uint16_t _view)
		{
			if (0 == (_render->m_debug & BGFX_DEBUG_PROFILER))
			{
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
				return beginGpuTimer(_view, _render->m_frameNum);
#else
				return UINT32_MAX;
#endif
			}
			ViewStats& stats = _render->m_perfStats.viewStats[_render->m_perfStats.numViews];
			stats.view = ViewId(_view);
			bx::strCopy(stats.name, BGFX_CONFIG_MAX_VIEW_NAME, getViewName(_render, _view));
			stats.cpuTimeBegin = bx::getHPCounter();
			return beginGpuTimer(_view, _render->m_frameNum);
		}

		void endViewTimer(Frame* _render, uint32_t _index)
		{
			if (_index == UINT32_MAX) return;
			endGpuTimer(_index);
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
			if (0 == (_render->m_debug & BGFX_DEBUG_PROFILER)) return;
#endif
			ViewStats& stats = _render->m_perfStats.viewStats[_render->m_perfStats.numViews++];
			stats.cpuTimeEnd = bx::getHPCounter();
			const GpuTimerResult& result = m_gpuTimerResults[stats.view];
			stats.gpuTimeBegin = result.begin;
			stats.gpuTimeEnd = result.end;
			stats.gpuFrameNum = result.frameNum;
		}
		void setMarker(const char* _marker, uint16_t _len) override
		{
			GX2DebugTagUserString(GX2_DEBUG_TAG_TYPE_BOOKMARK, "%.*s", int(_len), _marker);
		}
		void setName(Handle, const char*, uint16_t) override {}

		GX2ColorBuffer colorBuffer(Texture& _texture, const RenderView& _view, bool _srgb = false)
		{
			GX2ColorBuffer color = _texture.external && _texture.external->color ? *_texture.external->color : GX2ColorBuffer{};
			color.surface = _texture.external || _texture.renderImage ? _texture.renderSurface : _texture.texture.surface;
			if (_texture.flags & BGFX_TEXTURE_SRGB_MUTABLE) { uint32_t comp; color.surface.format = textureFormat(_texture.format, _srgb, comp); }
			color.viewMip = _view.mip; color.viewFirstSlice = _view.layer; color.viewNumSlices = _view.count;
			if (_texture.auxiliary) { color.aaBuffer = _texture.auxiliary->data; color.aaSize = _texture.auxiliary->size; }
			GX2InitColorBufferRegs(&color); return color;
		}

		GX2DepthBuffer depthBuffer(Texture& _texture, const RenderView& _view)
		{
			GX2DepthBuffer depth = _texture.external && _texture.external->depth ? *_texture.external->depth : GX2DepthBuffer{};
			depth.surface = _texture.external || _texture.renderImage ? _texture.renderSurface : _texture.texture.surface;
			if (depth.surface.depth > 1 || depth.surface.mipLevels > 1 || !separateRenderStorage(_texture))
			{ depth.hiZPtr = NULL; depth.hiZSize = 0; }
			depth.depthClear = _texture.depthClear; depth.stencilClear = _texture.stencilClear;
			depth.viewMip = _view.mip; depth.viewFirstSlice = _view.layer; depth.viewNumSlices = _view.count;
			if (_texture.hiZ) { depth.hiZPtr = _texture.hiZ->data; depth.hiZSize = _texture.hiZ->size; }
			GX2InitDepthBufferRegs(&depth);
			GX2InitDepthBufferHiZEnable(&depth, depth.hiZPtr != NULL); return depth;
		}

		// Cube mip chains pad faces differently from ordinary arrays. A native copy
		// supplies a real array layout when a shader requests face-array sampling.
		const GX2Texture& cubeArrayView(Texture& _texture)
		{
			if (!_texture.arrayImage)
			{
				_texture.arrayView = _texture.texture;
				GX2Surface& surface = _texture.arrayView.surface;
				surface.dim = GX2_SURFACE_DIM_TEXTURE_2D_ARRAY;
				surface.use = GX2_SURFACE_USE_TEXTURE; surface.swizzle = 0; surface.tileMode = GX2_TILE_MODE_DEFAULT;
				_texture.arrayImage = surfaceStorage(surface); GX2InitTextureRegs(&_texture.arrayView);
			}
			if (_texture.arrayDirty)
			{
				GX2Surface& surface = _texture.arrayView.surface;
				if (_texture.arrayImage->stamp > GX2GetRetiredTimeStamp())
				{
					Block* old = _texture.arrayImage;
					const uint32_t offset = surface.mipmapSize ? surface.mipLevelOffset[0] : surface.imageSize;
					_texture.arrayImage = reuse(_texture.spareArrayImages, old->size, surface.alignment, true);
					surface.image = _texture.arrayImage->data;
					surface.mipmaps = surface.mipmapSize ? (uint8_t*)surface.image+offset : NULL;
					_texture.spareArrayImages.push_back(old);
					GX2InitTextureRegs(&_texture.arrayView);
				}
				for (uint32_t mip = 0; mip < surface.mipLevels; ++mip)
					for (uint32_t slice = 0; slice < surface.depth; ++slice)
						GX2CopySurface(&_texture.texture.surface, mip, slice, &surface, mip, slice);
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _texture.arrayImage->data, _texture.arrayImage->size);
				GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _texture.arrayImage->data, _texture.arrayImage->size);
				_texture.arrayDirty = false;
				if (_texture.image) touch(_texture.image);
			}
			touch(_texture.arrayImage); return _texture.arrayView;
		}

		bool restoreRenderTexture(Texture& _texture, const RenderView& _view)
		{
			if (!separateRenderStorage(_texture)) return false;
			bool dirty = false;
			for (uint32_t slice = _view.layer; slice < uint32_t(_view.layer)+_view.count; ++slice) dirty |= _texture.levels[_view.mip].slices[slice].renderDirty;
			if (!dirty) return false;
			bool started = false;
			const GX2Surface& surface = _texture.texture.surface;
			const bool volume = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D;
			const bool array = surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE || surface.dim == GX2_SURFACE_DIM_TEXTURE_2D_ARRAY;
			const bool depth = bimg::isDepth(bimg::TextureFormat::Enum(_texture.format));
			const GX2Texture& sampled = surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE ? cubeArrayView(_texture) : _texture.texture;
			const bool stencil = _texture.format == TextureFormat::D24S8 || _texture.format == TextureFormat::D32FS8;
			for (uint32_t slice = _view.layer; slice < uint32_t(_view.layer)+_view.count; ++slice)
			{
				TextureStage& stage = _texture.levels[_view.mip].slices[slice];
				if (!stage.renderDirty) continue;
				if (!started) { restore(); beginBatch(); started = true; }
				GX2Texture source = sampled;
				source.viewFirstMip = _view.mip; source.viewNumMips = 1;
				if (!volume)
				{
					source.surface.dim = array ? GX2_SURFACE_DIM_TEXTURE_2D_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D;
					source.viewFirstSlice = slice; source.viewNumSlices = 1;
				}
				source.compMap = GX2_COMP_MAP(0, 1, 2, 3); GX2InitTextureRegs(&source);
				GX2Texture stencilSource = source;
				stencilSource.surface.format = _texture.format == TextureFormat::D32FS8 ? GX2_SURFACE_FORMAT_UINT_G8_X24 : GX2_SURFACE_FORMAT_UINT_X24_G8;
				stencilSource.compMap = GX2_COMP_MAP(1, 4, 4, 5); GX2InitTextureRegs(&stencilSource);
				const uint32_t width = bx::max(1u, surface.width >> _view.mip), height = bx::max(1u, surface.height >> _view.mip);
				const float params[4] = {1.0f/width, 1.0f/height, float(slice), stencil ? 1.0f : 0.0f};
				const uint32_t type = uint32_t(surface.format) & 0x300;
				const uint32_t family = depth ? 3 : type == 0x100 ? 6 : type == 0x300 ? 9 : 0;
				Program& program = m_programs[m_mipGen->m_program[family+(volume ? 2 : array ? 1 : 0)].idx];
				const RenderView single = {_view.mip, uint16_t(slice), 1};
				if (depth)
				{
					GX2DepthBuffer target = depthBuffer(_texture, single);
					fullscreen(program, source, NULL, &target, width, height, params, stencil ? &stencilSource : NULL);
				}
				else
				{
					GX2ColorBuffer target = colorBuffer(_texture, single, 0 != (_texture.flags & BGFX_TEXTURE_SRGB));
					fullscreen(program, source, &target, NULL, width, height, params);
				}
				stage.renderDirty = false;
			}
			if (started)
			{
				endBatch();
				invalidateTexture(_texture, depth ? GX2_INVALIDATE_MODE_DEPTH_BUFFER : GX2_INVALIDATE_MODE_COLOR_BUFFER, true);
				if (_texture.image) touch(_texture.image);
				if (_texture.renderImage) touch(_texture.renderImage);
				if (_texture.auxiliary) touch(_texture.auxiliary);
				if (_texture.hiZ) touch(_texture.hiZ);
			}
			return started;
		}

		void bindFrameBuffer(FrameBufferHandle _handle)
		{
			GX2_PROFILE_SCOPE(FrameBufferBuild);
			Window* window = windowFor(_handle);
			if (window)
			{
				prepareWindow(*window);
			}
			bool restored = false;
			if (isValid(_handle))
				for (const Attachment& attachment : m_frameBuffers[_handle.idx].attachments)
					restored |= restoreRenderTexture(m_textures[attachment.handle.idx], {attachment.mip, attachment.layer, attachment.numLayers});
			if (restored) restore();
			m_currentFrameBuffer = _handle;
			m_numColors = 0; m_hasDepth = false; m_depthReadOnly = false; m_stencilReadOnly = false;
			if (window)
			{
				m_colors[0] = windowColor(*window); m_numColors = 1;
				if (window->swapChain.flags & BGFX_SWAP_CHAIN_SRGB_BACKBUFFER)
				{
					m_colors[0].surface.format = GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8;
					GX2InitColorBufferRegs(&m_colors[0]);
				}
				if (window->colorImage) touch(window->colorImage);
				if (window->auxiliary) touch(window->auxiliary);
				if (isValid(window->swapChain.depth))
				{
					Texture& texture = m_textures[window->swapChain.depth.idx];
					if (restoreRenderTexture(texture, {0, 0, 1})) restore();
					m_activeDepth = depthBuffer(texture, {0, 0, 1}); m_hasDepth = true;
					if (texture.image) touch(texture.image);
					if (texture.renderImage) touch(texture.renderImage);
					if (texture.hiZ) touch(texture.hiZ);
				}
				else
				{
					m_activeDepth = window->depth; m_hasDepth = window->depthImage != NULL;
					if (window->depthImage) touch(window->depthImage);
					if (window->hiZ) touch(window->hiZ);
				}
				m_targetWidth = bx::max(1u, m_colors[0].surface.width >> m_colors[0].viewMip);
				m_targetHeight = bx::max(1u, m_colors[0].surface.height >> m_colors[0].viewMip);
			}
			else
			{
				for (const Attachment& attachment : m_frameBuffers[_handle.idx].attachments)
				{
					Texture& texture = m_textures[attachment.handle.idx];
					const RenderView view = {attachment.mip, attachment.layer, attachment.numLayers};
					const bool depth = bimg::isDepth(bimg::TextureFormat::Enum(texture.format));
					if (depth)
					{
						m_activeDepth = depthBuffer(texture, view); m_hasDepth = true;
						m_depthReadOnly = 0 != (attachment.flags & BGFX_ATTACHMENT_READ_ONLY_DEPTH);
						m_stencilReadOnly = 0 != (attachment.flags & BGFX_ATTACHMENT_READ_ONLY_STENCIL);
					}
					else
					{
						GX2ColorBuffer& color = m_colors[m_numColors++];
						color = colorBuffer(texture, view, 0 != (attachment.flags & BGFX_ATTACHMENT_SRGB));
						// GX2's native formats have fixed storage order. Route logical exports
						// only on ordinary attachments; helper copies use storage order.
						uint32_t swap;
						switch (texture.format)
						{
						case TextureFormat::BGRA8: case TextureFormat::BGRA4: case TextureFormat::BGR5A1: swap = 1; break;
						case TextureFormat::B5G6R5: swap = 2; break;
						case TextureFormat::A8: swap = 3; break;
						default: swap = (color.regs[1] >> 16) & 3; break;
						}
						// WUT GX2ColorBuffer ABI: regs[1] is CB_COLOR_INFO, COMP_SWAP[17:16].
						color.regs[1] = (color.regs[1] & ~(3u << 16)) | (swap << 16);
					}
					m_targetWidth = bx::max(1u, texture.texture.surface.width >> attachment.mip);
					m_targetHeight = bx::max(1u, texture.texture.surface.height >> attachment.mip);
					if (texture.image) touch(texture.image);
					if (texture.renderImage) touch(texture.renderImage);
					if (texture.auxiliary) touch(texture.auxiliary);
					if (texture.hiZ) touch(texture.hiZ);
				}
			}
			for (uint32_t ii = 0; ii < m_numColors; ++ii) GX2SetColorBuffer(&m_colors[ii], GX2RenderTarget(ii));
			if (m_hasDepth)
			{
				GX2SetDepthBuffer(&m_activeDepth);
				if (m_activeDepth.hiZPtr) GX2SetClearDepthStencil(&m_activeDepth, m_activeDepth.depthClear, m_activeDepth.stencilClear);
			}
			GX2SetAAMode(m_numColors ? m_colors[0].surface.aa : m_activeDepth.surface.aa);
			m_cachedState = UINT64_MAX;
		}

		void markRendered(Texture& _texture, const RenderView& _view)
		{
			_texture.arrayDirty = true;
			RenderView latest = _view;
			for (auto it = _texture.rendered.begin(); it != _texture.rendered.end(); ++it)
				if (it->mip == _view.mip && it->layer == _view.layer && it->count == _view.count)
				{
					latest.autoMips |= it->autoMips;
					_texture.rendered.erase(it);
					break;
				}
			// Preserve last-writer order for overlapping mutable-sRGB layer ranges.
			_texture.rendered.push_back(latest);
		}

		void finishRenderPass()
		{
			uint32_t colorIndex = 0;
			Window* window = windowFor(m_currentFrameBuffer);
			if (!window)
				for (const Attachment& attachment : m_frameBuffers[m_currentFrameBuffer.idx].attachments)
				{
					Texture& texture = m_textures[attachment.handle.idx];
					const bool depth = bimg::isDepth(bimg::TextureFormat::Enum(texture.format));
					const bool written = depth ? m_depthWritten : 0 != (m_colorWritten & (1u << colorIndex++));
					if (!written) continue;
					invalidateTexture(texture, depth ? GX2_INVALIDATE_MODE_DEPTH_BUFFER : GX2_INVALIDATE_MODE_COLOR_BUFFER, true);
					if (!(texture.flags & BGFX_TEXTURE_RT_WRITE_ONLY) || texture.renderSurface.aa != GX2_AA_MODE1X) markRendered(texture, {attachment.mip, attachment.layer, attachment.numLayers, 0 != (attachment.flags & BGFX_ATTACHMENT_AUTO_GEN_MIPS), !depth && 0 != (uint32_t(m_colors[colorIndex-1].surface.format) & 0x400)});
				}
			else
			{
				window->colorDirty |= m_colorWritten != 0;
				if (m_depthWritten && isValid(window->swapChain.depth))
				{
					Texture& texture = m_textures[window->swapChain.depth.idx];
					invalidateTexture(texture, GX2_INVALIDATE_MODE_DEPTH_BUFFER, true);
					if (!(texture.flags & BGFX_TEXTURE_RT_WRITE_ONLY) || texture.renderSurface.aa != GX2_AA_MODE1X) markRendered(texture, {0, 0, 1});
				}
			}
			m_colorWritten = 0; m_depthWritten = false;
		}

		Block* surfaceStorage(GX2Surface& _surface)
		{
			GX2CalcSurfaceSizeAndAlignment(&_surface);
			const uint32_t offset = _surface.mipmapSize ? _surface.mipLevelOffset[0] : _surface.imageSize;
			Block* block = allocate(offset+_surface.mipmapSize, _surface.alignment);
			DCInvalidateRange(block->data, block->size);
			_surface.image = block->data; _surface.mipmaps = _surface.mipmapSize ? (uint8_t*)block->data+offset : NULL;
			return block;
		}

		void primitiveRestart(GX2PrimitiveMode _primitive, uint32_t _indexSize)
		{
			const bool strip = _primitive == GX2_PRIMITIVE_MODE_TRIANGLE_STRIP || _primitive == GX2_PRIMITIVE_MODE_LINE_STRIP;
			const uint32_t size = strip ? _indexSize : 0;
			if (size == m_restartSize) return;
			alignas(64) const uint32_t registers[] = {0xc0016900, 0x2a5, size ? 1u : 0u};
			GX2CopyDisplayList(registers, sizeof(registers));
			if (size) GX2SetPrimitiveRestartIndex(size == 4 ? UINT32_MAX : UINT16_MAX);
			m_restartSize = size;
		}

		void rasterSamples(bool _multisample, bool _sampleFrequency = false)
		{
			// Keep GX2's scheduler baseline; change only MSAA_ENABLE and PS_ITER_SAMPLE.
			const uint32_t mode = 0x14000 | (_multisample ? 1 : 0) | (_sampleFrequency ? 0x20000 : 0);
			if (mode == m_rasterMode) return;
			alignas(64) const uint32_t registers[] = {0xc0016900, 0x293, mode};
			GX2CopyDisplayList(registers, sizeof(registers));
			m_rasterMode = mode;
		}

		void fullscreen(Program& _program, const GX2Texture& _source, GX2ColorBuffer* _color, GX2DepthBuffer* _depth, uint32_t _width, uint32_t _height, const float* _params = NULL, const GX2Texture* _stencil = NULL, const Rect* _rect = NULL, bool _sampleFrequency = false)
		{
			if (_color) GX2SetColorBuffer(_color, GX2_RENDER_TARGET_0);
			if (_depth)
			{
				GX2SetDepthBuffer(_depth);
				if (_depth->hiZPtr) GX2SetClearDepthStencil(_depth, _depth->depthClear, _depth->stencilClear);
			}
			GX2SetAAMode(_color ? _color->surface.aa : _depth->surface.aa);
			GX2SetViewport(0, 0, _width, _height, -1, 1);
			if (_rect) setScissor(*_rect);
			else setScissor({0, 0, uint16_t(_width), uint16_t(_height)});
			GX2SetDepthStencilControl(_depth != NULL, _depth != NULL, GX2_COMPARE_FUNC_ALWAYS, _stencil != NULL, FALSE,
				GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_REPLACE,
				GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_REPLACE);
			if (_stencil) GX2SetStencilMask(0xff, 0xff, 0, 0xff, 0xff, 0);
			GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, _color != NULL); channelMasks(15, _color ? 1 : 0);
			const uint32_t type = uint32_t(_source.surface.format) & 0x300;
			const bool point = _depth != NULL || type == 0x100 || type == 0x300;
			GX2Sampler sampler; GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, point ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR);
			// NO_MIP bypasses the selected source mip on native GX2.
			GX2InitSamplerZMFilter(&sampler, point ? GX2_TEX_Z_FILTER_MODE_POINT : GX2_TEX_Z_FILTER_MODE_LINEAR, GX2_TEX_MIP_FILTER_MODE_POINT); GX2InitSamplerLOD(&sampler, 0, 0, 0);
			GX2SetPixelSampler(&sampler, 0); GX2SetPixelTexture(&_source, 0);
			if (_stencil) { GX2SetPixelSampler(&sampler, 1); GX2SetPixelTexture(_stencil, 1); }
			if (_params) updateUniform(m_uniformReg.find("u_mipGen")->m_handle.idx, _params, 16);
			uniformsBegin(_program); uniformsEnd(_program);
			RenderDraw draw; draw.clear(); draw.m_streamMask = 0; vertexInput(_program, draw);
			rasterSamples(true, _sampleFrequency);
			GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, 1);
			if (_sampleFrequency) rasterSamples(true);
		}

		void resolveDepth(Texture& _texture, const RenderView& _view)
		{
			GX2DepthBuffer depth = depthBuffer(_texture, _view);
			if (depth.hiZPtr) GX2SetClearDepthStencil(&depth, depth.depthClear, depth.stencilClear);
			if (depth.surface.aa == GX2_AA_MODE1X || _texture.texture.surface.aa != GX2_AA_MODE1X)
			{
				if (separateRenderStorage(_texture)) GX2ConvertDepthBufferToTextureSurface(&depth, &_texture.texture.surface, _view.mip, _view.layer);
				else if (depth.surface.aa != GX2_AA_MODE1X)
				{
					// Write-only depth retains native depth tiling. Blits sample a
					// converted mirror, refreshed only after another depth write.
					if (!_texture.depthSamplesImage)
					{
						_texture.depthSamples = depth.surface;
						_texture.depthSamples.use = GX2_SURFACE_USE_TEXTURE;
						_texture.depthSamples.swizzle = 0; _texture.depthSamples.tileMode = GX2_TILE_MODE_DEFAULT;
						_texture.depthSamplesImage = surfaceStorage(_texture.depthSamples);
					}
					GX2ConvertDepthBufferToTextureSurface(&depth, &_texture.depthSamples, _view.mip, _view.layer);
					GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _texture.depthSamplesImage->data, _texture.depthSamplesImage->size);
					GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _texture.depthSamplesImage->data, _texture.depthSamplesImage->size);
					touch(_texture.depthSamplesImage);
				}
				else if (depth.hiZPtr) GX2ExpandDepthBuffer(&depth);
				return;
			}
			// GX2's depth conversion preserves samples. Resolve sample zero with
			// the shared depth shader, then convert the single-sample depth layout.
			if (!_texture.depthSamplesImage)
			{
				_texture.depthSamples = depth.surface; _texture.depthSamples.use = GX2_SURFACE_USE_TEXTURE;
				_texture.depthSamples.swizzle = 0; _texture.depthSamples.tileMode = GX2_TILE_MODE_DEFAULT;
				_texture.depthSamplesImage = surfaceStorage(_texture.depthSamples);
				_texture.resolvedDepth = depth.surface; _texture.resolvedDepth.aa = GX2_AA_MODE1X;
				_texture.resolvedDepth.dim = depth.surface.depth > 1 ? GX2_SURFACE_DIM_TEXTURE_2D_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D;
				_texture.resolvedDepth.use = GX2_SURFACE_USE_DEPTH_BUFFER; _texture.resolvedDepth.swizzle = 0; _texture.resolvedDepth.tileMode = GX2_TILE_MODE_DEFAULT;
				_texture.resolvedDepthImage = surfaceStorage(_texture.resolvedDepth);
			}
			GX2ConvertDepthBufferToTextureSurface(&depth, &_texture.depthSamples, _view.mip, _view.layer);
			GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _texture.depthSamplesImage->data, _texture.depthSamplesImage->size);
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _texture.depthSamplesImage->data, _texture.depthSamplesImage->size);
			restore(); beginBatch();
			for (uint32_t slice = _view.layer; slice < uint32_t(_view.layer)+_view.count; ++slice)
			{
				GX2Texture source = _texture.texture; source.surface = _texture.depthSamples;
				source.viewFirstMip = 0; source.viewNumMips = 1; source.viewFirstSlice = slice; source.viewNumSlices = 1; GX2InitTextureRegs(&source);
				GX2DepthBuffer dest = {}; dest.surface = _texture.resolvedDepth; dest.viewFirstSlice = slice; dest.viewNumSlices = 1; GX2InitDepthBufferRegs(&dest);
				Program& program = m_programs[g_depthResolve->m_program[source.surface.depth > 1 ? 1 : 0].idx];
				fullscreen(program, source, NULL, &dest, dest.surface.width, dest.surface.height);
			}
			endBatch();
			GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, _texture.resolvedDepthImage->data, _texture.resolvedDepthImage->size);
			GX2DepthBuffer resolved = {}; resolved.surface = _texture.resolvedDepth; resolved.viewFirstSlice = _view.layer; resolved.viewNumSlices = _view.count; GX2InitDepthBufferRegs(&resolved);
			GX2ConvertDepthBufferToTextureSurface(&resolved, &_texture.texture.surface, 0, _view.layer);
			touch(_texture.depthSamplesImage); touch(_texture.resolvedDepthImage);
		}

		void generateMips(Texture& _texture, const RenderView& _view)
		{
			if (!_view.autoMips || !supportsMipAutogen(_texture.format) || _texture.texture.surface.mipLevels <= _view.mip+1) return;
			const GX2Surface& surface = _texture.texture.surface;
			const bool volume = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D;
			const bool array = surface.dim != GX2_SURFACE_DIM_TEXTURE_2D && !volume;
			Program& program = m_programs[m_mipGen->m_program[volume ? 2 : array ? 1 : 0].idx];
			const bool cube = surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE;
			if (cube) cubeArrayView(_texture);
			for (uint32_t mip = _view.mip+1; mip < surface.mipLevels; ++mip)
			{
				const uint32_t width = bx::max(1u, surface.width >> mip), height = bx::max(1u, surface.height >> mip);
				const uint32_t first = volume ? 0 : _view.layer, count = volume ? bx::max(1u, surface.depth >> mip) : _view.count;
				if (cube || mip == uint32_t(_view.mip)+1) restore();
				beginBatch();
				for (uint32_t slice = first; slice < first+count; ++slice)
				{
					GX2Texture source = cube ? _texture.arrayView : _texture.texture; source.viewFirstMip = mip-1; source.viewNumMips = 1;
					source.surface.format = textureFormat(_texture.format, _view.srgb, source.compMap);
					// Filter storage channels rather than logical alpha/BGR routing.
					switch (_texture.format)
					{
					case TextureFormat::A8: source.compMap = GX2_COMP_MAP(0, 4, 4, 5); break;
					case TextureFormat::BGRA8:
					case TextureFormat::BGRA4:
					case TextureFormat::BGR5A1: source.compMap = GX2_COMP_MAP(0, 1, 2, 3); break;
					case TextureFormat::R5G6B5:
					case TextureFormat::B5G6R5:
					case TextureFormat::RG11B10F: source.compMap = GX2_COMP_MAP(0, 1, 2, 5); break;
					default: break;
					}
					if (!volume) { source.surface.dim = array ? GX2_SURFACE_DIM_TEXTURE_2D_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D; source.viewFirstSlice = slice; source.viewNumSlices = 1; }
					GX2InitTextureRegs(&source);
					GX2ColorBuffer dest = {}; dest.surface = surface; dest.surface.format = source.surface.format; dest.viewMip = mip; dest.viewFirstSlice = slice; dest.viewNumSlices = 1; GX2InitColorBufferRegs(&dest);
					const float params[4] = {1.0f/width, 1.0f/height, (float(slice)+0.5f)/count, 0};
					fullscreen(program, source, &dest, NULL, width, height, params);
				}
				endBatch();
				invalidateTexture(_texture, GX2_INVALIDATE_MODE_COLOR_BUFFER);
				invalidateTexture(_texture, GX2_INVALIDATE_MODE_TEXTURE);
				if (cube)
				{
					for (uint32_t slice = first; slice < first+count; ++slice) GX2CopySurface(&surface, mip, slice, &_texture.arrayView.surface, mip, slice);
					GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _texture.arrayImage->data, _texture.arrayImage->size);
					GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _texture.arrayImage->data, _texture.arrayImage->size);
				}
			}
		}


		bool textureNeedsHelper(const Texture& _texture)
		{
			if (separateRenderStorage(_texture) || _texture.hiZ || _texture.renderSurface.aa != GX2_AA_MODE1X) return true;
			for (const RenderView& view : _texture.rendered)
				if (view.autoMips && supportsMipAutogen(_texture.format) && _texture.texture.surface.mipLevels > view.mip+1) return true;
			return false;
		}

		void resolveTexture(Texture& _texture)
		{
			if (_texture.rendered.empty()) return;
			bool helperWrites = false;
			for (const RenderView& view : _texture.rendered)
			{
				if (bimg::isDepth(bimg::TextureFormat::Enum(_texture.format)))
				{
					resolveDepth(_texture, view);
					helperWrites |= !(_texture.flags & BGFX_TEXTURE_RT_WRITE_ONLY) && (separateRenderStorage(_texture) || _texture.hiZ);
				}
				else if (_texture.renderSurface.aa != GX2_AA_MODE1X)
				{
					GX2ColorBuffer color = colorBuffer(_texture, view, view.srgb);
					if (_texture.texture.surface.aa != GX2_AA_MODE1X) GX2ExpandAAColorBuffer(&color);
					else
					{
						GX2Surface destination = _texture.texture.surface; uint32_t comp;
						destination.format = textureFormat(_texture.format, view.srgb, comp);
						GX2ResolveAAColorBuffer(&color, &destination, view.mip, view.layer);
					}
					helperWrites = true;
				}
			}
			if (helperWrites)
			{
				_texture.arrayDirty = true;
				invalidateTexture(_texture, GX2_INVALIDATE_MODE_COLOR_BUFFER);
			}
			if (!(_texture.flags & BGFX_TEXTURE_RT_WRITE_ONLY) || !_texture.depthSamplesImage)
				invalidateTexture(_texture, GX2_INVALIDATE_MODE_TEXTURE);
			for (const RenderView& view : _texture.rendered) generateMips(_texture, view);
			_texture.rendered.clear();
			if (_texture.image) touch(_texture.image);
		}

		bool prepareTextures(const RenderBind& _bind, const Program& _program)
		{
			GX2_PROFILE_SCOPE(PrepareTextures);
			bool restored = false;
			for (uint32_t mask = _program.samplerMask; mask; mask &= mask-1)
			{
				const uint32_t unit = bx::countTrailingZeros(mask);
				const Binding& bind = _bind.m_bind[unit];
				if (bind.m_idx == kInvalidHandle || bind.m_type != Binding::Texture) continue;
				Texture& texture = m_textures[bind.m_idx];
				const uint8_t dimension = _program.fragment->samplerDimensions[unit] ? _program.fragment->samplerDimensions[unit] : _program.vertex->samplerDimensions[unit];
				const bool array = texture.texture.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE && dimension == TextureDimension::Dimension2DArray+1;
				if (!restored && ((!texture.rendered.empty() && textureNeedsHelper(texture)) || (array && texture.arrayDirty))) { endBatch(); restored = true; }
				resolveTexture(texture);
				if (array) cubeArrayView(texture);
			}
			if (restored) { restore(); bindFrameBuffer(m_currentFrameBuffer); beginBatch(); }
			return restored;
		}

		GX2Surface transferSource(Texture& _texture, uint8_t, uint16_t, uint16_t)
		{
			resolveTexture(_texture);
			if (_texture.image) touch(_texture.image);
			return _texture.texture.surface;
		}

		GX2Surface transferDestination(Texture& _texture, uint8_t _mip, uint16_t _slice, uint16_t _count, const Rect& _rect, bool _preserveSource = false)
		{
			resolveTexture(_texture);
			if (_texture.hiZ && !_texture.renderImage) retire(_texture.hiZ);
			if (_preserveSource && _texture.image && _texture.image->stamp > GX2GetRetiredTimeStamp())
			{
				Block* previous = _texture.image; GX2Surface source = _texture.texture.surface;
				_texture.image = reuse(_texture.spareImages, previous->size, source.alignment, true); textureStorage(_texture, _texture.image);
				for (uint32_t mip = 0; mip < source.mipLevels; ++mip)
					for (uint32_t slice = 0; slice < _texture.levels[mip].slices.size(); ++slice)
					{
						const bool replaced = mip == _mip && slice >= _slice && slice < uint32_t(_slice)+_count
							&& !_rect.m_x && !_rect.m_y && _rect.m_width == bx::max(1u, source.width >> mip) && _rect.m_height == bx::max(1u, source.height >> mip);
						if (!replaced) GX2CopySurface(&source, mip, slice, &_texture.texture.surface, mip, slice);
					}
				touch(previous); _texture.spareImages.push_back(previous);
			}
			if (_texture.image) touch(_texture.image);
			return _texture.texture.surface;
		}

		void transferWritten(Texture& _texture, uint8_t _mip, uint16_t _slice, uint16_t _count)
		{
			_texture.arrayDirty = true;
			invalidateTexture(_texture, GX2_INVALIDATE_MODE_COLOR_BUFFER);
			invalidateTexture(_texture, GX2_INVALIDATE_MODE_TEXTURE);
			for (uint32_t slice = _slice; slice < uint32_t(_slice)+_count; ++slice) _texture.levels[_mip].slices[slice].renderDirty = separateRenderStorage(_texture);
			if (_texture.image) touch(_texture.image);
		}

		bool transferScreen(FrameBufferHandle _handle, GX2Surface& _surface, uint32_t& _mip, uint32_t& _slice, TextureFormat::Enum& _format)
		{
			Window* window = windowFor(_handle);
			if (!window || m_suspended || !window->swapChain.nwh) return false;
			prepareWindow(*window); resolveWindow(*window);
			// Screenshot-only windows still consume their owned color/AA storage.
			if (window->colorImage) touch(window->colorImage);
			if (window->auxiliary) touch(window->auxiliary);
			const GX2ColorBuffer& color = presentColor(*window);
			_surface = color.surface; _mip = color.viewMip; _slice = color.viewFirstSlice;
			if (window->swapChain.flags & BGFX_SWAP_CHAIN_SRGB_BACKBUFFER) _surface.format = GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8;
			_format = TextureFormat::RGBA8;
			if (_surface.aa == GX2_AA_MODE1X) GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _surface.image, _surface.imageSize);
			return true;
		}

		Buffer& transferBuffer(Handle _handle)
		{
			return _handle.isIndexBuffer() ? m_indexBuffers[_handle.idx] : m_vertexBuffers[_handle.idx];
		}

		GX2Surface transferLinear(GX2SurfaceFormat _format, uint32_t _width, uint32_t _height)
		{
			GX2Surface surface = {};
			surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
			surface.width = _width; surface.height = _height; surface.depth = 1;
			surface.mipLevels = 1; surface.format = _format;
			surface.aa = GX2_AA_MODE1X;
			surface.use = GX2_SURFACE_USE_TEXTURE;
			surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
			GX2CalcSurfaceSizeAndAlignment(&surface);
			return surface;
		}

		// Coordinates remain pixels for BC surfaces; CPU pitches below are in blocks.
		// Full selected mip copies also cover tiny BC mip tails without an Ex rect.
		void transferCopy(const GX2Surface& _src, uint32_t _srcMip, uint32_t _srcSlice,
			uint32_t _srcX, uint32_t _srcY, GX2Surface& _dst, uint32_t _dstMip,
			uint32_t _dstSlice, uint32_t _dstX, uint32_t _dstY,
			uint32_t _width, uint32_t _height, TextureFormat::Enum _format)
		{
			const uint32_t srcWidth = bx::max(1u, _src.width >> _srcMip);
			const uint32_t srcHeight = bx::max(1u, _src.height >> _srcMip);
			const uint32_t dstWidth = bx::max(1u, _dst.width >> _dstMip);
			const uint32_t dstHeight = bx::max(1u, _dst.height >> _dstMip);
			if (_src.aa != GX2_AA_MODE1X || _dst.aa != GX2_AA_MODE1X)
			{
				unsupported("native multisample surface blit (prepare a single-sample representation)");
				return;
			}
			if (!(_srcX | _srcY | _dstX | _dstY)
			&& srcWidth == dstWidth && srcHeight == dstHeight
			&& _width == srcWidth && _height == srcHeight)
			{
				GX2CopySurface(&_src, _srcMip, _srcSlice, &_dst, _dstMip, _dstSlice);
				return;
			}
			if (bimg::isCompressed(bimg::TextureFormat::Enum(_format))
			&& ((_srcX | _srcY | _dstX | _dstY | _width | _height) & 3))
			{
				unsupported("unaligned BC subregion blit (GX2CopySurfaceEx requires 4x4 pixels)");
				return;
			}
			GX2Rect rect = { int32_t(_srcX), int32_t(_srcY),
				int32_t(_srcX+_width), int32_t(_srcY+_height) };
			GX2Point point = { int32_t(_dstX), int32_t(_dstY) };
			GX2CopySurfaceEx(&_src, _srcMip, _srcSlice, &_dst, _dstMip, _dstSlice, 1, &rect, &point);
		}

		void transferQueueSurface(const GX2Surface& _source, uint32_t _mip,
			uint32_t _slice, TextureFormat::Enum _format, TransferOutput _output, const GX2ColorBuffer* _aaSource = NULL)
		{
			GX2Surface staging = transferLinear(_source.format,
				bx::max(1u, _source.width >> _mip), bx::max(1u, _source.height >> _mip));
			Block* block = allocate(staging.imageSize, staging.alignment);
			// Allocations have complete 64-byte cache-block ownership. CPU bookkeeping
			// is in Block, never in the GPU destination allocation.
			DCInvalidateRange(block->data, block->size);
			staging.image = block->data;
			if (_aaSource && _source.aa != GX2_AA_MODE1X)
			{
				GX2ColorBuffer source = *_aaSource; source.surface.format = _source.format;
				GX2InitColorBufferRegs(&source); GX2ResolveAAColorBuffer(&source, &staging, 0, 0);
			}
			else transferCopy(_source, _mip, _slice, 0, 0, staging, 0, 0, 0, 0,
				staging.width, staging.height, _format);
			// Copy uses a GPU output cache; retirement must cover its publication too.
			GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, block->data, block->size);
			touch(block);
			_output.block = block;
			_output.width = staging.width; _output.height = staging.height;
			_output.pitch = GX2GetSurfaceMipPitch(&staging, 0)*textureStorageBlockSize(_format);
			_output.rowBytes = calcTextureRegionPitch(_format, staging.width);
			_output.rows = calcTextureRegionNumRows(_format, staging.height);
			_output.format = _format;
			m_transferOutputs.push_back(_output);
		}

		void readTexture(TextureHandle _handle, void* _data, uint16_t _layer, uint8_t _mip) override
		{
			uploadTextures();
			Texture& texture = m_textures[_handle.idx];
			const GX2Surface source = transferSource(texture, _mip, _layer, 1);
			TransferOutput output;
			output.destination = _data;
			// Both public read APIs return submit frame + 2. Rendering frame F+1 must
			// publish F's result before its render semaphore is released (see .md).
			output.deadline = m_transferFrame + 1;
			transferQueueSurface(source, _mip, _layer, texture.format, output);
		}


		void requestScreenShot(FrameBufferHandle _handle, const char* _filePath) override
		{
			GX2Surface source;
			uint32_t mip, slice;
			TextureFormat::Enum format;
			if (!transferScreen(_handle, source, mip, slice, format)) return;
			TransferOutput output;
			output.kind = TransferOutput::ScreenShot;
			output.path.assign(_filePath, _filePath+bx::strLen(_filePath)+1);
			transferQueueSurface(source, mip, slice, format, output, &windowColor(*windowFor(_handle)));
		}

		// Call after all ordinary draws and debug text, once per submit, also when
		// the target is unavailable. BGFX_RESET_CAPTURE drives video capture;
		// Frame::m_capture is a graphics-debugger request and is unrelated.
		void transferCapture(bool _enabled)
		{
			if (_enabled)
			{
				GX2Surface source;
				uint32_t mip, slice;
				TextureFormat::Enum format;
				if (transferScreen(BGFX_INVALID_HANDLE, source, mip, slice, format))
				{
					TransferOutput output;
					output.kind = TransferOutput::CaptureFrame;
					transferQueueSurface(source, mip, slice, format, output, &windowColor(m_mainWindow));
				}
			}
			else if (m_captureRequested)
			{
				TransferOutput output;
				output.kind = TransferOutput::CaptureEnd;
				m_transferOutputs.push_back(output);
			}
			m_captureRequested = _enabled;
		}

		void transferEndCapture()
		{
			if (m_captureActive) g_callback->captureEnd();
			m_captureActive = false;
		}

		// Publish retired snapshots before the public read readiness frame. Shutdown
		// drains callbacks after its one GPU drain and before allocations are freed.
		void transferPublish(bool _shutdown = false)
		{
			OSTime completed = _shutdown ? INT64_MAX : GX2GetRetiredTimeStamp();
			for (TransferOutput& output : m_transferOutputs)
			{
				const bool due = output.kind == TransferOutput::TextureRead
					&& int32_t(m_transferFrame-output.deadline) >= 0;
				if (!output.block || (!_shutdown && !due) || output.block->stamp <= completed) continue;
				if (output.block->stamp == kPending) { GX2Flush(); seal(); }
				// A timeout is not completion. Only a due explicit readback (or shutdown)
				// reaches this wait; normal screenshot/capture collection only polls.
				{ GX2_PROFILE_SCOPE(ReadbackWait); while (!GX2WaitTimeStamp(output.block->stamp)) {} }
				completed = GX2GetRetiredTimeStamp();
			}
			size_t count = 0;
			for (TransferOutput& output : m_transferOutputs)
			{
				if (output.block && output.block->stamp > completed) break;
				if (output.kind == TransferOutput::CaptureEnd)
				{
					transferEndCapture();
					++count;
					continue;
				}
				// Pre-invalidation prevents dirty eviction during GPU ownership; this
				// post-invalidation drops stale/speculatively refilled CPU cache lines.
				DCInvalidateRange(output.block->data, output.block->size);
				if (output.kind == TransferOutput::TextureRead)
				{
					const uint32_t blockSize = textureStorageBlockSize(output.format);
					const uint32_t packedSize = bimg::getBlockInfo(bimg::TextureFormat::Enum(output.format)).blockSize;
					for (uint32_t row = 0; row < output.rows; ++row)
					{
						uint8_t* dst = (uint8_t*)output.destination+row*output.rowBytes;
						const uint8_t* src = (const uint8_t*)output.block->data+row*output.pitch;
						if (blockSize != packedSize)
							for (uint32_t pixel = 0; pixel < output.width; ++pixel)
								bx::memCopy(dst+pixel*packedSize, src+pixel*blockSize, packedSize);
						else bx::memCopy(dst, src, output.rowBytes);
					}
				}
				else if (output.kind == TransferOutput::ScreenShot)
				{
					g_callback->screenShot(output.path.data(), output.width, output.height,
						output.pitch, output.format, output.block->data, output.pitch*output.rows, false);
				}
				else
				{
					// Match the conventional BGRA8 video capture stream. The snapshot is
					// retired and CPU-owned here; no GPU upload or extra allocation is needed.
					bimg::imageSwizzleBgra8(output.block->data, output.pitch, output.width, output.height,
						output.block->data, output.pitch);
					output.format = TextureFormat::BGRA8;
					if (!m_captureActive || m_captureWidth != output.width || m_captureHeight != output.height
					|| m_capturePitch != output.pitch || m_captureFormat != output.format)
					{
						transferEndCapture();
						g_callback->captureBegin(output.width, output.height, output.pitch, output.format, false);
						m_captureActive = true;
						m_captureWidth = output.width; m_captureHeight = output.height;
						m_capturePitch = output.pitch; m_captureFormat = output.format;
					}
					g_callback->captureFrame(output.block->data, output.pitch*output.rows);
				}
				// Keep the backend's single owner/free path. This intentionally retires
				// the consumed block through the next seal, rather than freeing twice.
				retire(output.block);
				++count;
			}
			m_transferOutputs.erase(m_transferOutputs.begin(), m_transferOutputs.begin()+count);
			if (_shutdown) transferEndCapture();
		}

		// Ex copies require complete BC blocks. Repack encoded edge blocks on GPU
		// when source/destination mips do not share the same complete dimensions.
		void transferBcEdge(const BlitItem& _blit, const GX2Surface& _source, GX2Surface& _destination,
			uint32_t _width, uint32_t _height, uint32_t _count, TextureFormat::Enum _format)
		{
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(_format));
			const uint32_t srcWidth = bx::max(1u, _source.width >> _blit.m_srcMip), srcHeight = bx::max(1u, _source.height >> _blit.m_srcMip);
			const uint32_t dstWidth = bx::max(1u, _destination.width >> _blit.m_dstMip), dstHeight = bx::max(1u, _destination.height >> _blit.m_dstMip);
			const bool fullSource = _blit.m_srcX+bx::alignUp(_width, 4) > srcWidth || _blit.m_srcY+bx::alignUp(_height, 4) > srcHeight;
			for (uint32_t slice = 0; slice < _count; ++slice)
			{
				GX2Surface source = transferLinear(_source.format, fullSource ? srcWidth : bx::alignUp(_width, 4), fullSource ? srcHeight : bx::alignUp(_height, 4));
				GX2Surface destination = transferLinear(_destination.format, dstWidth, dstHeight);
				Block* src = gpuBufferAllocate(source.imageSize, source.alignment);
				Block* dst = gpuBufferAllocate(destination.imageSize, destination.alignment);
				DCInvalidateRange(src->data, src->size); DCInvalidateRange(dst->data, dst->size);
				source.image = src->data; destination.image = dst->data;
				transferCopy(_source, _blit.m_srcMip, _blit.m_srcZ+slice, fullSource ? 0 : _blit.m_srcX, fullSource ? 0 : _blit.m_srcY,
					source, 0, 0, 0, 0, source.width, source.height, _format);
				gpuCopyWritten(src);
				if (_blit.m_dstX || _blit.m_dstY || _width != dstWidth || _height != dstHeight)
				{
					GX2CopySurface(&_destination, _blit.m_dstMip, _blit.m_dstZ+slice, &destination, 0, 0);
					gpuCopyWritten(dst);
				}
				const uint32_t srcPitch = GX2GetSurfaceMipPitch(&source, 0)*info.blockSize;
				const uint32_t dstPitch = GX2GetSurfaceMipPitch(&destination, 0)*info.blockSize;
				const uint32_t srcOffset = fullSource ? (_blit.m_srcY/4)*srcPitch+(_blit.m_srcX/4)*info.blockSize : 0;
				const uint32_t dstOffset = (_blit.m_dstY/4)*dstPitch+(_blit.m_dstX/4)*info.blockSize;
				gpuCopyRows(src, srcOffset, srcPitch, dst, dstOffset, dstPitch, calcTextureRegionPitch(_format, _width), calcTextureRegionNumRows(_format, _height));
				gpuCopyWritten(dst);
				GX2CopySurface(&destination, 0, 0, &_destination, _blit.m_dstMip, _blit.m_dstZ+slice);
				retire(src); retire(dst);
			}
		}

		void transferColorSamples(const BlitItem& _blit)
		{
			Texture& src = m_textures[_blit.m_src.idx];
			Texture& dst = m_textures[_blit.m_dst.idx];
			if (src.texture.surface.aa != dst.texture.surface.aa)
			{ unsupported("color sample copy requires matching AA modes"); return; }
			const uint64_t sourceBegin = OSEffectiveToPhysical(uint32_t(uintptr_t(src.texture.surface.image)));
			const uint64_t destinationBegin = OSEffectiveToPhysical(uint32_t(uintptr_t(dst.texture.surface.image)));
			const bool aliased = sourceBegin < destinationBegin+dst.texture.surface.imageSize
				&& destinationBegin < sourceBegin+src.texture.surface.imageSize;
			resolveTexture(src);
			// A borrowed source may alias the owned target's pending AA writes.
			if (aliased && &src != &dst) resolveTexture(dst);
			if (src.image) touch(src.image);
			if (src.auxiliary) touch(src.auxiliary);
			const uint16_t count = bx::max<uint16_t>(1, _blit.m_depth);
			GX2ColorBuffer destination = colorBuffer(dst, {_blit.m_dstMip, _blit.m_dstZ, count});
			if (!destination.aaBuffer)
			{ unsupported("external MSAA color destination lacks auxiliary metadata"); return; }
			const uint32_t width = bx::min<uint32_t>(_blit.m_width,
				bx::max(1u, src.texture.surface.width >> _blit.m_srcMip)-_blit.m_srcX,
				bx::max(1u, dst.texture.surface.width >> _blit.m_dstMip)-_blit.m_dstX);
			const uint32_t height = bx::min<uint32_t>(_blit.m_height,
				bx::max(1u, src.texture.surface.height >> _blit.m_srcMip)-_blit.m_srcY,
				bx::max(1u, dst.texture.surface.height >> _blit.m_dstMip)-_blit.m_dstY);
			GX2Texture source = src.texture;
			source.surface.format = GX2SurfaceFormat(uint32_t(source.surface.format) & ~0x400u);
			source.compMap = GX2_COMP_MAP(0, 1, 2, 3);
			source.viewFirstMip = _blit.m_srcMip; source.viewNumMips = 1;
			source.viewFirstSlice = _blit.m_srcZ; source.viewNumSlices = 1;
			auto copy = [&](GX2Texture input, GX2ColorBuffer output, const Rect& rect, int32_t offsetX, int32_t offsetY)
			{
				Program& program = m_programs[m_mipGen->m_program[input.surface.dim == GX2_SURFACE_DIM_TEXTURE_2D_MSAA_ARRAY ? 17 : 16].idx];
				output.surface.format = input.surface.format;
				const float params[4] = {float(offsetX), float(offsetY), 0, 0};
				restore(); beginBatch();
				for (uint16_t slice = 0; slice < count; ++slice)
				{
					GX2InitTextureRegs(&input);
					output.viewNumSlices = 1; GX2InitColorBufferRegs(&output);
					fullscreen(program, input, &output, NULL,
						bx::max(1u, output.surface.width >> output.viewMip), bx::max(1u, output.surface.height >> output.viewMip),
						params, NULL, &rect, true);
					++input.viewFirstSlice; ++output.viewFirstSlice;
				}
				endBatch();
			};
			Block* snapshotImage = NULL;
			Block* snapshotAuxiliary = NULL;
			if (aliased)
			{
				// Capture every selected layer before writing any destination layer;
				// per-layer snapshots would corrupt overlapping array self-copies.
				GX2Surface snapshot = source.surface;
				snapshot.width = width; snapshot.height = height; snapshot.depth = count; snapshot.mipLevels = 1;
				snapshot.dim = count > 1 ? GX2_SURFACE_DIM_TEXTURE_2D_MSAA_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D_MSAA;
				snapshot.use = GX2SurfaceUse(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
				snapshot.swizzle = 0; snapshot.tileMode = GX2_TILE_MODE_DEFAULT;
				snapshotImage = surfaceStorage(snapshot);
				GX2ColorBuffer color = {}; color.surface = snapshot; color.viewNumSlices = count;
				uint32_t bytes, alignment; GX2CalcColorBufferAuxInfo(&color, &bytes, &alignment);
				snapshotAuxiliary = allocate(bytes, alignment);
				bx::memSet(snapshotAuxiliary->data, GX2_AUX_BUFFER_CLEAR_VALUE, bytes);
				DCFlushRange(snapshotAuxiliary->data, bytes);
				color.aaBuffer = snapshotAuxiliary->data; color.aaSize = bytes;
				const Rect rect = {0, 0, uint16_t(width), uint16_t(height)};
				copy(source, color, rect, _blit.m_srcX, _blit.m_srcY);
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, snapshotImage->data, snapshotImage->size);
				GX2InitColorBufferRegs(&color); GX2ExpandAAColorBuffer(&color);
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, snapshotImage->data, snapshotImage->size);
				GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, snapshotImage->data, snapshotImage->size);
				source.surface = snapshot; source.viewFirstMip = 0; source.viewFirstSlice = 0;
			}
			const Rect rect = {int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), uint16_t(width), uint16_t(height)};
			copy(source, destination, rect, int32_t(snapshotImage ? 0 : _blit.m_srcX)-_blit.m_dstX, int32_t(snapshotImage ? 0 : _blit.m_srcY)-_blit.m_dstY);
			markRendered(dst, {_blit.m_dstMip, _blit.m_dstZ, count});
			invalidateTexture(dst, GX2_INVALIDATE_MODE_COLOR_BUFFER);
			if (dst.image) touch(dst.image);
			if (dst.auxiliary) touch(dst.auxiliary);
			if (snapshotImage) { touch(snapshotImage); touch(snapshotAuxiliary); retire(snapshotImage); retire(snapshotAuxiliary); }
		}

		void transferDepthSamples(const BlitItem& _blit)
		{
			Texture& src = m_textures[_blit.m_src.idx];
			Texture& dst = m_textures[_blit.m_dst.idx];
			if (dst.texture.surface.aa != GX2_AA_MODE1X && src.texture.surface.aa != dst.texture.surface.aa)
			{ unsupported("depth sample copy requires matching AA modes or a single-sample destination"); return; }
			if (src.format == TextureFormat::D24F)
			{ unsupported("sampled D24F encoding"); return; }
			resolveTexture(src);
			GX2Texture source = src.texture;
			if (!separateRenderStorage(src) && src.depthSamplesImage) source.surface = src.depthSamples;
			source.compMap = GX2_COMP_MAP(0, 1, 2, 3);
			source.viewFirstMip = _blit.m_srcMip; source.viewNumMips = 1;
			source.viewFirstSlice = _blit.m_srcZ; source.viewNumSlices = 1;
			const uint16_t count = bx::max<uint16_t>(1, _blit.m_depth);
			const uint32_t width = bx::min<uint32_t>(_blit.m_width,
				bx::max(1u, source.surface.width >> _blit.m_srcMip)-_blit.m_srcX,
				bx::max(1u, dst.texture.surface.width >> _blit.m_dstMip)-_blit.m_dstX);
			const uint32_t height = bx::min<uint32_t>(_blit.m_height,
				bx::max(1u, source.surface.height >> _blit.m_srcMip)-_blit.m_srcY,
				bx::max(1u, dst.texture.surface.height >> _blit.m_dstMip)-_blit.m_dstY);
			const bool stencil = src.format == TextureFormat::D24S8 || src.format == TextureFormat::D32FS8;
			auto copy = [&](GX2Texture input, GX2DepthBuffer output, const Rect& rect, int32_t offsetX, int32_t offsetY)
			{
				Program& program = m_programs[m_mipGen->m_program[input.surface.dim == GX2_SURFACE_DIM_TEXTURE_2D_MSAA_ARRAY ? 19 : 18].idx];
				const float params[4] = {float(offsetX), float(offsetY), 0, stencil ? 1.0f : 0.0f};
				restore(); beginBatch();
				for (uint16_t slice = 0; slice < count; ++slice)
				{
					GX2InitTextureRegs(&input);
					GX2Texture stencilInput = input;
					stencilInput.surface.format = src.format == TextureFormat::D32FS8 ? GX2_SURFACE_FORMAT_UINT_G8_X24 : GX2_SURFACE_FORMAT_UINT_X24_G8;
					stencilInput.compMap = GX2_COMP_MAP(1, 4, 4, 5); GX2InitTextureRegs(&stencilInput);
					output.viewNumSlices = 1; GX2InitDepthBufferRegs(&output);
					GX2InitDepthBufferHiZEnable(&output, output.hiZPtr != NULL);
					fullscreen(program, input, NULL, &output,
						bx::max(1u, output.surface.width >> output.viewMip), bx::max(1u, output.surface.height >> output.viewMip),
						params, stencil ? &stencilInput : NULL, &rect, output.surface.aa != GX2_AA_MODE1X);
					++input.viewFirstSlice; ++output.viewFirstSlice;
				}
				endBatch();
			};
			GX2DepthBuffer destination = depthBuffer(dst, {_blit.m_dstMip, _blit.m_dstZ, count});
			const uint64_t sourceBegin = OSEffectiveToPhysical(uint32_t(uintptr_t(source.surface.image)));
			const uint64_t destinationBegin = OSEffectiveToPhysical(uint32_t(uintptr_t(destination.surface.image)));
			const bool aliased = sourceBegin < destinationBegin+destination.surface.imageSize
				&& destinationBegin < sourceBegin+source.surface.imageSize;
			const bool toTexture = destination.surface.aa != dst.texture.surface.aa
				|| !(destination.surface.use & GX2_SURFACE_USE_DEPTH_BUFFER);
			Block* temporaryDepth = NULL; Block* temporaryImage = NULL;
			GX2Surface converted = {};
			if (toTexture && dst.texture.surface.aa != GX2_AA_MODE1X)
			{
				// A sampleable-only AA destination has no native depth descriptor.
				// Preserve its untouched samples in native tiling, then convert back.
				GX2DepthBuffer temporary = {};
				temporary.surface = dst.texture.surface;
				temporary.surface.width = bx::max(1u, temporary.surface.width >> _blit.m_dstMip);
				temporary.surface.height = bx::max(1u, temporary.surface.height >> _blit.m_dstMip);
				temporary.surface.depth = count; temporary.surface.mipLevels = 1;
				temporary.surface.dim = count > 1 ? GX2_SURFACE_DIM_TEXTURE_2D_MSAA_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D_MSAA;
				temporary.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
				temporary.surface.swizzle = 0; temporary.surface.tileMode = GX2_TILE_MODE_DEFAULT;
				temporaryDepth = surfaceStorage(temporary.surface); temporary.viewNumSlices = count;
				if (_blit.m_dstX || _blit.m_dstY || width != temporary.surface.width || height != temporary.surface.height)
				{
					resolveTexture(dst);
					GX2Texture previous = dst.texture;
					previous.compMap = GX2_COMP_MAP(0, 1, 2, 3);
					previous.viewFirstMip = _blit.m_dstMip; previous.viewNumMips = 1;
					previous.viewFirstSlice = _blit.m_dstZ; previous.viewNumSlices = 1;
					copy(previous, temporary, {0, 0, uint16_t(temporary.surface.width), uint16_t(temporary.surface.height)}, 0, 0);
				}
				copy(source, temporary, {int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), uint16_t(width), uint16_t(height)},
					int32_t(_blit.m_srcX)-_blit.m_dstX, int32_t(_blit.m_srcY)-_blit.m_dstY);
				GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, temporaryDepth->data, temporaryDepth->size);
				GX2Surface output = transferDestination(dst, _blit.m_dstMip, _blit.m_dstZ, count,
					{int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), uint16_t(width), uint16_t(height)});
				GX2InitDepthBufferRegs(&temporary); GX2InitDepthBufferHiZEnable(&temporary, FALSE);
				GX2ConvertDepthBufferToTextureSurface(&temporary, &output, _blit.m_dstMip, _blit.m_dstZ);
				transferWritten(dst, _blit.m_dstMip, _blit.m_dstZ, count);
			}
			else
			{
				if (toTexture || aliased)
				{
					// Convert through a cropped depth target. Capture every layer before
					// writing the destination, including overlapping borrowed aliases.
					GX2DepthBuffer temporary = {};
					temporary.surface = source.surface;
					temporary.surface.width = width; temporary.surface.height = height; temporary.surface.depth = count;
					temporary.surface.mipLevels = 1; temporary.surface.aa = toTexture ? dst.texture.surface.aa : source.surface.aa;
					temporary.surface.dim = temporary.surface.aa == GX2_AA_MODE1X ? (count > 1 ? GX2_SURFACE_DIM_TEXTURE_2D_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D)
						: (count > 1 ? GX2_SURFACE_DIM_TEXTURE_2D_MSAA_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D_MSAA);
					temporary.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
					temporary.surface.swizzle = 0; temporary.surface.tileMode = GX2_TILE_MODE_DEFAULT;
					temporaryDepth = surfaceStorage(temporary.surface); temporary.viewNumSlices = count;
					copy(source, temporary, {0, 0, uint16_t(width), uint16_t(height)}, _blit.m_srcX, _blit.m_srcY);
					GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, temporaryDepth->data, temporaryDepth->size);
					converted = temporary.surface; converted.use = GX2_SURFACE_USE_TEXTURE;
					temporaryImage = surfaceStorage(converted);
					GX2InitDepthBufferRegs(&temporary); GX2InitDepthBufferHiZEnable(&temporary, FALSE);
					GX2ConvertDepthBufferToTextureSurface(&temporary, &converted, 0, 0);
					GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, temporaryImage->data, temporaryImage->size);
					GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, temporaryImage->data, temporaryImage->size);
					if (toTexture)
					{
						GX2Surface output = transferDestination(dst, _blit.m_dstMip, _blit.m_dstZ, count,
							{int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), uint16_t(width), uint16_t(height)});
						for (uint16_t slice = 0; slice < count; ++slice)
							transferCopy(converted, 0, slice, 0, 0, output, _blit.m_dstMip, _blit.m_dstZ+slice, _blit.m_dstX, _blit.m_dstY, width, height, dst.format);
						transferWritten(dst, _blit.m_dstMip, _blit.m_dstZ, count);
					}
					else { source.surface = converted; source.viewFirstMip = 0; source.viewFirstSlice = 0; }
				}
				if (!toTexture)
				{
					restoreRenderTexture(dst, {_blit.m_dstMip, _blit.m_dstZ, count});
					copy(source, destination, {int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), uint16_t(width), uint16_t(height)},
						int32_t(aliased ? 0 : _blit.m_srcX)-_blit.m_dstX, int32_t(aliased ? 0 : _blit.m_srcY)-_blit.m_dstY);
					markRendered(dst, {_blit.m_dstMip, _blit.m_dstZ, count});
					invalidateTexture(dst, GX2_INVALIDATE_MODE_DEPTH_BUFFER, true);
				}
			}
			if (src.image) touch(src.image);
			if (src.renderImage) touch(src.renderImage);
			if (src.depthSamplesImage) touch(src.depthSamplesImage);
			if (src.hiZ) touch(src.hiZ);
			if (dst.image) touch(dst.image);
			if (dst.renderImage) touch(dst.renderImage);
			if (dst.hiZ) touch(dst.hiZ);
			if (temporaryDepth) { touch(temporaryDepth); retire(temporaryDepth); }
			if (temporaryImage) { touch(temporaryImage); retire(temporaryImage); }
		}

		void transferTextureBlit(const BlitItem& _blit)
		{
			Texture& src = m_textures[_blit.m_src.idx];
			Texture& dst = m_textures[_blit.m_dst.idx];
			if (src.texture.surface.aa != GX2_AA_MODE1X && bimg::isDepth(bimg::TextureFormat::Enum(src.format)))
			{ transferDepthSamples(_blit); return; }
			if (src.texture.surface.aa != GX2_AA_MODE1X && dst.texture.surface.aa != GX2_AA_MODE1X
			&& !bimg::isDepth(bimg::TextureFormat::Enum(src.format)))
			{ transferColorSamples(_blit); return; }
			const uint16_t count = bx::max<uint16_t>(1, _blit.m_depth);
			const bool resolve = src.texture.surface.aa != GX2_AA_MODE1X
				&& dst.texture.surface.aa == GX2_AA_MODE1X && !bimg::isDepth(bimg::TextureFormat::Enum(src.format));
			// Native resolve consumes compressed color/aux directly. Keep pending
			// source expansion for a later multisample sampling consumer.
			GX2Surface source = resolve ? src.texture.surface : transferSource(src, _blit.m_srcMip, _blit.m_srcZ, count);
			// Only a source alias requires an immutable old destination backing.
			// Independent GPU writes are ordered after previous readers in the FIFO.
			bool aliased = false;
			if (dst.image)
			{
				const uint64_t begin = OSEffectiveToPhysical(uint32_t(uintptr_t(dst.image->data)));
				auto overlaps = [&](const void* data, uint32_t size)
				{
					const uint64_t address = OSEffectiveToPhysical(uint32_t(uintptr_t(data)));
					return begin < address+size && address < begin+dst.image->size;
				};
				aliased = overlaps(source.image, source.imageSize)
					|| (source.mipmapSize && overlaps(source.mipmaps, source.mipmapSize));
			}
			GX2Surface destination = transferDestination(dst, _blit.m_dstMip, _blit.m_dstZ, count,
				{int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), _blit.m_width, _blit.m_height}, aliased);
			// Raw copies preserve stored values across UNORM/sRGB views. Arithmetic
			// resolves retain the source encoding for gamma-correct averaging.
			if (!resolve) source.format = GX2SurfaceFormat(uint32_t(source.format) & ~0x400u);
			destination.format = source.format;
			const uint32_t width = bx::min<uint32_t>(_blit.m_width,
				bx::max(1u, source.width >> _blit.m_srcMip)-_blit.m_srcX,
				bx::max(1u, destination.width >> _blit.m_dstMip)-_blit.m_dstX);
			const uint32_t height = bx::min<uint32_t>(_blit.m_height,
				bx::max(1u, source.height >> _blit.m_srcMip)-_blit.m_srcY,
				bx::max(1u, destination.height >> _blit.m_dstMip)-_blit.m_dstY);
			const bool full = !(_blit.m_srcX | _blit.m_srcY | _blit.m_dstX | _blit.m_dstY)
				&& width == bx::max(1u, source.width >> _blit.m_srcMip) && height == bx::max(1u, source.height >> _blit.m_srcMip)
				&& width == bx::max(1u, destination.width >> _blit.m_dstMip) && height == bx::max(1u, destination.height >> _blit.m_dstMip);
			if (resolve)
			{
				if (full && (src.auxiliary || (src.external && src.external->color && src.external->color->aaBuffer)))
				{
					GX2ColorBuffer color = colorBuffer(src, {_blit.m_srcMip, _blit.m_srcZ, count});
					color.surface.format = source.format; GX2InitColorBufferRegs(&color);
					GX2ResolveAAColorBuffer(&color, &destination, _blit.m_dstMip, _blit.m_dstZ);
				}
				else
				{
					resolveTexture(src);
					restore(); beginBatch();
					Program& program = m_programs[m_mipGen->m_program[source.dim == GX2_SURFACE_DIM_TEXTURE_2D_MSAA_ARRAY ? 15 : 14].idx];
					const Rect rect = {int16_t(_blit.m_dstX), int16_t(_blit.m_dstY), uint16_t(width), uint16_t(height)};
					const float params[4] = {float(int32_t(_blit.m_srcX)-_blit.m_dstX), float(int32_t(_blit.m_srcY)-_blit.m_dstY), float(1u << source.aa), 0};
					for (uint16_t slice = 0; slice < count; ++slice)
					{
						GX2Texture sampled = src.texture; sampled.surface.format = source.format;
						sampled.compMap = GX2_COMP_MAP(0, 1, 2, 3);
						sampled.viewFirstMip = _blit.m_srcMip; sampled.viewNumMips = 1;
						sampled.viewFirstSlice = _blit.m_srcZ+slice; sampled.viewNumSlices = 1; GX2InitTextureRegs(&sampled);
						GX2ColorBuffer color = {}; color.surface = destination;
						color.viewMip = _blit.m_dstMip; color.viewFirstSlice = _blit.m_dstZ+slice; color.viewNumSlices = 1; GX2InitColorBufferRegs(&color);
						fullscreen(program, sampled, &color, NULL, bx::max(1u, destination.width >> _blit.m_dstMip), bx::max(1u, destination.height >> _blit.m_dstMip), params, NULL, &rect);
					}
					endBatch();
				}
				if (src.image) touch(src.image);
				if (src.auxiliary) touch(src.auxiliary);
				transferWritten(dst, _blit.m_dstMip, _blit.m_dstZ, count);
				return;
			}
			if (!full && bimg::isCompressed(bimg::TextureFormat::Enum(src.format)) && ((width | height) & 3))
			{
				if ((_blit.m_srcX | _blit.m_srcY | _blit.m_dstX | _blit.m_dstY) & 3) { unsupported("BC blit origin must address complete compressed blocks"); return; }
				transferBcEdge(_blit, source, destination, width, height, count, src.format);
				transferWritten(dst, _blit.m_dstMip, _blit.m_dstZ, count);
				return;
			}
			for (uint16_t slice = 0; slice < count; ++slice)
				transferCopy(source, _blit.m_srcMip, _blit.m_srcZ+slice, _blit.m_srcX, _blit.m_srcY,
					destination, _blit.m_dstMip, _blit.m_dstZ+slice, _blit.m_dstX, _blit.m_dstY,
					width, height, src.format);
			transferWritten(dst, _blit.m_dstMip, _blit.m_dstZ, count);
		}


		// Transfer each view's blits before its clear/draws. Native helpers run
		// outside the display list and share one state restoration for the group.
		bool transferBlits(BlitState& _state, uint16_t _view)
		{
			if (!_state.hasItem(_view)) return false;
			endBatch();
			finishRenderPass();
			uploadTextures();
			do { transferBlit(_state.advance()); } while (_state.hasItem(_view));
			restore();
			if (frameBufferAvailable(m_currentFrameBuffer)) bindFrameBuffer(m_currentFrameBuffer);
			beginBatch();
			return true; // caller reapplies its current view's viewport/scissor.
		}

		// Call after endBatch/finishRenderPass; do not manufacture a UINT16_MAX view
		// index for BlitState::hasItem, which indexes Frame::m_viewOrder[_view].
		void transferRemainingBlits(BlitState& _state)
		{
			if (_state.m_item == _state.m_frame->m_numBlitItems) return;
			uploadTextures();
			do { transferBlit(_state.advance()); }
			while (_state.m_item < _state.m_frame->m_numBlitItems);
			restore();
		}

		void restore()
		{
			GX2_PROFILE_ADD(ContextRestores, 1);
			GX2SetContextState(m_context);
			GX2SetShaderMode(kGraphicsShaderMode);
			GX2SetStreamOutEnable(FALSE);
			GX2SetRasterizerClipControlEx(TRUE, TRUE, TRUE);
			GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
			GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
			GX2SetAlphaToMask(FALSE, GX2_ALPHA_TO_MASK_MODE_NON_DITHERED);
			GX2SetLineWidth(1.0f); GX2SetPointLimits(1.0f, 255.0f);
			m_depthControl = {};
			GX2SetAAMask(0xff, 0xff, 0xff, 0xff); m_sampleMask = UINT32_MAX;
			m_rasterMode = UINT32_MAX; rasterSamples(true);
			m_restartSize = UINT32_MAX; primitiveRestart(GX2_PRIMITIVE_MODE_TRIANGLES, 0);
			m_cachedState = UINT64_MAX;
			m_cachedStencil = UINT64_MAX;
			m_cachedScissor = {-1, -1, UINT16_MAX, UINT16_MAX};
			m_cachedRgba = UINT32_MAX;
			m_boundProgram = NULL;
			m_boundFetch = NULL;
			bx::memSet(m_boundAttributes, 0, sizeof(m_boundAttributes));
			GX2SetAttribBuffer(BGFX_CONFIG_MAX_VERTEX_STREAMS+1, 16, 0, m_defaultVertex->data);
			bx::memSet(m_boundTextures, 0, sizeof(m_boundTextures));
			m_borderValid[0] = m_borderValid[1] = 0;

		}

		void beginBatch(uint32_t _uniformBytes = 0)
		{
			m_listOffset = acquire(m_lists, kListReservation);
#if !BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			m_uploadOffset = acquire(m_uploads, _uniformBytes);
#else
			BX_UNUSED(_uniformBytes);
#endif
			m_uploadUsed = 0;
			m_batchDraws = 0;
			GX2BeginDisplayListEx((uint8_t*)m_lists.block->data+m_listOffset, kListReservation, TRUE);
		}

		void endBatch()
		{
			void* listData = (uint8_t*)m_lists.block->data+m_listOffset;
			uint32_t bytes;
			{ GX2_PROFILE_SCOPE(ListFinish); bytes = GX2EndDisplayList(listData); }
			GX2_PROFILE_ADD(ListBytes, bytes);
#if !BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			if (m_uploadUsed)
			{
				// Publish populated spans only; other ring ranges can still be in flight.
				OSMemoryBarrier();
				{ GX2_PROFILE_SCOPE(UniformInvalidate); GX2Invalidate(GX2_INVALIDATE_MODE_UNIFORM_BLOCK, (uint8_t*)m_uploads.block->data+m_uploadOffset, m_uploadUsed); }
			}
#endif
			if (bytes) { GX2_PROFILE_SCOPE(ListCall); GX2CallDisplayList(listData, bytes); }
			m_lists.head += bx::alignUp(bytes, 64);
			m_uploads.head += m_uploadUsed;
		}

		// Clears share the existing draw-command budget; neither path can fill a
		// list with an unbounded run of commands that consumes no uniform bytes.
		void reserveBatch(uint32_t _operations, uint32_t _uniformBytes)
		{
#if !BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			const uint32_t capacity = m_uploads.block->size;
#else
			BX_UNUSED(_uniformBytes);
#endif
			if (m_batchDraws+_operations > 32
#if !BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			|| m_uploadOffset+m_uploadUsed+_uniformBytes > capacity
			|| m_uploads.head+m_uploadUsed+_uniformBytes > m_uploads.tail+capacity
#endif
			)
			{
				endBatch(); beginBatch(_uniformBytes);
			}
			m_batchDraws += _operations;
		}

		void uniformsBegin(Program& _program)
		{
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			// Large inline constants also consume the display list's draw budget.
			reserveBatch(1+(_program.vertex->uniformSize+_program.fragment->uniformSize)/4096, 0);
#else
			const uint32_t bytes = bx::alignUp(_program.vertex->uniformSize, 256)+bx::alignUp(_program.fragment->uniformSize, 256);
			reserveBatch(1, bytes);
#endif
			GX2_PROFILE_SCOPE(UniformFill);
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			if (_program.mvpOnly)
			{
				m_stageData[0] = m_uniformData[0];
				GX2_PROFILE_ADD(UniformBytes, 64);
				return; // The following full MVP write needs neither zeroing nor default copies.
			}
#endif
			for (uint32_t stage = 0; stage < 2; ++stage)
			{
				Shader& shader = *(stage ? _program.fragment : _program.vertex);
				if (!shader.uniformSize) continue;
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
				// Native setters copy into the display list before this CPU scratch is reused.
				m_stageData[stage] = m_uniformData[stage];
#else
				m_stageData[stage] = (uint8_t*)m_uploads.block->data + m_uploadOffset + m_uploadUsed;
#endif
				// A fresh snapshot has no contents to preserve. dcbz zeros its cache
				// blocks without fetching the previous bytes from MEM2.
				GX2_PROFILE_ADD(UniformBytes, shader.uniformSize);
				{ GX2_PROFILE_SCOPE(UniformZero); DCZeroRange(m_stageData[stage], shader.uniformSize); }
#if !BGFX_CONFIG_GX2_UNIFORM_REGISTERS
				m_uploadUsed += bx::alignUp(shader.uniformSize, 256);
#endif
				const GX2VertexShader* vertex = (const GX2VertexShader*)shader.header->data;
				const GX2PixelShader* fragment = (const GX2PixelShader*)shader.header->data;
				const uint32_t count = stage ? fragment->initialValueCount : vertex->initialValueCount;
				const GX2UniformInitialValue* initial = stage ? fragment->initialValues : vertex->initialValues;
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
				if (count) { GX2_PROFILE_SCOPE(UniformInitial);
#endif
				for (uint32_t ii = 0; ii < count; ++ii)
					writeFloats<BGFX_CONFIG_GX2_UNIFORM_REGISTERS>(m_stageData[stage]+initial[ii].offset*4, initial[ii].value, 4);
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
				}
#endif
				// Application values override native initializers; predefined fields follow.
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
				if (!shader.uniforms.empty()) { GX2_PROFILE_SCOPE(UniformApplication);
#endif
				for (const Uniform& uniform : shader.uniforms)
				{
					uint8_t* dst = m_stageData[stage]+uniform.offset*4;
					const float* src = (const float*)m_uniforms[uniform.handle.idx];
					if (uniform.type == UniformType::Mat3)
					{
						for (uint32_t ii = 0; ii < uint32_t(uniform.count)*3; ++ii)
							writeFloats<BGFX_CONFIG_GX2_UNIFORM_REGISTERS>(dst+ii*16, src+ii*3, 3);
					}
					else writeFloats<BGFX_CONFIG_GX2_UNIFORM_REGISTERS>(dst, src, uniform.rows*4);
				}
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
				}
#endif
			}
		}

		void setShaderUniform4f(uint8_t _flags, uint32_t _loc, const void* _data, uint32_t _num)
		{
			void* dst = m_stageData[0 != (_flags & kUniformFragmentBit)]+_loc*4;
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			if (m_boundProgram && m_boundProgram->compute) { writeFloats(dst, _data, _num*4); return; }
#endif
			writeFloats<BGFX_CONFIG_GX2_UNIFORM_REGISTERS>(dst, _data, _num*4);
		}
		void setShaderUniform4x4f(uint8_t _flags, uint32_t _loc, const void* _data, uint32_t _num)
		{ setShaderUniform4f(_flags, _loc, _data, _num); }

		void uniformsEnd(Program& _program)
		{
			GX2_PROFILE_SCOPE(UniformEnd);
#if !BGFX_CONFIG_GX2_UNIFORM_REGISTERS
			// Only uniformsBegin appends snapshots. With the same program, the preceding
			// allocation has the same stride; a batch restart has no predecessor.
			const uint32_t stride = bx::alignUp(_program.vertex->uniformSize, 256)+bx::alignUp(_program.fragment->uniformSize, 256);
#else
			if (_program.mvpOnly)
			{
				GX2_PROFILE_SCOPE(UniformBind);
				GX2_PROFILE_ADD(UniformBinds, 1);
				GX2SetVertexUniformReg(0, 16, m_uniformData[0]);
			}
			else
#endif
			for (uint32_t stage = 0; stage < 2; ++stage)
			{
				const uint32_t size = (stage ? _program.fragment : _program.vertex)->uniformSize;
				if (!size) continue;
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
				GX2_PROFILE_SCOPE(UniformBind);
				GX2_PROFILE_ADD(UniformBinds, 1);
				if (stage) GX2SetPixelUniformReg(0, size/4, m_stageData[stage]);
				else GX2SetVertexUniformReg(0, size/4, m_stageData[stage]);
#else
				// Compare one CPU cache line inline; larger blocks keep their original path.
				const bool cacheable = size <= 32;
				const bool reuse = cacheable && m_boundProgram == &_program && m_uploadUsed >= 2*stride
					&& 0 == __builtin_memcmp(__builtin_assume_aligned(m_stageData[stage], 32),
						__builtin_assume_aligned(m_stageData[stage]-stride, 32), 16)
					&& 0 == __builtin_memcmp(__builtin_assume_aligned(m_stageData[stage]+16, 16),
						__builtin_assume_aligned(m_stageData[stage]-stride+16, 16), 16);
				{ GX2_PROFILE_SCOPE(UniformPublish);
					// Uniform-ring starts are 256-byte aligned; these spans cover one/two CPU lines.
					if (cacheable) __asm__ volatile ("dcbst 0,%0" : : "r" (m_stageData[stage]) : "memory");
					else if (size <= 64) __asm__ volatile ("dcbf 0,%0; dcbf 0,%1" : : "r" (m_stageData[stage]), "r" (m_stageData[stage]+32) : "memory");
					else DCFlushRangeNoSync(m_stageData[stage], size);
				}
				if (reuse) { GX2_PROFILE_ADD(UniformReuses, 1); continue; }
				{ GX2_PROFILE_SCOPE(UniformBind);
					GX2_PROFILE_ADD(UniformBinds, 1);
					if (stage) GX2SetPixelUniformBlock(0, size, m_stageData[stage]);
					else GX2SetVertexUniformBlock(0, size, m_stageData[stage]);
				}
#endif
			}
			if (m_boundProgram != &_program) bindProgram(_program);
		}

		BX_NO_INLINE void bindProgram(Program& _program)
		{
			Block* header = _program.primitiveId ? _program.vertex->primitiveHeader : _program.vertex->header;
			Block* code = _program.primitiveId ? _program.vertex->primitiveProgram : _program.vertex->program;
			GX2SetVertexShader((const GX2VertexShader*)header->data);
			if (!m_boundProgram || m_boundProgram->primitiveId != _program.primitiveId)
			{
				const uint32_t mode = _program.primitiveId ? 1 : 0;
				alignas(64) const uint32_t registers[] = {
					UINT32_C(0xc0016900), 0x290, mode, // VGT_GS_MODE: scenario A or off.
					UINT32_C(0xc0016900), 0x2ad, mode, // VGT_REUSE_OFF: per-primitive vertex results.
				};
				GX2CopyDisplayList(registers, sizeof(registers));
			}
			GX2SetPixelShader((const GX2PixelShader*)_program.fragment->header->data);
			touch(code); touch(header);
			touch(_program.fragment->program); touch(_program.fragment->header);
			m_boundProgram = &_program;
		}

		GX2AttribStream attribute(const GX2AttribVar& _var, const VertexLayout& _layout, Attrib::Enum _attr, uint32_t _stream)
		{
			uint8_t num; AttribType::Enum type; bool normalized, asInt;
			_layout.decode(_attr, num, type, normalized, asInt);
			static const uint32_t formats[][4] =
			{
				{ GX2_ATTRIB_TYPE_8, GX2_ATTRIB_TYPE_8_8, GX2_ATTRIB_TYPE_8_8_8_8, GX2_ATTRIB_TYPE_8_8_8_8 },
				{ GX2_ATTRIB_TYPE_8, GX2_ATTRIB_TYPE_8_8, GX2_ATTRIB_TYPE_8_8_8_8, GX2_ATTRIB_TYPE_8_8_8_8 },
				{ GX2_ATTRIB_TYPE_10_10_10_2, GX2_ATTRIB_TYPE_10_10_10_2, GX2_ATTRIB_TYPE_10_10_10_2, GX2_ATTRIB_TYPE_10_10_10_2 },
				{ GX2_ATTRIB_TYPE_16, GX2_ATTRIB_TYPE_16_16, GX2_ATTRIB_TYPE_16_16_16_16, GX2_ATTRIB_TYPE_16_16_16_16 },
				{ GX2_ATTRIB_TYPE_16, GX2_ATTRIB_TYPE_16_16, GX2_ATTRIB_TYPE_16_16_16_16, GX2_ATTRIB_TYPE_16_16_16_16 },
				{ GX2_ATTRIB_TYPE_16_FLOAT, GX2_ATTRIB_TYPE_16_16_FLOAT, GX2_ATTRIB_TYPE_16_16_16_16_FLOAT, GX2_ATTRIB_TYPE_16_16_16_16_FLOAT },
				{ GX2_ATTRIB_TYPE_32_FLOAT, GX2_ATTRIB_TYPE_32_32_FLOAT, GX2_ATTRIB_TYPE_32_32_32_FLOAT, GX2_ATTRIB_TYPE_32_32_32_32_FLOAT },
				{ GX2_ATTRIB_TYPE_32, GX2_ATTRIB_TYPE_32_32, GX2_ATTRIB_TYPE_32_32_32, GX2_ATTRIB_TYPE_32_32_32_32 },
				{ GX2_ATTRIB_TYPE_32, GX2_ATTRIB_TYPE_32_32, GX2_ATTRIB_TYPE_32_32_32, GX2_ATTRIB_TYPE_32_32_32_32 },
			};
			uint32_t format = formats[type][num-1];
			if (type == AttribType::Int8 || type == AttribType::Int16 || type == AttribType::Int32) format |= GX2_ATTRIB_FLAG_SIGNED;
			BX_UNUSED(asInt); // CPU packing convention; shader reflection determines delivery.
			const bool integer = _var.type == GX2_SHADER_VAR_TYPE_INT || _var.type == GX2_SHADER_VAR_TYPE_UINT
				|| (_var.type >= GX2_SHADER_VAR_TYPE_INT2 && _var.type <= GX2_SHADER_VAR_TYPE_UINT4);
			if (integer) format |= GX2_ATTRIB_FLAG_INTEGER;
			else if (!normalized || type == AttribType::Float || type == AttribType::Half) format |= GX2_ATTRIB_FLAG_SCALED;
			GX2AttribStream stream = {};
			stream.location = _var.location; stream.buffer = _stream; stream.offset = _layout.m_offset[_attr];
			stream.format = GX2AttribFormat(format); stream.type = GX2_ATTRIB_INDEX_PER_VERTEX;
			stream.mask = GX2_SEL_MASK(0, num >= 2 ? 1 : 4, num >= 3 ? 2 : 4, num == 4 ? 3 : 5);
			stream.endianSwap = _layout.isPackedColor(_attr) ? GX2_ENDIAN_SWAP_8_IN_32
				: type <= AttribType::Uint8 ? GX2_ENDIAN_SWAP_NONE
				: (type == AttribType::Int16 || type == AttribType::Uint16 || type == AttribType::Half) ? GX2_ENDIAN_SWAP_8_IN_16 : GX2_ENDIAN_SWAP_8_IN_32;
			return stream;
		}

		BX_NO_INLINE Fetch* createFetchShader(Program& _program, const RenderDraw& _draw, const uint32_t* layouts, uint32_t streamMask, uint16_t instanceStride, uint32_t gpuEndianMask)
		{
			GX2_PROFILE_ADD(FetchCreates, 1);
			GX2VertexShader& shader = *(GX2VertexShader*)_program.vertex->header->data;
			GX2AttribStream attributes[32];
			uint32_t count = 0;
			for (uint32_t ii = 0; ii < shader.attribVarCount; ++ii)
			{
				const GX2AttribVar& var = shader.attribVars[ii];
				bool found = false;
				for (uint32_t ss = 0; ss < BGFX_CONFIG_MAX_VERTEX_STREAMS && !found; ++ss)
				{
					if (0 == (streamMask & (1<<ss))) continue;
					const Stream& stream = _draw.m_stream[ss];
					const Buffer& buffer = m_vertexBuffers[stream.m_handle.idx];
					const VertexLayout& layout = m_layouts[isValid(stream.m_layoutHandle) ? stream.m_layoutHandle.idx : buffer.layout.idx];
					for (uint32_t aa = 0; aa < Attrib::Count; ++aa)
						if (layout.has(Attrib::Enum(aa)) && 0 == bx::strCmp(var.name, s_attribName[aa]))
						{ attributes[count] = attribute(var, layout, Attrib::Enum(aa), ss);
							if (buffer.littleEndian) attributes[count].endianSwap = GX2_ENDIAN_SWAP_NONE;
							++count; found = true; break; }
				}
				for (uint32_t jj = 0; !found && jj < instanceStride/16; ++jj)
				{
					char name[24]; bx::snprintf(name, sizeof(name), "i_data%d", jj);
					if (0 == bx::strCmp(name, var.name))
					{
						GX2AttribStream stream = {};
						stream.location = var.location; stream.buffer = BGFX_CONFIG_MAX_VERTEX_STREAMS;
						stream.offset = jj*16; stream.format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
						stream.type = GX2_ATTRIB_INDEX_PER_INSTANCE; stream.aluDivisor = 1;
						stream.mask = GX2_SEL_MASK(0, 1, 2, 3); stream.endianSwap = (gpuEndianMask & (1u << BGFX_CONFIG_MAX_VERTEX_STREAMS)) ? GX2_ENDIAN_SWAP_NONE : GX2_ENDIAN_SWAP_8_IN_32;
						attributes[count++] = stream; found = true;
					}
				}
				if (!found)
				{
					// Pinned backends source missing attributes at offset zero of the last stream.
					if (streamMask)
					{
						const uint32_t ss = 31-bx::countLeadingZeros(streamMask);
						for (uint32_t aa = 0; aa < Attrib::Count; ++aa)
							if (0 == bx::strCmp(var.name, s_attribName[aa]))
							{
								const bool bytes = aa >= Attrib::Color0 && aa <= Attrib::Indices;
								VertexLayout layout;
								layout.begin().add(Attrib::Enum(aa), bytes ? 4 : aa >= Attrib::TexCoord0 ? 2 : 3,
									bytes ? AttribType::Uint8 : AttribType::Float).end();
								attributes[count] = attribute(var, layout, Attrib::Enum(aa), ss);
								if (gpuEndianMask & (1u << ss)) attributes[count].endianSwap = GX2_ENDIAN_SWAP_NONE;
								++count; found = true; break;
							}
					}
					if (found) continue;
					GX2AttribStream stream = {};
					stream.location = var.location; stream.buffer = BGFX_CONFIG_MAX_VERTEX_STREAMS+1;
					stream.format = GX2_ATTRIB_FORMAT_FLOAT_32; stream.type = GX2_ATTRIB_INDEX_PER_VERTEX;
					stream.mask = GX2_SEL_MASK(4, 4, 4, 5);
					attributes[count++] = stream;
				}
			}
			Fetch native = {};
			bx::memCopy(native.layouts, layouts, sizeof(native.layouts));
			native.streamMask = streamMask; native.instanceStride = instanceStride; native.gpuEndianMask = gpuEndianMask;
			const uint32_t size = GX2CalcFetchShaderSizeEx(count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
			native.program = allocate(size, GX2_SHADER_PROGRAM_ALIGNMENT);
			GX2InitFetchShaderEx(&native.shader, (uint8_t*)native.program->data, count, attributes, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, native.program->data, size);
			_program.fetches.push_back(native);
			return &_program.fetches.back();
		}

		void vertexInput(Program& _program, const RenderDraw& _draw)
		{
			GX2_PROFILE_SCOPE(VertexInput);
			const uint32_t streamMask = _draw.m_streamMask == UINT32_MAX ? 0 : _draw.m_streamMask;
			uint32_t layouts[BGFX_CONFIG_MAX_VERTEX_STREAMS] = {};
			uint32_t gpuEndianMask = 0;
			for (uint32_t ii = 0; ii < BGFX_CONFIG_MAX_VERTEX_STREAMS; ++ii)
			{
				if (0 == (streamMask & (1<<ii))) continue;
				const Stream& stream = _draw.m_stream[ii];
				Buffer& buffer = m_vertexBuffers[stream.m_handle.idx];
				const VertexLayout& layout = m_layouts[isValid(stream.m_layoutHandle) ? stream.m_layoutHandle.idx : buffer.layout.idx];
				layouts[ii] = layout.m_hash;
				if (buffer.littleEndian) gpuEndianMask |= 1u << ii;
				const uint32_t offset = stream.m_startVertex*layout.m_stride;
				const void* ptr = (const uint8_t*)buffer.block->data+offset;
				if (m_boundAttributes[ii] != ptr || m_boundStrides[ii] != layout.m_stride)
				{
					GX2SetAttribBuffer(ii, buffer.size-offset, layout.m_stride, ptr);
					m_boundAttributes[ii] = ptr; m_boundStrides[ii] = layout.m_stride;
				}
				touch(buffer.block);
			}
			const uint16_t instanceStride = isValid(_draw.m_instanceDataBuffer) ? _draw.m_instanceDataStride : 0;
			if (instanceStride)
			{
				Buffer& buffer = m_vertexBuffers[_draw.m_instanceDataBuffer.idx];
				if (buffer.littleEndian) gpuEndianMask |= 1u << BGFX_CONFIG_MAX_VERTEX_STREAMS;
				const void* ptr = (uint8_t*)buffer.block->data+_draw.m_instanceDataOffset;
				GX2SetAttribBuffer(BGFX_CONFIG_MAX_VERTEX_STREAMS, buffer.size-_draw.m_instanceDataOffset, instanceStride, ptr);
				touch(buffer.block);
			}
			Fetch* fetch = NULL;
			for (Fetch& candidate : _program.fetches)
				if (candidate.streamMask == streamMask && candidate.instanceStride == instanceStride && candidate.gpuEndianMask == gpuEndianMask
				&& 0 == __builtin_memcmp(layouts, candidate.layouts, sizeof(layouts))) { fetch = &candidate; break; }
			if (!fetch) fetch = createFetchShader(_program, _draw, layouts, streamMask, instanceStride, gpuEndianMask);
			if (m_boundFetch != fetch->program)
			{
				GX2SetFetchShader(&fetch->shader); m_boundFetch = fetch->program;
			}
			touch(fetch->program);
		}

		void channelMasks(uint32_t _components, uint32_t _targets)
		{
			GX2ChannelMask mask[8];
			for (uint32_t ii = 0; ii < 8; ++ii) mask[ii] = GX2ChannelMask(_targets & (1u << ii) ? _components : 0);
			GX2SetTargetChannelMasks(mask[0], mask[1], mask[2], mask[3], mask[4], mask[5], mask[6], mask[7]);
		}

		void depthControl(const DepthControl& _depth)
		{
			if (m_cachedState == UINT64_MAX || _depth.m_constant != m_depthControl.m_constant || _depth.m_slopeScale != m_depthControl.m_slopeScale
			|| _depth.m_clamp != m_depthControl.m_clamp || _depth.m_depthClamp != m_depthControl.m_depthClamp)
			{
				GX2SetRasterizerClipControlEx(TRUE, !_depth.m_depthClamp, TRUE);
				GX2SetPolygonOffset(float(_depth.m_constant), _depth.m_slopeScale, float(_depth.m_constant), _depth.m_slopeScale, _depth.m_clamp);
				m_depthControl = _depth; m_cachedState = UINT64_MAX;
			}
		}

		void state(uint64_t _state, uint64_t _stencil, uint32_t _rgba, bool _wireframe = false, bool _sampleFrequency = false)
		{
			GX2_PROFILE_SCOPE(PipelineState);
			if (_state & BGFX_STATE_CONSERVATIVE_RASTER) unsupported("conservative raster");
			const bool invalid = m_cachedState == UINT64_MAX;
			const bool multisample = 0 != (_state & BGFX_STATE_MSAA);
			rasterSamples(multisample, multisample && _sampleFrequency);
			// Alpha reference, topology and MSAA are applied outside this state group.
			_state &= ~(BGFX_STATE_ALPHA_REF_MASK | BGFX_STATE_PT_MASK | BGFX_STATE_MSAA);
			if (_state != m_cachedState || _stencil != m_cachedStencil || _wireframe != m_wireframe || ((_state & BGFX_STATE_BLEND_INDEPENDENT) && _rgba != m_cachedRgba))
			{
				GX2_PROFILE_ADD(PipelineSets, 1);
				const uint32_t front = unpackStencil(0, _stencil);
				const bool separate = stencilFrontAndBack(_stencil);
				const uint32_t back = separate ? unpackStencil(1, _stencil) : front;
				GX2SetDepthStencilControl(m_hasDepth && 0 != (_state & BGFX_STATE_DEPTH_TEST_MASK), m_hasDepth && !m_depthReadOnly && 0 != (_state & BGFX_STATE_WRITE_Z),
					s_compare[(_state & BGFX_STATE_DEPTH_TEST_MASK) >> BGFX_STATE_DEPTH_TEST_SHIFT],
					m_hasDepth && stencilEnabled(_stencil), separate,
					s_compare[(front & BGFX_STENCIL_TEST_MASK) >> BGFX_STENCIL_TEST_SHIFT],
					s_stencil[(front & BGFX_STENCIL_OP_PASS_Z_MASK) >> BGFX_STENCIL_OP_PASS_Z_SHIFT],
					s_stencil[(front & BGFX_STENCIL_OP_FAIL_Z_MASK) >> BGFX_STENCIL_OP_FAIL_Z_SHIFT],
					s_stencil[(front & BGFX_STENCIL_OP_FAIL_S_MASK) >> BGFX_STENCIL_OP_FAIL_S_SHIFT],
					s_compare[(back & BGFX_STENCIL_TEST_MASK) >> BGFX_STENCIL_TEST_SHIFT],
					s_stencil[(back & BGFX_STENCIL_OP_PASS_Z_MASK) >> BGFX_STENCIL_OP_PASS_Z_SHIFT],
					s_stencil[(back & BGFX_STENCIL_OP_FAIL_Z_MASK) >> BGFX_STENCIL_OP_FAIL_Z_SHIFT],
					s_stencil[(back & BGFX_STENCIL_OP_FAIL_S_MASK) >> BGFX_STENCIL_OP_FAIL_S_SHIFT]);
				const uint8_t readMask = uint8_t(front >> BGFX_STENCIL_FUNC_RMASK_SHIFT);
				const uint8_t writeMask = m_stencilReadOnly ? 0 : unpackStencilWriteMask(_stencil);
				GX2SetStencilMask(readMask, writeMask, uint8_t(front), readMask, writeMask, uint8_t(back));
				// Stencil-only changes preserve raster, blend, color-mask and point state.
				if (_state == m_cachedState && _wireframe == m_wireframe && _rgba == m_cachedRgba)
				{ m_cachedStencil = _stencil; return; }
				GX2_PROFILE_ADD(RasterBlendSets, 1);
				const bool ccw = 0 != (_state & BGFX_STATE_FRONT_CCW);
				const bool bias = m_depthControl.m_constant != 0 || m_depthControl.m_slopeScale != 0;
				GX2SetPolygonControl(ccw ? GX2_FRONT_FACE_CCW : GX2_FRONT_FACE_CW,
					0 != (_state & BGFX_STATE_CULL_CW), 0 != (_state & BGFX_STATE_CULL_CCW), _wireframe,
					_wireframe ? GX2_POLYGON_MODE_LINE : GX2_POLYGON_MODE_TRIANGLE, _wireframe ? GX2_POLYGON_MODE_LINE : GX2_POLYGON_MODE_TRIANGLE, bias, bias, bias);
				const uint32_t blend = uint32_t((_state & BGFX_STATE_BLEND_MASK) >> BGFX_STATE_BLEND_SHIFT);
				uint8_t blendMask = 0;
				for (uint32_t ii = 0; ii < m_numColors; ++ii)
				{
					uint32_t factors = blend;
					uint32_t eqRgb = (_state >> BGFX_STATE_BLEND_EQUATION_SHIFT) & 7;
					uint32_t eqAlpha = (_state >> (BGFX_STATE_BLEND_EQUATION_SHIFT+3)) & 7;
					if (ii && (_state & BGFX_STATE_BLEND_INDEPENDENT))
					{
						const uint32_t independent = (_rgba >> ((ii-1)*11)) & 0x7ff;
						factors = (independent & 255) | ((independent & 255) << 8); eqRgb = eqAlpha = independent >> 8;
					}
					if (factors) blendMask |= 1u << ii;
					GX2SetBlendControl(GX2RenderTarget(ii), s_blend[factors & 15], s_blend[(factors >> 4) & 15], s_equation[eqRgb], TRUE,
						s_blend[(factors >> 8) & 15], s_blend[(factors >> 12) & 15], s_equation[eqAlpha]);
				}
				const uint32_t mask = colorWriteMask(_state);
				GX2SetColorControl(GX2_LOGIC_OP_COPY, blendMask, FALSE, m_numColors != 0 && mask != 0);
				GX2_PROFILE_ADD(ColorDisabledSets, m_numColors == 0 || mask == 0);
				channelMasks(mask, (1u << m_numColors)-1);
				GX2SetAlphaToMask(0 != (_state & BGFX_STATE_BLEND_ALPHA_TO_COVERAGE), GX2_ALPHA_TO_MASK_MODE_NON_DITHERED);
				const float pointSize = float(bx::max<uint64_t>(1, (_state & BGFX_STATE_POINT_SIZE_MASK) >> BGFX_STATE_POINT_SIZE_SHIFT));
				GX2SetPointSize(pointSize, pointSize);
				m_cachedState = _state; m_wireframe = _wireframe; m_cachedStencil = _stencil;
			}
			if (invalid || m_cachedRgba != _rgba)
			{
				GX2SetBlendConstantColor(float(_rgba >> 24)/255, float((_rgba >> 16)&255)/255, float((_rgba >> 8)&255)/255, float(_rgba&255)/255);
				m_cachedRgba = _rgba;
			}
		}

		void readonlyBuffer(const Binding& _bind, uint32_t _unit, const Shader* _computeShader = NULL)
		{
			if ((_bind.m_type != Binding::VertexBuffer && _bind.m_type != Binding::IndexBuffer)
			|| (!_computeShader && !m_boundProgram->compute && _bind.m_access != Access::Read)) { unsupported("writable or non-buffer raw bindings"); return; }
			const bool index = _bind.m_type == Binding::IndexBuffer;
			Buffer& buffer = index ? m_indexBuffers[_bind.m_idx] : m_vertexBuffers[_bind.m_idx];
			const uint32_t offset = bx::min(_bind.m_offset, buffer.size);
			const uint32_t size = bx::min(_bind.m_size, buffer.size-offset);
			// update() and gpuBufferWritten() publish once at the producer boundary.
			const uint32_t endian = buffer.littleEndian ? 0 : index && !(buffer.flags & BGFX_BUFFER_INDEX32) ? 1 : 2;
			alignas(64) uint32_t commands[] = {
				UINT32_C(0xc0076d00), // SET_RESOURCE, register + 7 words
				0,
				OSEffectiveToPhysical(uint32_t(uintptr_t(buffer.block->data))+offset),
				size-1,
				(4u << 8) | (13u << 20) | (1u << 26) | (endian << 30),
				1u, 0, 0, UINT32_C(0xc0000000),
			};
			const Shader* vertex = _computeShader ? _computeShader : m_boundProgram->compute ? m_boundProgram->compute : m_boundProgram->vertex;
			const Shader* fragment = _computeShader ? NULL : m_boundProgram->fragment;
			if (vertex->rawSrvMask & (1u << _unit))
			{
				commands[1] = 0x460u+7*_unit;
				GX2CopyDisplayList(commands, sizeof(commands));
			}
			if (fragment && (fragment->rawSrvMask & (1u << _unit)))
			{
				commands[1] = 7*_unit;
				GX2CopyDisplayList(commands, sizeof(commands));
			}
			m_boundTextures[_unit] = NULL; // A later texture must replace this descriptor.
			touch(buffer.block);
		}

		void textures(const RenderBind& _bind, const float _palette[][4])
		{
			GX2_PROFILE_SCOPE(Textures);
			static const GX2TexClampMode clamps[] = { GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_CLAMP_MODE_MIRROR, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_CLAMP_BORDER };
			for (uint32_t mask = m_boundProgram->samplerMask; mask; mask &= mask-1)
			{
				const uint32_t ii = bx::countTrailingZeros(mask);
				const Binding& bind = _bind.m_bind[ii];
				if (bind.m_idx == kInvalidHandle) continue;
				if ((m_boundProgram->vertex->rawSrvMask | m_boundProgram->fragment->rawSrvMask) & (1u << ii))
				{
					readonlyBuffer(bind, ii);
					continue;
				}
				if (!m_boundProgram->vertex->samplerDimensions[ii] && !m_boundProgram->fragment->samplerDimensions[ii]) continue;
				if (bind.m_type != Binding::Texture) { unsupported("image and compute bindings"); continue; }
				Texture& texture = m_textures[bind.m_idx];
				const uint32_t flags = bind.m_samplerFlags & BGFX_SAMPLER_INTERNAL_DEFAULT ? uint32_t(texture.flags) : bind.m_samplerFlags;
				const uint8_t encodedDimension = m_boundProgram->fragment->samplerDimensions[ii] ? m_boundProgram->fragment->samplerDimensions[ii] : m_boundProgram->vertex->samplerDimensions[ii];
				const uint8_t stages = (m_boundProgram->vertex->samplerDimensions[ii] ? 1 : 0)
					| (m_boundProgram->fragment->samplerDimensions[ii] ? 2 : 0);
				const TextureDimension::Enum dimension = encodedDimension ? TextureDimension::Enum(encodedDimension-1) : TextureDimension::Count;
				const bool border = (flags & BGFX_SAMPLER_U_MASK) == BGFX_SAMPLER_U_BORDER
					|| (flags & BGFX_SAMPLER_V_MASK) == BGFX_SAMPLER_V_BORDER || (flags & BGFX_SAMPLER_W_MASK) == BGFX_SAMPLER_W_BORDER;
				const float* color = _palette[(flags & BGFX_SAMPLER_BORDER_COLOR_MASK) >> BGFX_SAMPLER_BORDER_COLOR_SHIFT];
				const bool vertexBorderChanged = border && m_boundProgram->vertex->samplerDimensions[ii]
					&& (!(m_borderValid[0] & (1u << ii)) || 0 != bx::memCmp(m_borderColors[0][ii], color, 16));
				const bool pixelBorderChanged = border && m_boundProgram->fragment->samplerDimensions[ii]
					&& (!(m_borderValid[1] & (1u << ii)) || 0 != bx::memCmp(m_borderColors[1][ii], color, 16));
				const bool borderChanged = vertexBorderChanged || pixelBorderChanged;
				GX2TexBorderType borderType = GX2_TEX_BORDER_TYPE_VARIABLE;

				if (m_boundTextures[ii] != &texture.texture || !(m_boundBindings[ii] == bind) || m_boundDimensions[ii] != encodedDimension || borderChanged || (m_boundStages[ii] & stages) != stages)
				{
					if (color[0] == 0 && color[1] == 0 && color[2] == 0)
					{
						if (color[3] == 0) borderType = GX2_TEX_BORDER_TYPE_TRANSPARENT_BLACK;
						else if (color[3] == 1) borderType = GX2_TEX_BORDER_TYPE_BLACK;
					}
					else if (color[0] == 1 && color[1] == 1 && color[2] == 1 && color[3] == 1) borderType = GX2_TEX_BORDER_TYPE_WHITE;
					const GX2Texture& base = texture.texture.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE && dimension == TextureDimension::Dimension2DArray ? texture.arrayView : texture.texture;
					GX2Texture view = base;
					view.viewFirstMip = bind.m_firstMip;
					view.viewNumMips = bx::min<uint32_t>(bind.m_numMips, view.surface.mipLevels-bind.m_firstMip);
					const bool cube = view.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE;
					const uint32_t faces = cube && (dimension == TextureDimension::DimensionCube || dimension == TextureDimension::DimensionCubeArray) ? 6 : 1;
					view.viewFirstSlice = uint32_t(bind.m_firstLayer)*faces;
					view.viewNumSlices = bx::min<uint32_t>(uint32_t(bind.m_numLayers)*faces, view.surface.depth-view.viewFirstSlice);
					if (texture.flags & BGFX_TEXTURE_SRGB_MUTABLE) view.surface.format = textureFormat(texture.format, 0 != (flags & BGFX_SAMPLER_SRGB), view.compMap);
					if (flags & BGFX_SAMPLER_SAMPLE_STENCIL)
					{
						view.surface.format = texture.format == TextureFormat::D32FS8 ? GX2_SURFACE_FORMAT_UINT_G8_X24 : GX2_SURFACE_FORMAT_UINT_X24_G8;
						view.compMap = GX2_COMP_MAP(1, 4, 4, 5);
					}
					// Reuse prepared base registers when the sampled view is unchanged.
					if (view.viewFirstMip != base.viewFirstMip || view.viewNumMips != base.viewNumMips
					|| view.viewFirstSlice != base.viewFirstSlice || view.viewNumSlices != base.viewNumSlices
					|| view.surface.format != base.surface.format || view.compMap != base.compMap)
					{ GX2_PROFILE_ADD(TextureViewBuilds, 1); GX2InitTextureRegs(&view); }
					if (stages & 2) { GX2_PROFILE_ADD(TextureSets, 1); GX2SetPixelTexture(&view, ii); }
					if (stages & 1) { GX2_PROFILE_ADD(TextureSets, 1); GX2SetVertexTexture(&view, ii); }
					const bool integer = (uint32_t(view.surface.format) & 0x300) == 0x100 || (uint32_t(view.surface.format) & 0x300) == 0x300;
					// Cache CPU register construction, independently of GPU binding state.
					const uint64_t samplerKey = uint64_t(flags) | (uint64_t(view.viewNumMips) << 32)
						| (uint64_t(bind.m_lod.min) << 40) | (uint64_t(bind.m_lod.max) << 48)
						| (uint64_t(borderType) << 56) | (uint64_t(integer) << 58) | (uint64_t(m_anisotropy) << 59);
					GX2Sampler& sampler = m_sampler;
					if (samplerKey != m_samplerKey)
					{
						GX2_PROFILE_ADD(SamplerBuilds, 1);
						GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);
						GX2InitSamplerClamping(&sampler, clamps[(flags & BGFX_SAMPLER_U_MASK) >> BGFX_SAMPLER_U_SHIFT],
							clamps[(flags & BGFX_SAMPLER_V_MASK) >> BGFX_SAMPLER_V_SHIFT], clamps[(flags & BGFX_SAMPLER_W_MASK) >> BGFX_SAMPLER_W_SHIFT]);
						GX2InitSamplerXYFilter(&sampler, flags & BGFX_SAMPLER_MAG_POINT ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR,
							flags & BGFX_SAMPLER_MIN_POINT ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR,
							flags & (BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC) ? m_anisotropy : GX2_TEX_ANISO_RATIO_NONE);
						GX2InitSamplerZMFilter(&sampler, flags & BGFX_SAMPLER_MIN_POINT ? GX2_TEX_Z_FILTER_MODE_POINT : GX2_TEX_Z_FILTER_MODE_LINEAR,
							view.viewNumMips <= 1 || (flags & BGFX_SAMPLER_MIP_POINT) ? GX2_TEX_MIP_FILTER_MODE_POINT : GX2_TEX_MIP_FILTER_MODE_LINEAR);
						const float maxLod = float(view.viewNumMips-1);
						GX2InitSamplerLOD(&sampler, bx::min(maxLod, float(bind.m_lod.min)*0.25f),
							bind.m_lod.max == UINT8_MAX ? maxLod : bx::min(maxLod, float(bind.m_lod.max)*0.25f), 0);
						if ((flags & BGFX_SAMPLER_SAMPLE_STENCIL) || integer)
						{
							GX2InitSamplerXYFilter(&sampler, GX2_TEX_XY_FILTER_MODE_POINT, GX2_TEX_XY_FILTER_MODE_POINT, GX2_TEX_ANISO_RATIO_NONE);
							GX2InitSamplerZMFilter(&sampler, GX2_TEX_Z_FILTER_MODE_POINT, GX2_TEX_MIP_FILTER_MODE_POINT);
						}
						GX2InitSamplerDepthCompare(&sampler, s_compare[(flags & BGFX_SAMPLER_COMPARE_MASK) >> BGFX_SAMPLER_COMPARE_SHIFT]);
						GX2InitSamplerBorderType(&sampler, borderType);
						m_samplerKey = samplerKey;
					}
					if (stages & 2) GX2SetPixelSampler(&sampler, ii);
					if (stages & 1) GX2SetVertexSampler(&sampler, ii);
					m_boundTextures[ii] = &texture.texture; m_boundBindings[ii] = bind; m_boundDimensions[ii] = encodedDimension; m_boundStages[ii] = stages;
				}
				if (vertexBorderChanged)
				{
					if (borderType == GX2_TEX_BORDER_TYPE_VARIABLE) GX2SetVertexSamplerBorderColor(ii, color[0], color[1], color[2], color[3]);
					bx::memCopy(m_borderColors[0][ii], color, 16); m_borderValid[0] |= 1u << ii;
				}
				if (pixelBorderChanged)
				{
					if (borderType == GX2_TEX_BORDER_TYPE_VARIABLE) GX2SetPixelSamplerBorderColor(ii, color[0], color[1], color[2], color[3]);
					bx::memCopy(m_borderColors[1][ii], color, 16); m_borderValid[1] |= 1u << ii;
				}
				if (texture.image) touch(texture.image);
				if (dimension == TextureDimension::Dimension2DArray && texture.arrayImage) touch(texture.arrayImage);
			}
		}

		Window* windowFor(FrameBufferHandle _handle)
		{
			if (!isValid(_handle)) return &m_mainWindow;
			Window& window = m_frameBuffers[_handle.idx].window;
			return window.swapChain.nwh ? &window : NULL;
		}

		bool frameBufferAvailable(FrameBufferHandle _handle)
		{
			Window* window = windowFor(_handle);
			return !window || (!m_suspended && window->swapChain.nwh);
		}

		void releaseWindowColor(Window& _window)
		{
			if (_window.colorImage)
			{
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _window.colorImage->data, _window.colorImage->size);
				if (_window.auxiliary) GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _window.auxiliary->data, _window.auxiliary->size);
				retire(_window.colorImage); retire(_window.auxiliary);
			}
			retire(_window.resolveImage); _window.resolved = {};
			_window.color = {}; _window.colorDirty = false;
		}

		void releaseWindowDepth(Window& _window)
		{
			if (_window.depthImage)
			{
				GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, _window.depthImage->data, _window.depthImage->size);
				retire(_window.depthImage); retire(_window.hiZ);
			}
			_window.depth = {};
		}

		void suspend()
		{
			m_native.drain(); seal();
			discardWindow(m_mainWindow);
			for (FrameBuffer& fb : m_frameBuffers) discardWindow(fb.window);
			// Retired MEM1 images also belong to the foreground heap being returned.
			for (size_t ii = 0; ii < m_allocations.size();)
			{
				Block* block = m_allocations[ii];
				if (block->heap) discardBlock(block);
				else ++ii;
			}
			m_native.release();
			m_context = NULL; m_suspended = true;
		}

		void discardBlock(Block*& _block)
		{
			if (!_block) return;
			m_touched.erase(std::remove(m_touched.begin(), m_touched.end(), _block), m_touched.end());
			m_retired.erase(std::remove(m_retired.begin(), m_retired.end(), _block), m_retired.end());
			m_allocations.erase(std::find(m_allocations.begin(), m_allocations.end(), _block));
			freeBlock(_block); bx::deleteObject(g_allocator, _block); _block = NULL;
		}

		void discardWindow(Window& _window)
		{
			discardBlock(_window.colorImage); discardBlock(_window.auxiliary); discardBlock(_window.resolveImage);
			discardBlock(_window.depthImage); discardBlock(_window.hiZ);
			_window.color = {}; _window.depth = {}; _window.resolved = {};
			_window.colorDirty = false; _window.used = false;
		}

		void destroyWindow(Window& _window)
		{
			if (!m_suspended) { releaseWindowColor(_window); releaseWindowDepth(_window); }
			else discardWindow(_window);
			_window.used = false; _window.swapChain.nwh = NULL;
		}

		GX2ColorBuffer& windowColor(Window& _window) { return _window.color; }

		void updateWindows(const SwapChain& _main)
		{
			if (m_mainWindow.swapChain.height != _main.height) m_native.configureTV(_main.height);
			m_mainWindow.swapChain = _main;
			m_mainWindow.used = false;
			for (FrameBuffer& fb : m_frameBuffers) fb.window.used = false;
		}

		void prepareWindow(Window& _window)
		{
			const GX2AAMode aa = GX2AAMode(bx::min<uint32_t>((_window.swapChain.flags & BGFX_SWAP_CHAIN_MSAA_MASK) >> BGFX_SWAP_CHAIN_MSAA_SHIFT, GX2_AA_MODE8X));
			const uint32_t width = bx::max(1u, _window.swapChain.width);
			const uint32_t height = bx::max(1u, _window.swapChain.height);
			prepareWindowDepth(_window, width, height, aa);
			GX2ColorBuffer& color = _window.color;
			if (!_window.colorImage || color.surface.width != width || color.surface.height != height || color.surface.aa != aa)
			{
				releaseWindowColor(_window);
				color.surface.dim = aa == GX2_AA_MODE1X ? GX2_SURFACE_DIM_TEXTURE_2D : GX2_SURFACE_DIM_TEXTURE_2D_MSAA;
				color.surface.width = width; color.surface.height = height;
				color.surface.depth = 1; color.surface.mipLevels = 1;
				color.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8; color.surface.aa = aa;
				color.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
				color.surface.tileMode = GX2_TILE_MODE_DEFAULT;
				GX2CalcSurfaceSizeAndAlignment(&color.surface);
				_window.colorImage = allocateFrameBuffer(color.surface.imageSize, color.surface.alignment);
				DCInvalidateRange(_window.colorImage->data, _window.colorImage->size);
				color.surface.image = _window.colorImage->data; color.viewNumSlices = 1;
				if (aa != GX2_AA_MODE1X)
				{
					uint32_t bytes, alignment; GX2CalcColorBufferAuxInfo(&color, &bytes, &alignment);
					_window.auxiliary = allocateFrameBuffer(bytes, alignment);
					bx::memSet(_window.auxiliary->data, GX2_AUX_BUFFER_CLEAR_VALUE, bytes);
					DCFlushRange(_window.auxiliary->data, bytes);
					color.aaBuffer = _window.auxiliary->data; color.aaSize = bytes;
				}
				GX2InitColorBufferRegs(&color);
			}
		}

		void prepareWindowDepth(Window& _window, uint32_t width, uint32_t height, GX2AAMode aa)
		{
			if (isValid(_window.swapChain.depth) || _window.swapChain.formatDepthStencil == TextureFormat::Count)
			{
				if (_window.depthImage) releaseWindowDepth(_window);
				return;
			}
			uint32_t comp;
			const GX2SurfaceFormat format = textureFormat(_window.swapChain.formatDepthStencil, false, comp);
			GX2DepthBuffer& depth = _window.depth;
			if (!_window.depthImage || depth.surface.width != width || depth.surface.height != height
			|| depth.surface.format != format || depth.surface.aa != aa)
			{
				releaseWindowDepth(_window);
				depth.surface.dim = aa == GX2_AA_MODE1X ? GX2_SURFACE_DIM_TEXTURE_2D : GX2_SURFACE_DIM_TEXTURE_2D_MSAA;
				depth.surface.width = width; depth.surface.height = height;
				depth.surface.depth = 1; depth.surface.mipLevels = 1;
				depth.surface.format = format; depth.surface.aa = aa;
				depth.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
				depth.surface.tileMode = GX2_TILE_MODE_DEFAULT;
				GX2CalcSurfaceSizeAndAlignment(&depth.surface);
				_window.depthImage = allocateFrameBuffer(depth.surface.imageSize, depth.surface.alignment);
				DCInvalidateRange(_window.depthImage->data, _window.depthImage->size);
				depth.surface.image = _window.depthImage->data;
				depth.viewNumSlices = 1; depth.depthClear = 1.0f;
				uint32_t bytes, alignment; GX2CalcDepthBufferHiZInfo(&depth, &bytes, &alignment);
				if (bytes)
				{
					_window.hiZ = allocateFrameBuffer(bytes, alignment); DCInvalidateRange(_window.hiZ->data, bytes);
					depth.hiZPtr = _window.hiZ->data; depth.hiZSize = bytes;
				}
				GX2InitDepthBufferRegs(&depth); GX2InitDepthBufferHiZEnable(&depth, depth.hiZPtr != NULL);
			}
		}

		// Outside display lists, after debug draws, at presentation or first reader.
		// Keep the multisample source intact: later partial draws retain all samples.
		GX2ColorBuffer& presentColor(Window& _window)
		{ return _window.resolveImage ? _window.resolved : _window.color; }

		void resolveWindow(Window& _window)
		{
			if (!_window.colorDirty) return;
			GX2ColorBuffer source = _window.color;
			GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, source.surface.image, source.surface.imageSize);
			if (source.surface.aa != GX2_AA_MODE1X)
			{
				GX2ColorBuffer& target = _window.resolved;
				if (!_window.resolveImage)
				{
					target.surface = source.surface; target.surface.aa = GX2_AA_MODE1X;
					target.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
					GX2CalcSurfaceSizeAndAlignment(&target.surface);
					_window.resolveImage = allocateFrameBuffer(target.surface.imageSize, target.surface.alignment);
					DCInvalidateRange(_window.resolveImage->data, _window.resolveImage->size);
					target.surface.image = _window.resolveImage->data; target.viewNumSlices = 1;
					GX2InitColorBufferRegs(&target);
				}
				GX2Surface destination = target.surface;
				if (_window.swapChain.flags & BGFX_SWAP_CHAIN_SRGB_BACKBUFFER)
				{
					source.surface.format = destination.format = GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8;
					GX2InitColorBufferRegs(&source);
				}
				GX2ResolveAAColorBuffer(&source, &destination, 0, 0);
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, destination.image, destination.imageSize);
				touch(_window.colorImage); touch(_window.auxiliary); touch(_window.resolveImage);
				restore();
			}
			_window.colorDirty = false;
		}

		bool preparePresent()
		{
			bool present = false;
			if (m_mainWindow.used) { resolveWindow(m_mainWindow); present = true; }
			for (FrameBuffer& fb : m_frameBuffers)
				if (fb.window.used) { resolveWindow(fb.window); present = true; }
			return present;
		}

		void presentWindow(Window& _window, bool _independentDrc)
		{
			GX2_PROFILE_SCOPE(Present);
			if (!_window.used) return;
			const Gx2PlatformData& platform = *(const Gx2PlatformData*)_window.swapChain.nwh;
			GX2ColorBuffer color = presentColor(_window);
			if (_window.swapChain.flags & BGFX_SWAP_CHAIN_SRGB_BACKBUFFER)
			{
				color.surface.format = GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8; GX2InitColorBufferRegs(&color);
			}
			GX2CopyColorBufferToScanBuffer(&color, platform.display ? GX2_SCAN_TARGET_DRC : GX2_SCAN_TARGET_TV);
			if (!platform.display && !_independentDrc) GX2CopyColorBufferToScanBuffer(&color, GX2_SCAN_TARGET_DRC);
			if (_window.colorImage) touch(_window.colorImage);
			if (_window.resolveImage) touch(_window.resolveImage);
			_window.used = false;
		}


		void setScissor(const Rect& _rect)
		{
			GX2_PROFILE_SCOPE(Scissor);
			if (0 != __builtin_memcmp(&m_cachedScissor, &_rect, sizeof(Rect)))
			{
				GX2_PROFILE_ADD(ScissorSets, 1);
				GX2SetScissor(_rect.m_x, _rect.m_y, _rect.m_width, _rect.m_height);
				m_cachedScissor = _rect;
			}
		}

		void viewport(const View& _view)
		{
			const Rect& rect = _view.m_rect;
			GX2SetViewport(rect.m_x, rect.m_y, rect.m_width, rect.m_height, 2*_view.m_minDepth-_view.m_maxDepth, _view.m_maxDepth);
		}

		void depthClearValues(Texture& _texture, const GX2DepthBuffer& _depth)
		{
			_texture.depthClear = _depth.depthClear; _texture.stencilClear = _depth.stencilClear;
			if (_texture.external && _texture.external->depth)
			{
				_texture.external->depth->depthClear = _depth.depthClear;
				_texture.external->depth->stencilClear = _depth.stencilClear;
			}
		}

		void clear(const View& _view, const ClearQuad& _quad, const float _palette[][4])
		{
			const Clear& clear = _view.m_clear;
			if (0 == (clear.m_flags & BGFX_CLEAR_MASK)) return;
			float colors[kColorTargets][4] = {};
			uint32_t colorMask = 0, integerMask = 0;
			if (clear.m_flags & BGFX_CLEAR_COLOR)
				for (uint32_t ii = 0; ii < m_numColors; ++ii)
				{
					if ((clear.m_flags & BGFX_CLEAR_COLOR_USE_PALETTE) && clear.m_index[ii] == UINT8_MAX) continue;
					const uint32_t type = uint32_t(m_colors[ii].surface.format) & 0x300;
					const bool integer = type == 0x100 || type == 0x300;
					getClearColor(colors[ii], clear, _palette, ii, integer);
					if (integer) integerMask |= 1u << ii;
					colorMask |= 1u << ii;
				}
			const GX2ClearFlags depthFlags = GX2ClearFlags(((clear.m_flags & BGFX_CLEAR_DEPTH) && !m_depthReadOnly ? GX2_CLEAR_FLAGS_DEPTH : 0) | ((clear.m_flags & BGFX_CLEAR_STENCIL) && !m_stencilReadOnly ? GX2_CLEAR_FLAGS_STENCIL : 0));
			const Rect& rect = _view.m_clippedRect;
			if (!integerMask && rect.m_x == 0 && rect.m_y == 0 && rect.m_width == m_targetWidth && rect.m_height == m_targetHeight)
			{
				reserveBatch(bx::countBits(colorMask)+(m_hasDepth && depthFlags ? 1u : 0u), 0);
				for (uint32_t ii = 0; ii < m_numColors; ++ii)
					if (colorMask & (1u << ii))
					{
						GX2ColorBuffer target = m_colors[ii];
						float* color = colors[ii];
						// Native clear implementations may rebuild regs: clear in storage order.
						if (!windowFor(m_currentFrameBuffer))
						{
							uint32_t colorIndex = 0;
							for (const Attachment& attachment : m_frameBuffers[m_currentFrameBuffer.idx].attachments)
							{
								const TextureFormat::Enum format = m_textures[attachment.handle.idx].format;
								if (bimg::isDepth(bimg::TextureFormat::Enum(format))) continue;
								if (colorIndex++ != ii) continue;
								if (format == TextureFormat::BGRA8 || format == TextureFormat::BGRA4 || format == TextureFormat::BGR5A1 || format == TextureFormat::B5G6R5) bx::swap(color[0], color[2]);
								else if (format == TextureFormat::A8) { color[0] = color[3]; color[1] = color[2] = color[3] = 0; }
								break;
							}
						}
						GX2InitColorBufferRegs(&target); GX2ClearColor(&target, color[0], color[1], color[2], color[3]);
					}
				if (m_hasDepth && depthFlags)
				{
					GX2ClearDepthStencilEx(&m_activeDepth, clear.m_depth, clear.m_stencil, depthFlags);
					if (depthFlags & GX2_CLEAR_FLAGS_DEPTH) m_activeDepth.depthClear = clear.m_depth;
					if (depthFlags & GX2_CLEAR_FLAGS_STENCIL) m_activeDepth.stencilClear = clear.m_stencil;
					Window* window = windowFor(m_currentFrameBuffer);
					if (!window)
						for (const Attachment& attachment : m_frameBuffers[m_currentFrameBuffer.idx].attachments)
						{
							Texture& texture = m_textures[attachment.handle.idx];
							if (!bimg::isDepth(bimg::TextureFormat::Enum(texture.format))) continue;
							depthClearValues(texture, m_activeDepth);
						}
					else if (isValid(window->swapChain.depth))
					{
						Texture& texture = m_textures[window->swapChain.depth.idx];
						depthClearValues(texture, m_activeDepth);
					}
					else window->depth = m_activeDepth;
				}
				restore(); bindFrameBuffer(m_currentFrameBuffer); viewport(_view);
			}
			else
			{
				const Rect& viewportRect = _view.m_rect;
				GX2SetViewport(viewportRect.m_x, viewportRect.m_y, viewportRect.m_width, viewportRect.m_height, -1, 1);
				setScissor(rect);
				Program& program = m_programs[_quad.m_program[bx::max<uint32_t>(1, m_numColors)-1].idx];
				const UniformRegInfo* colorUniform = m_uniformReg.find("bgfx_clear_color");
				const UniformRegInfo* depthUniform = m_uniformReg.find("bgfx_clear_depth");
				if (integerMask)
				{
					const DepthControl noBias = {}; depthControl(noBias);
					GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE,
						GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
						GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
					GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
					GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE); channelMasks(15, 1);
					GX2SetAlphaToMask(FALSE, GX2_ALPHA_TO_MASK_MODE_NON_DITHERED);
					GX2SetAAMask(0xff, 0xff, 0xff, 0xff); m_sampleMask = UINT32_MAX;
			m_rasterMode = UINT32_MAX; rasterSamples(true);
			m_restartSize = UINT32_MAX; primitiveRestart(GX2_PRIMITIVE_MODE_TRIANGLES, 0);
					const float depth[4] = {clear.m_depth, 0, 0, 0}; updateUniform(depthUniform->m_handle.idx, depth, 16);
					for (uint32_t ii = 0; ii < m_numColors; ++ii)
						if (integerMask & (1u << ii))
						{
							GX2SetColorBuffer(&m_colors[ii], GX2_RENDER_TARGET_0);
							Program& typed = m_programs[m_mipGen->m_program[(uint32_t(m_colors[ii].surface.format) & 0x300) == 0x100 ? 12 : 13].idx];
							updateUniform(colorUniform->m_handle.idx, colors[ii], 16); uniformsBegin(typed); uniformsEnd(typed);
							RenderDraw draw; draw.clear(); draw.m_streamMask = 0; vertexInput(typed, draw);
							GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, 1);
						}
					for (uint32_t ii = 0; ii < m_numColors; ++ii) GX2SetColorBuffer(&m_colors[ii], GX2RenderTarget(ii));
					m_cachedState = UINT64_MAX;
				}
				const uint32_t floatMask = colorMask & ~integerMask;
				if (floatMask || depthFlags)
				{
					updateUniform(colorUniform->m_handle.idx, colors, sizeof(colors));
					float depth[4] = { clear.m_depth, 0, 0, 0 }; updateUniform(depthUniform->m_handle.idx, depth, 16);
					uniformsBegin(program); uniformsEnd(program);
					RenderDraw draw; draw.clear(); draw.m_streamMask = 1;
					draw.m_stream[0].m_handle = _quad.m_vb; draw.m_stream[0].m_layoutHandle = _quad.m_layout; vertexInput(program, draw);
					const uint32_t stencil = BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_OP_FAIL_S_REPLACE | BGFX_STENCIL_OP_FAIL_Z_REPLACE | BGFX_STENCIL_OP_PASS_Z_REPLACE | BGFX_STENCIL_FUNC_REF(clear.m_stencil);
					const DepthControl noBias = {}; depthControl(noBias);
					state((floatMask ? BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A : 0)
						| (clear.m_flags & BGFX_CLEAR_DEPTH ? BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_ALWAYS : 0),
						clear.m_flags & BGFX_CLEAR_STENCIL ? stencil : 0, 0);
					channelMasks(15, floatMask);
					GX2SetAAMask(0xff, 0xff, 0xff, 0xff); m_sampleMask = UINT32_MAX;
			m_rasterMode = UINT32_MAX; rasterSamples(true);
			m_restartSize = UINT32_MAX; primitiveRestart(GX2_PRIMITIVE_MODE_TRIANGLES, 0);
					GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
				}
				viewport(_view); m_cachedState = UINT64_MAX;
			}
			m_colorWritten |= colorMask; m_depthWritten |= m_hasDepth && depthFlags;
		}

		GX2SurfaceFormat computeImageFormat(uint32_t _format)
		{
			switch (_format)
			{
			case 1: return GX2_SURFACE_FORMAT_FLOAT_R32_G32_B32_A32;
			case 2: return GX2_SURFACE_FORMAT_FLOAT_R16_G16_B16_A16;
			case 3: return GX2_SURFACE_FORMAT_FLOAT_R32;
			case 5: return GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
			case 8: return GX2_SURFACE_FORMAT_UINT_R32_G32_B32_A32;
			case 9: return GX2_SURFACE_FORMAT_UINT_R32;
			case 10: return GX2_SURFACE_FORMAT_FLOAT_R16_G16;
			case 11: return GX2_SURFACE_FORMAT_FLOAT_R32_G32;
			default: return GX2_SURFACE_FORMAT_FLOAT_R32_G32_B32_A32; // Quantized float4 intermediates: R16F/RG8/R8.
			}
		}

		uint32_t computeLayout(const Shader& _shader, const RenderBind& _bind, std::vector<ComputeOutput>& _outputs)
		{
			uint32_t bytes = 0;
			for (const ComputeResource& resource : _shader.resources)
			{
				if (!resource.write) continue;
				const Binding& bind = _bind.m_bind[resource.binding];
				ComputeOutput output;
				output.binding = resource.binding; output.elementBytes = resource.elementBytes;
				if (resource.kind == 0)
				{
					const Buffer& buffer = bind.m_type == Binding::IndexBuffer ? m_indexBuffers[bind.m_idx] : m_vertexBuffers[bind.m_idx];
					output.size = bx::min(bind.m_size, buffer.size-bind.m_offset);
				}
				else
				{
					const GX2Surface& surface = m_textures[bind.m_idx].texture.surface;
					const uint32_t layers = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D ? bx::max(1u, surface.depth >> bind.m_firstMip) : surface.depth;
					output.layers = bx::min<uint32_t>(bind.m_numLayers, layers-bind.m_firstLayer);
					output.surface = transferLinear(computeImageFormat(resource.format), bx::max(1u, surface.width >> bind.m_firstMip), bx::max(1u, surface.height >> bind.m_firstMip));
					output.size = output.surface.imageSize*output.layers;
				}
				output.offset = bx::alignUp(bytes, bx::max(256u, output.surface.alignment));
				bytes = output.offset+output.size;
				_outputs.push_back(output);
			}
			return bx::alignUp(bytes, 256);
		}

		struct GraphicsImage
		{
			Binding binding;
			ComputeOutput output;
			// 0: canonical only, 1: matching export copy, 2: pending export writes,
			// 3: published export writes not yet materialized in canonical storage.
			uint8_t authority = 0;
		};

		struct GraphicsImages
		{
			std::vector<GraphicsImage> images;
			Block* layout = NULL;
			uint32_t params[128] = {};
		};

		GraphicsImage* graphicsImage(GraphicsImages& _images, const Binding& _bind)
		{
			for (GraphicsImage& image : _images.images)
				if (image.binding.m_idx == _bind.m_idx && image.binding.m_firstMip == _bind.m_firstMip
				&& image.binding.m_firstLayer == _bind.m_firstLayer && image.binding.m_numLayers == _bind.m_numLayers
				&& image.binding.m_format == _bind.m_format) return &image;
			return NULL;
		}

		uint32_t graphicsImageLayout(GraphicsImages& _images, const Program& _program, const RenderBind& _bind, uint32_t _bytes)
		{
			for (const Shader* shader : { _program.vertex, _program.fragment })
				for (const ComputeResource& resource : shader->resources)
				{
					const Binding& bind = _bind.m_bind[resource.binding];
					GraphicsImage* previous = graphicsImage(_images, bind);
					if (previous)
					{
						if (!previous->output.size && (resource.read || resource.write))
						{
							previous->output.offset = bx::alignUp(_bytes, bx::max(256u, previous->output.surface.alignment));
							previous->output.size = previous->output.surface.imageSize*previous->output.layers;
							_bytes = previous->output.offset+previous->output.size;
						}
						continue;
					}
					GraphicsImage image; image.binding = bind;
					const GX2Surface& surface = m_textures[bind.m_idx].texture.surface;
					const uint32_t layers = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D ? bx::max(1u, surface.depth >> bind.m_firstMip) : surface.depth;
					image.output.layers = bx::min<uint32_t>(bind.m_numLayers, layers-bind.m_firstLayer);
					image.output.elementBytes = resource.elementBytes;
					image.output.surface = transferLinear(computeImageFormat(resource.format), bx::max(1u, surface.width >> bind.m_firstMip), bx::max(1u, surface.height >> bind.m_firstMip));
					image.output.size = resource.read || resource.write ? image.output.surface.imageSize*image.output.layers : 0;
					image.output.offset = image.output.size ? bx::alignUp(_bytes, bx::max(256u, image.output.surface.alignment)) : 0;
					if (image.output.size) _bytes = image.output.offset+image.output.size;
					_images.images.push_back(image);
				}
			return _bytes;
		}

		void graphicsImagePublish(GraphicsImage& _image, Block* _exports)
		{
			if (_image.authority != 2) return;
			GX2Invalidate(GX2_INVALIDATE_MODE_EXPORT_BUFFER, (uint8_t*)_exports->data+_image.output.offset, _image.output.size);
			_image.authority = 3;
		}

		// Caller ends the batch/pass before native copies; one copy per canonical
		// consumer, never one copy per dependent UAV draw.
		void graphicsImageStore(GraphicsImage& _image, Block* _exports)
		{
			if (_image.authority < 2) return;
			graphicsImagePublish(_image, _exports);
			const ComputeOutput& output = _image.output;
			Texture& texture = m_textures[_image.binding.m_idx];
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, (uint8_t*)_exports->data+output.offset, output.size);
			const bool convert = output.surface.format != texture.texture.surface.format;
			if (convert) { restore(); beginBatch(); }
			for (uint32_t layer = 0; layer < output.layers; ++layer)
			{
				GX2Surface source = output.surface;
				source.image = (uint8_t*)_exports->data+output.offset+layer*source.imageSize;
				if (convert) computeImageConvert(texture, _image.binding, source, layer, false);
				else GX2CopySurface(&source, 0, 0, &texture.texture.surface, _image.binding.m_firstMip, _image.binding.m_firstLayer+layer);
			}
			if (convert) endBatch();
			transferWritten(texture, _image.binding.m_firstMip, _image.binding.m_firstLayer, output.layers);
			_image.authority = 1;
		}

		bool graphicsImagesConsume(GraphicsImages& _images, Block* _exports, uint16_t _texture = kInvalidHandle, bool _invalidate = false)
		{
			bool changed = false;
			for (GraphicsImage& image : _images.images)
			{
				if (_texture != kInvalidHandle && image.binding.m_idx != _texture) continue;
				if (image.authority >= 2)
				{
					if (!changed) { endBatch(); finishRenderPass(); changed = true; }
					graphicsImageStore(image, _exports);
				}
				if (_invalidate) image.authority = 0;
			}
			if (changed) { restore(); bindFrameBuffer(m_currentFrameBuffer); beginBatch(); }
			return changed;
		}

		bool graphicsImagesBind(GraphicsImages& _images, Block* _exports, const Program& _program, const RenderBind& _bind)
		{
			GX2_PROFILE_SCOPE(GraphicsImagesBind);
			if (_images.images.empty()) return false;
			bool restored = false;
			// Sampled aliases consume canonical texture storage.
			for (uint32_t unit = 0; unit < kTextureSamplers; ++unit)
				if (_bind.m_bind[unit].m_idx != kInvalidHandle && _bind.m_bind[unit].m_type == Binding::Texture
				&& (_program.vertex->samplerDimensions[unit] || _program.fragment->samplerDimensions[unit]))
					restored |= graphicsImagesConsume(_images, _exports, _bind.m_bind[unit].m_idx);
			if (_program.vertex->resources.empty() && _program.fragment->resources.empty()) return restored;
			uint32_t params[128] = {};
			bool copying = false;
			for (const Shader* shader : { _program.vertex, _program.fragment })
				for (const ComputeResource& resource : shader->resources)
				{
					const Binding& bind = _bind.m_bind[resource.binding];
					GraphicsImage& image = *graphicsImage(_images, bind);
					const ComputeOutput& output = image.output;
					if (!image.authority && (resource.read || resource.write))
					{
						if (!copying) { endBatch(); finishRenderPass(); copying = true; }
						// A different typed/subresource alias must first publish its writes.
						for (GraphicsImage& alias : _images.images)
							if (&alias != &image && alias.binding.m_idx == bind.m_idx) { graphicsImageStore(alias, _exports); alias.authority = 0; }
						Texture& texture = m_textures[bind.m_idx]; resolveTexture(texture);
						if (texture.image) touch(texture.image);
						const bool convert = output.surface.format != texture.texture.surface.format;
						if (convert) { restore(); beginBatch(); }
						for (uint32_t layer = 0; layer < output.layers; ++layer)
						{
							GX2Surface dest = output.surface;
							dest.image = (uint8_t*)_exports->data+output.offset+layer*dest.imageSize;
							if (convert) computeImageConvert(texture, bind, dest, layer, true);
							else GX2CopySurface(&texture.texture.surface, bind.m_firstMip, bind.m_firstLayer+layer, &dest, 0, 0);
						}
						if (convert) endBatch();
						GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, (uint8_t*)_exports->data+output.offset, output.size);
						image.authority = 1;
					}
					graphicsImagePublish(image, _exports);
					uint32_t* entry = params+resource.binding*8;
					entry[0] = bx::toLittleEndian(output.offset/4); entry[1] = bx::toLittleEndian(output.size/4);
					entry[2] = bx::toLittleEndian(output.surface.pitch*output.elementBytes/4);
					entry[3] = bx::toLittleEndian(output.surface.imageSize/4);
					entry[4] = bx::toLittleEndian(output.surface.width); entry[5] = bx::toLittleEndian(output.surface.height);
					entry[6] = bx::toLittleEndian(output.layers);
				}
			if (copying) { restore(); bindFrameBuffer(m_currentFrameBuffer); beginBatch(); restored = true; }
			if (_exports && (m_boundExportData != _exports->data || m_boundExportSize != _exports->size))
			{
				endBatch(); bindComputeExports(_exports); beginBatch();
			}
			if (!_images.layout || bx::memCmp(params, _images.params, sizeof(params)))
			{
				retire(_images.layout);
				_images.layout = allocate(sizeof(params), 256);
				bx::memCopy(_images.layout->data, params, sizeof(params)); bx::memCopy(_images.params, params, sizeof(params));
				GX2Invalidate(GX2InvalidateMode(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), _images.layout->data, sizeof(params));
			}
			if (!_program.vertex->resources.empty()) GX2SetVertexUniformBlock(15, sizeof(params), _images.layout->data);
			if (!_program.fragment->resources.empty()) GX2SetPixelUniformBlock(15, sizeof(params), _images.layout->data);
			touch(_images.layout); if (_exports) touch(_exports);
			return restored;
		}

		void graphicsImagesWritten(GraphicsImages& _images, const Program& _program, const RenderBind& _bind)
		{
			GX2_PROFILE_SCOPE(GraphicsImagesWritten);
			for (const Shader* shader : { _program.vertex, _program.fragment })
				for (const ComputeResource& resource : shader->resources)
					if (resource.write) graphicsImage(_images, _bind.m_bind[resource.binding])->authority = 2;
		}

		// Convert a CPU-owned raw buffer once, so every byte of its canonical
		// allocation has one byte representation. Ownership stays in gpuWritten.
		void computeBufferNative(Buffer& _buffer, bool _index)
		{
			if (_buffer.littleEndian) return;
			Block* source = gpuBufferAllocate(_buffer.size);
			DCInvalidateRange(source->data, source->size);
			gpuCopyBytes(_buffer.block, 0, source, 0, _buffer.size);
			gpuCopyWritten(source);
			gpuBufferDestination(_buffer);
			restore(); beginBatch();
			for (uint32_t offset = 0; offset < _buffer.size;)
			{
				const uint32_t bytes = bx::min<uint32_t>(_buffer.size-offset, 8192*8192);
				const uint32_t width = bx::min<uint32_t>(2048, bx::alignUp((bytes+3)/4, 64));
				GX2Texture input = {};
				input.surface = transferLinear(GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, width, (bytes+width*4-1)/(width*4));
				input.surface.image = (uint8_t*)source->data+offset;
				input.viewNumMips = input.viewNumSlices = 1;
				input.compMap = _index && !(_buffer.flags & BGFX_BUFFER_INDEX32) ? GX2_COMP_MAP(1, 0, 3, 2) : GX2_COMP_MAP(3, 2, 1, 0);
				GX2InitTextureRegs(&input);
				GX2ColorBuffer dest = {}; dest.surface = input.surface;
				dest.surface.image = (uint8_t*)_buffer.block->data+offset;
				dest.viewNumSlices = 1; GX2InitColorBufferRegs(&dest);
				const float params[4] = {1.0f/width, 1.0f/input.surface.height, 0, 0};
				fullscreen(m_programs[m_mipGen->m_program[0].idx], input, &dest, NULL, width, input.surface.height, params);
				offset += bytes;
			}
			endBatch(); _buffer.littleEndian = true; gpuBufferWritten(_buffer); retire(source);
		}

		// A float4 intermediate needs a real format conversion at the imageStore
		// boundary. The compiler has already rounded stores to the image format.
		void computeImageConvert(Texture& _texture, const Binding& _bind, GX2Surface _linear, uint32_t _layer, bool _toExport)
		{
			GX2Texture input = {};
			GX2ColorBuffer dest = {};
			uint32_t program = 0;
			if (_toExport)
			{
				input = _texture.texture;
				input.viewFirstMip = _bind.m_firstMip; input.viewNumMips = 1;
				input.viewFirstSlice = _bind.m_firstLayer+_layer; input.viewNumSlices = 1;
				const bool volume = input.surface.dim == GX2_SURFACE_DIM_TEXTURE_3D;
				const bool array = input.surface.dim != GX2_SURFACE_DIM_TEXTURE_2D && !volume;
				if (volume) { input.viewFirstSlice = 0; input.viewNumSlices = input.surface.depth; }
				if (input.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE)
				{
					input = cubeArrayView(_texture); input.viewFirstMip = _bind.m_firstMip;
					input.viewNumMips = 1; input.viewFirstSlice = _bind.m_firstLayer+_layer; input.viewNumSlices = 1;
				}
				program = volume ? 2 : array ? 1 : 0;
				dest.surface = _linear;
			}
			else
			{
				input.surface = _linear; input.viewNumMips = input.viewNumSlices = 1;
				input.compMap = GX2_COMP_MAP(0, 1, 2, 3);
				dest.surface = _texture.texture.surface; dest.viewMip = _bind.m_firstMip;
				dest.viewFirstSlice = _bind.m_firstLayer+_layer;
			}
			GX2InitTextureRegs(&input); dest.viewNumSlices = 1; GX2InitColorBufferRegs(&dest);
			const uint32_t depth = bx::max(1u, _texture.texture.surface.depth >> _bind.m_firstMip);
			const float params[4] = {1.0f/_linear.width, 1.0f/_linear.height, (float(_bind.m_firstLayer+_layer)+0.5f)/depth, 0};
			fullscreen(m_programs[m_mipGen->m_program[program].idx], input, &dest, NULL, _linear.width, _linear.height, params);
		}

		void computeSampled(const Binding& _bind, uint32_t _unit, const float _palette[][4])
		{
			Texture& texture = m_textures[_bind.m_idx];
			GX2Texture view = texture.texture;
			const Shader& shader = *m_boundProgram->compute;
			const uint8_t dimension = shader.samplerDimensions[_unit];
			if (view.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE && dimension == TextureDimension::Dimension2DArray+1) view = cubeArrayView(texture);
			view.viewFirstMip = _bind.m_firstMip;
			view.viewNumMips = _bind.m_type == Binding::Image ? 1 : bx::min<uint32_t>(_bind.m_numMips, view.surface.mipLevels-view.viewFirstMip);
			const uint32_t faces = view.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE && _bind.m_type == Binding::Texture ? 6 : 1;
			view.viewFirstSlice = uint32_t(_bind.m_firstLayer)*faces;
			view.viewNumSlices = bx::min<uint32_t>(uint32_t(_bind.m_numLayers)*faces, view.surface.depth-view.viewFirstSlice);
			if (view.surface.dim == GX2_SURFACE_DIM_TEXTURE_3D && _bind.m_type == Binding::Image)
			{
				view.viewFirstSlice = 0;
				view.viewNumSlices = view.surface.depth;
			}
			if (_bind.m_type == Binding::Image) view.surface.format = textureFormat(TextureFormat::Enum(_bind.m_format), false, view.compMap);
			const uint32_t flags = _bind.m_samplerFlags & BGFX_SAMPLER_INTERNAL_DEFAULT ? uint32_t(texture.flags) : _bind.m_samplerFlags;
			if (_bind.m_type == Binding::Texture && (texture.flags & BGFX_TEXTURE_SRGB_MUTABLE)) view.surface.format = textureFormat(texture.format, 0 != (flags & BGFX_SAMPLER_SRGB), view.compMap);
			GX2InitTextureRegs(&view); GX2SetComputeTexture(&view, _unit);
			static const GX2TexClampMode clamps[] = { GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_CLAMP_MODE_MIRROR, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_CLAMP_BORDER };
			GX2Sampler sampler;
			GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);
			GX2InitSamplerClamping(&sampler, clamps[(flags & BGFX_SAMPLER_U_MASK) >> BGFX_SAMPLER_U_SHIFT], clamps[(flags & BGFX_SAMPLER_V_MASK) >> BGFX_SAMPLER_V_SHIFT], clamps[(flags & BGFX_SAMPLER_W_MASK) >> BGFX_SAMPLER_W_SHIFT]);
			const bool integer = (uint32_t(view.surface.format) & 0x300) == 0x100 || (uint32_t(view.surface.format) & 0x300) == 0x300;
			GX2InitSamplerXYFilter(&sampler, integer || (flags & BGFX_SAMPLER_MAG_POINT) ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR,
				integer || (flags & BGFX_SAMPLER_MIN_POINT) ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR,
				flags & (BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC) ? m_anisotropy : GX2_TEX_ANISO_RATIO_NONE);
			GX2InitSamplerZMFilter(&sampler, integer || (flags & BGFX_SAMPLER_MIN_POINT) ? GX2_TEX_Z_FILTER_MODE_POINT : GX2_TEX_Z_FILTER_MODE_LINEAR,
				view.viewNumMips <= 1 ? GX2_TEX_MIP_FILTER_MODE_NONE : integer || (flags & BGFX_SAMPLER_MIP_POINT) ? GX2_TEX_MIP_FILTER_MODE_POINT : GX2_TEX_MIP_FILTER_MODE_LINEAR);
			const float maxLod = float(view.viewNumMips-1);
			GX2InitSamplerLOD(&sampler, _bind.m_type == Binding::Image ? 0 : bx::min(maxLod, float(_bind.m_lod.min)*0.25f),
				_bind.m_type == Binding::Image || _bind.m_lod.max == UINT8_MAX ? maxLod : bx::min(maxLod, float(_bind.m_lod.max)*0.25f), 0);
			GX2InitSamplerDepthCompare(&sampler, s_compare[(flags & BGFX_SAMPLER_COMPARE_MASK) >> BGFX_SAMPLER_COMPARE_SHIFT]);
			GX2InitSamplerBorderType(&sampler, GX2_TEX_BORDER_TYPE_VARIABLE);
			const float* color = _palette[(flags & BGFX_SAMPLER_BORDER_COLOR_MASK) >> BGFX_SAMPLER_BORDER_COLOR_SHIFT];
			const bool border = (flags & BGFX_SAMPLER_U_MASK) == BGFX_SAMPLER_U_BORDER
				|| (flags & BGFX_SAMPLER_V_MASK) == BGFX_SAMPLER_V_BORDER || (flags & BGFX_SAMPLER_W_MASK) == BGFX_SAMPLER_W_BORDER;
			if (border && (!(m_borderValid[0] & (1u << _unit)) || bx::memCmp(m_borderColors[0][_unit], color, 16)))
			{
				GX2SetComputeSamplerBorderColor(_unit, color[0], color[1], color[2], color[3]);
				bx::memCopy(m_borderColors[0][_unit], color, 16); m_borderValid[0] |= 1u << _unit;
			}
			GX2SetComputeSampler(&sampler, _unit);
			if (texture.image) touch(texture.image);
		}

		uint32_t indirectRecords(IndirectBufferHandle _handle, uint32_t _start, uint32_t _num)
		{
			return bx::min(_num, m_vertexBuffers[_handle.idx].size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE-_start);
		}

		uint32_t indirectChunk(bool _indexed)
		{
			// Native IB size is twenty bits in DWORDs, padded to four DWORDs.
			return (UINT32_C(0xffffc)*4)/(_indexed ? 96 : 80);
		}

		void bindComputeExports(Block* _exports)
		{
			if (_exports && (m_boundExportData != _exports->data || m_boundExportSize != _exports->size))
			{
				GX2SetShaderExportBuffer(_exports->data, _exports->size);
				m_boundExportData = _exports->data; m_boundExportSize = _exports->size;
			}
		}

		Block* lowerIndirect(uint32_t _mode, IndirectBufferHandle _args, IndexBufferHandle _count,
			uint32_t _params[3][4], uint32_t _bytes, Block* _exports)
		{
			static const uint8_t* const data[] = {cs_gx2_indirect_gx2, cs_gx2_indirect_indexed_gx2, cs_gx2_indirect_dispatch_gx2};
			Shader& shader = m_indirectShaders[_mode];
			if (!shader.header) loadShader(shader, data[_mode], true);
			Block* commands = gpuBufferAllocate(_bytes);
			DCInvalidateRange(commands->data, commands->size); // GPU-only; never CPU-write the payload.
			_params[2][0] = OSEffectiveToPhysical(uint32_t(uintptr_t(commands->data)));
			GX2SetShaderMode(GX2_SHADER_MODE_COMPUTE_SHADER);
			GX2SetComputeShader((const GX2ComputeShader*)shader.header->data);
			Binding args;
			args.setBuffer(VertexBufferHandle{_args.idx}, Access::Read);
			readonlyBuffer(args, 0, &shader);
			if (_mode != 2)
			{
				// The compiler hoists this read even when count suppression is disabled.
				Binding count = args;
				if (isValid(_count)) count.setIndexBuffer(_count, Access::Read);
				readonlyBuffer(count, 1, &shader);
			}
			Block* layout = allocate(512, 256);
			bx::memSet(layout->data, 0, 512);
			((uint32_t*)layout->data)[2*8+1] = bx::toLittleEndian(_bytes/4);
			GX2Invalidate(GX2InvalidateMode(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), layout->data, 512);
			GX2SetComputeUniformBlock(15, 512, layout->data); retire(layout);
			const GX2ComputeShader* native = (const GX2ComputeShader*)shader.header->data;
			const GX2UniformVar* var = GX2GetComputeUniformVar(native, "u_indirectLower");
			Block* uniform = allocate(shader.uniformSize, 256);
			bx::memSet(uniform->data, 0, shader.uniformSize);
			uint32_t* dst = (uint32_t*)uniform->data+var->offset;
			for (uint32_t ii = 0; ii < 12; ++ii) dst[ii] = bx::toLittleEndian((&_params[0][0])[ii]);
			GX2Invalidate(GX2InvalidateMode(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), uniform->data, shader.uniformSize);
			GX2SetComputeUniformBlock(0, shader.uniformSize, uniform->data); retire(uniform);
			Block* params = allocate(sizeof(GX2DispatchParams), GX2_DISPATCH_PARAMS_ALIGNMENT);
			GX2DispatchParams* dispatch = (GX2DispatchParams*)params->data;
			dispatch->groupCountX = (_params[0][1]+63)/64;
			dispatch->groupCountY = dispatch->groupCountZ = 1; dispatch->padding = 0;
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU, params->data, sizeof(*dispatch));
			GX2DispatchCompute(dispatch); retire(params);
			touch(shader.header); touch(shader.program);
			GX2Invalidate(GX2_INVALIDATE_MODE_EXPORT_BUFFER, _exports->data, _bytes);
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _exports->data, _bytes); // Actual copy source reader.
			GX2SetShaderMode(kGraphicsShaderMode);
			gpuCopyBytes(_exports, 0, commands, 0, _bytes);
			GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, commands->data, commands->size);
			if (_mode == 2) GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, commands->data, 16); // RID147.
			return commands;
		}

		void callIndirect(Block* _commands, uint32_t _offset, uint32_t _bytes)
		{
			// Mesa r600_emit_pfp_sync_me: ME publication precedes PFP IB fetch.
			// Isolate the marker from GPU payload copies and their CPU cache lines.
			Block* ready = allocate(64);
			*(uint32_t*)ready->data = 0;
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU, ready->data, 4);
			const uint32_t marker = OSEffectiveToPhysical(uint32_t(uintptr_t(ready->data)));
			alignas(64) const uint32_t sync[] = {
				UINT32_C(0xc0033d00), marker, UINT32_C(0x00040000), 1, 0,
				UINT32_C(0xc0053c00), UINT32_C(0x00000115), marker, 0, 1, UINT32_MAX, 4,
			};
			GX2CopyDisplayList(sync, sizeof(sync));
			GX2CallDisplayList((const uint8_t*)_commands->data+_offset, _bytes);
			retire(ready);
		}

		void dispatchCompute(Program& _program, const RenderCompute& _compute, const RenderBind& _bind, Block* _exports, const float _palette[][4], ViewState& _viewState, const Frame* _frame, uint16_t _view, const View& _renderView, Block* _commands = NULL)
		{
			if (isValid(_compute.m_indirectBuffer) && !_commands)
			{
				const GX2ComputeShader* native = (const GX2ComputeShader*)_program.compute->header->data;
				const uint32_t waves = (native->workGroupSizeX*native->workGroupSizeY*native->workGroupSizeZ+63)/64;
				const uint32_t num = indirectRecords(_compute.m_indirectBuffer, _compute.m_startIndirect, _compute.m_numIndirect);
				for (uint32_t ii = 0; ii < num; ++ii)
				{
					uint32_t params[3][4] = {{_compute.m_startIndirect+ii, 1, 0, 0}, {}, {0, waves, 0, 0}};
					Block* commands = lowerIndirect(2, _compute.m_indirectBuffer, BGFX_INVALID_HANDLE, params, 144, _exports);
					// Preserve normal seed/publish/materialize dependencies for each record.
					dispatchCompute(_program, _compute, _bind, _exports, _palette, _viewState, _frame, _view, _renderView, commands);
					retire(commands);
				}
				return;
			}
			Shader& shader = *_program.compute;
			std::vector<ComputeOutput> outputs;
			computeLayout(shader, _bind, outputs);
			// Canonical inputs must be materialized before entering compute mode.
			for (uint32_t unit = 0; unit < kTextureSamplers; ++unit)
			{
				const Binding& bind = _bind.m_bind[unit];
				if (bind.m_idx == kInvalidHandle) continue;
				if (bind.m_type == Binding::Texture || bind.m_type == Binding::Image) resolveTexture(m_textures[bind.m_idx]);
			}
			for (const ComputeOutput& output : outputs)
			{
				const Binding& bind = _bind.m_bind[output.binding];
				if (bind.m_type != Binding::Image)
				{
					Buffer& buffer = bind.m_type == Binding::IndexBuffer ? m_indexBuffers[bind.m_idx] : m_vertexBuffers[bind.m_idx];
					computeBufferNative(buffer, bind.m_type == Binding::IndexBuffer);
					gpuCopyBytes(buffer.block, bind.m_offset, _exports, output.offset, output.size);
				}
				else
				{
					Texture& texture = m_textures[bind.m_idx];
					const bool convert = output.surface.format != texture.texture.surface.format;
					if (convert) { restore(); beginBatch(); }
					for (uint32_t layer = 0; layer < output.layers; ++layer)
					{
						GX2Surface dest = output.surface; dest.image = (uint8_t*)_exports->data+output.offset+layer*dest.imageSize;
						if (convert) computeImageConvert(texture, bind, dest, layer, true);
						else GX2CopySurface(&texture.texture.surface, bind.m_firstMip, bind.m_firstLayer+layer, &dest, 0, 0);
					}
					if (convert) endBatch();
				}
			}
			// All seed copies precede the producer. No CPU writeback of export bytes.
			if (!outputs.empty()) GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _exports->data, _exports->size);
			// Native surface copies disable state shadowing and clobber pipeline state.
			GX2SetContextState(m_context);
			GX2SetShaderMode(GX2_SHADER_MODE_COMPUTE_SHADER);
			GX2SetComputeShader((const GX2ComputeShader*)shader.header->data);
			m_boundProgram = &_program;
			for (uint32_t unit = 0; unit < kTextureSamplers; ++unit)
			{
				const Binding& bind = _bind.m_bind[unit];
				if (bind.m_idx == kInvalidHandle) continue;
				if (bind.m_type == Binding::Texture || bind.m_type == Binding::Image)
				{
					bool read = shader.samplerDimensions[unit] != 0;
					for (const ComputeResource& resource : shader.resources) if (resource.binding == unit && resource.read) read = true;
					if (read) computeSampled(bind, unit, _palette);
				}
				else if (shader.rawSrvMask & (1u << unit)) readonlyBuffer(bind, unit);
			}
			Block* layout = allocate(512, 256);
			bx::memSet(layout->data, 0, 512);
			// Queries need metadata for readonly and size-only bindings too.
			for (const ComputeResource& resource : shader.resources)
			{
				const Binding& bind = _bind.m_bind[resource.binding];
				uint32_t* entry = (uint32_t*)layout->data+resource.binding*8;
				if (resource.kind == 0)
				{
					const Buffer& buffer = bind.m_type == Binding::IndexBuffer ? m_indexBuffers[bind.m_idx] : m_vertexBuffers[bind.m_idx];
					entry[1] = bx::toLittleEndian(bx::min(bind.m_size, buffer.size-bind.m_offset)/4);
					continue;
				}
				const GX2Surface& surface = m_textures[bind.m_idx].texture.surface;
				const uint32_t layers = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D ? bx::max(1u, surface.depth >> bind.m_firstMip) : surface.depth;
				entry[4] = bx::toLittleEndian(bx::max(1u, surface.width >> bind.m_firstMip));
				entry[5] = bx::toLittleEndian(bx::max(1u, surface.height >> bind.m_firstMip));
				entry[6] = bx::toLittleEndian(bx::min<uint32_t>(bind.m_numLayers, layers-bind.m_firstLayer));
				if (surface.dim == GX2_SURFACE_DIM_TEXTURE_3D) entry[7] = bx::toLittleEndian(uint32_t(bind.m_firstLayer));
			}
			for (const ComputeOutput& output : outputs)
			{
				uint32_t* entry = (uint32_t*)layout->data+output.binding*8;
				entry[0] = bx::toLittleEndian(output.offset/4);
				if (output.surface.width)
				{
					const uint32_t element = output.elementBytes;
					entry[1] = bx::toLittleEndian(output.size/4);
					entry[2] = bx::toLittleEndian(output.surface.pitch*element/4);
					entry[3] = bx::toLittleEndian(output.surface.imageSize/4);
				}
			}
			GX2Invalidate(GX2InvalidateMode(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), layout->data, 512);
			GX2SetComputeUniformBlock(15, 512, layout->data); retire(layout);
			const GX2ComputeShader* native = (const GX2ComputeShader*)shader.header->data;
			if (shader.uniformSize)
			{
				Block* uniform = allocate(shader.uniformSize, 256);
				for (uint32_t ii = 0; ii < native->initialValueCount; ++ii) writeFloats((uint8_t*)uniform->data+native->initialValues[ii].offset*4, native->initialValues[ii].value, 4);
				for (const Uniform& binding : shader.uniforms)
				{
					uint8_t* dst = (uint8_t*)uniform->data+binding.offset*4;
					const float* src = (const float*)m_uniforms[binding.handle.idx];
					if (binding.type == UniformType::Mat3)
						for (uint32_t ii = 0; ii < uint32_t(binding.count)*3; ++ii) writeFloats(dst+ii*16, src+ii*3, 3);
					else writeFloats(dst, src, binding.rows*4);
				}
				m_stageData[0] = (uint8_t*)uniform->data;
				_viewState.setPredefined<4>(this, _view, _renderView, _program, _frame, _compute);
				GX2Invalidate(GX2InvalidateMode(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), uniform->data, shader.uniformSize);
				GX2SetComputeUniformBlock(0, shader.uniformSize, uniform->data); retire(uniform);
			}
			if (_commands) callIndirect(_commands, 64, 80);
			else
			{
				Block* params = allocate(sizeof(GX2DispatchParams), GX2_DISPATCH_PARAMS_ALIGNMENT);
				GX2DispatchParams* dispatch = (GX2DispatchParams*)params->data;
				// The native command always launches 64 lanes per physical group. The
				// compiler folds adjacent waves into independent logical groups and
				// recovers the original X count from RID147 using this same factor.
				const uint32_t waves = (native->workGroupSizeX*native->workGroupSizeY*native->workGroupSizeZ+63)/64;
				dispatch->groupCountX = _compute.m_numX*waves; dispatch->groupCountY = _compute.m_numY; dispatch->groupCountZ = _compute.m_numZ; dispatch->padding = 0;
				GX2Invalidate(GX2_INVALIDATE_MODE_CPU, params->data, sizeof(*dispatch));
				GX2DispatchCompute(dispatch); retire(params);
			}
			touch(shader.header); touch(shader.program);
			// Export publication belongs to this producer, before any copy, sampled
			// alias or attribute reader. The stable region may be reused afterward.
			for (const ComputeOutput& output : outputs)
			{
				void* data = (uint8_t*)_exports->data+output.offset;
				GX2Invalidate(GX2_INVALIDATE_MODE_EXPORT_BUFFER, data, output.size);
				GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, data, output.size);
			}
			GX2SetShaderMode(kGraphicsShaderMode);
			for (const ComputeOutput& output : outputs)
			{
				const Binding& bind = _bind.m_bind[output.binding];
				if (bind.m_type != Binding::Image)
				{
					Buffer& buffer = bind.m_type == Binding::IndexBuffer ? m_indexBuffers[bind.m_idx] : m_vertexBuffers[bind.m_idx];
					gpuCopyBytes(_exports, output.offset, buffer.block, bind.m_offset, output.size);
					gpuBufferWritten(buffer);
				}
				else
				{
					Texture& texture = m_textures[bind.m_idx];
					const bool convert = output.surface.format != texture.texture.surface.format;
					if (convert) { restore(); beginBatch(); }
					for (uint32_t layer = 0; layer < output.layers; ++layer)
					{
						GX2Surface source = output.surface; source.image = (uint8_t*)_exports->data+output.offset+layer*source.imageSize;
						if (convert) computeImageConvert(texture, bind, source, layer, false);
						else GX2CopySurface(&source, 0, 0, &texture.texture.surface, bind.m_firstMip, bind.m_firstLayer+layer);
					}
					if (convert) endBatch();
					transferWritten(texture, bind.m_firstMip, bind.m_firstLayer, output.layers);
				}
			}
			if (_exports) touch(_exports);
		}

		void submit(Frame* _render, const ClearQuad& _clearQuad, const MipGen& _mipGen, TextVideoMemBlitter& _blitter) override
		{
			GX2_PROFILE(begin(_render->m_frameNum, _render->m_numRenderItems != 0, 0 != (_render->m_reset & BGFX_RESET_VSYNC)));
			GX2_PROFILE_COARSE(Submit);
			if (_render->m_reset & BGFX_RESET_SUSPEND)
			{
				if (!m_suspended) suspend();
				_render->m_perfStats.cpuTimeBegin = _render->m_perfStats.cpuTimeEnd = bx::getHPCounter();
				return;
			}
			if (m_suspended)
			{
				m_native.acquire(_render->m_mainSwapChain.height);
				m_context = m_native.context; m_suspended = false;
			}
			m_mipGen = &_mipGen; m_transferFrame = _render->m_frameNum; transferPublish();
			Stats& stats = _render->m_perfStats;
			stats.cpuTimeBegin = bx::getHPCounter(); stats.cpuTimerFreq = bx::getHPFrequency();
			stats.numViews = 0; stats.numDraw = 0; stats.numCompute = 0;
			stats.numBlit = _render->m_numBlitItems; stats.numBlitRepack = _render->m_numBlitRepack;
			stats.gpuMemoryMax = -INT64_MAX; stats.gpuMemoryUsed = -INT64_MAX;
			bx::memSet(stats.numPrims, 0, sizeof(stats.numPrims));
			collect();
			resolveOcclusion(_render); resolveGpuTimers(_render);
			updateWindows(_render->m_mainSwapChain);
			const FrameBufferHandle initial = BGFX_INVALID_HANDLE;
#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
			s_gx2Profiler.startGpu(_render->m_numRenderItems != 0);
#endif
			m_anisotropy = (_render->m_reset & BGFX_RESET_MAXANISOTROPY) ? GX2_TEX_ANISO_RATIO_16_TO_1 : GX2_TEX_ANISO_RATIO_NONE;
			m_frameGpuTimer = beginGpuTimer(BGFX_CONFIG_MAX_VIEWS, _render->m_frameNum);
			uint32_t viewTimer = UINT32_MAX;
			uploadTextures();
			restore();
			m_currentFrameBuffer = BGFX_INVALID_HANDLE;
			if (frameBufferAvailable(initial))
			{
				const uint32_t interval = (_render->m_reset & BGFX_RESET_VSYNC) ? 1 : 0;
				// The native setter drains outstanding swaps even when unchanged.
				// Querying also catches SDL restoring interval one on foreground acquire.
				if (GX2GetSwapInterval() != interval) GX2SetSwapInterval(interval);
				bindFrameBuffer(initial);
			}
			if (_render->m_iboffset) update(m_indexBuffers[_render->m_transientIb->handle.idx], 0, _render->m_iboffset, _render->m_transientIb->data, false, true);
			if (_render->m_vboffset) update(m_vertexBuffers[_render->m_transientVb->handle.idx], 0, _render->m_vboffset, _render->m_transientVb->data, true, true);
			{ GX2_PROFILE_SORT(); _render->sort(); }
			uint32_t exportBytes = 0;
			bool hasGraphicsImages = false;
			{
				GX2_PROFILE_SCOPE(ExportPrepass);
				if (m_needExportPrepass)
				{
					for (uint32_t item = 0; item < _render->m_numRenderItems; ++item)
					{
						SortKey key;
						if (key.decode(_render->m_sortKeys[item], _render->m_viewRemap))
						{
							const RenderCompute& compute = _render->m_renderItem[_render->m_sortValues[item]].compute;
							std::vector<ComputeOutput> outputs;
							exportBytes = bx::max(exportBytes, computeLayout(*m_programs[key.m_program.idx].compute, _render->m_renderBind[compute.m_bindIdx], outputs));
							if (isValid(compute.m_indirectBuffer)) exportBytes = bx::max(exportBytes, 144u);
						}
						else
						{
							const RenderDraw& draw = _render->m_renderItem[_render->m_sortValues[item]].draw;
							if (draw.m_streamMask)
							{
								const Program& program = m_programs[key.m_program.idx];
								hasGraphicsImages |= !program.vertex->resources.empty() || !program.fragment->resources.empty();
							}
							if (!isValid(draw.m_indirectBuffer)) continue;
							const bool indexed = isValid(draw.m_indexBuffer);
							const uint32_t num = indirectRecords(draw.m_indirectBuffer, draw.m_startIndirect, draw.m_numIndirect);
							exportBytes = bx::max(exportBytes, bx::min(num, indirectChunk(indexed))*(indexed ? 96u : 80u));
						}
					}
				}
			}
			GraphicsImages images;
			// Most frames have no graphics image bindings. Their sorted draw data
			// does not need a second traversal to build an empty export layout.
			if (hasGraphicsImages)
			{
				for (uint32_t item = 0; item < _render->m_numRenderItems; ++item)
				{
					SortKey key;
					if (!key.decode(_render->m_sortKeys[item], _render->m_viewRemap))
					{
						const RenderDraw& draw = _render->m_renderItem[_render->m_sortValues[item]].draw;
						if (draw.m_streamMask) exportBytes = graphicsImageLayout(images, m_programs[key.m_program.idx], _render->m_renderBind[draw.m_bindIdx], exportBytes);
					}
				}
			}
			Block* exports = exportBytes ? gpuBufferAllocate(exportBytes) : NULL;
			if (exports) DCInvalidateRange(exports->data, exports->size);
			BlitState blits(_render);
			UniformCacheState ucs(_render);
			ViewState viewState(_render);
			uint16_t view = UINT16_MAX;
			// Frame view metadata stays immutable throughout this submission.
			const View* currentView = NULL;
			beginBatch();
			for (uint32_t item = 0; item < _render->m_numRenderItems; ++item)
			{
				GX2_PROFILE_SCOPE(DrawLoop);
				SortKey key;
				const bool compute = key.decode(_render->m_sortKeys[item], _render->m_viewRemap);
				GX2_PROFILE_ADD(DrawViewLookups, view != key.m_view);
				const View& renderView = view == key.m_view ? *currentView : _render->view(key.m_view);
				if (!frameBufferAvailable(renderView.m_fbh)) continue;
				if (view != key.m_view)
				{
					currentView = &renderView;
					endViewTimer(_render, viewTimer); viewTimer = UINT32_MAX;
					view = key.m_view;
					Window* window = windowFor(renderView.m_fbh);
					if (window) window->used = true;
					if (blits.hasItem(view)) graphicsImagesConsume(images, exports, kInvalidHandle, true);
					if (isValid(renderView.m_fbh) && !windowFor(renderView.m_fbh))
						for (const Attachment& attachment : m_frameBuffers[renderView.m_fbh.idx].attachments)
							graphicsImagesConsume(images, exports, attachment.handle.idx, true);
					transferBlits(blits, view);
					if (m_currentFrameBuffer.idx != renderView.m_fbh.idx)
					{
						endBatch(); finishRenderPass();
						// Binding restores only after a real helper or native context change.
						bindFrameBuffer(renderView.m_fbh); beginBatch();
					}
					viewTimer = beginViewTimer(_render, view);
					while (ucs.hasItem(view))
					{
						const UniformCacheItem uniform = ucs.advance();
						updateUniform(uniform.m_handle, &_render->m_uniformCacheFrame.m_data[uniform.m_offset], uniform.m_size);
					}
					viewState.setView(renderView); viewState.m_rect = renderView.m_rect;
					viewport(renderView); clear(renderView, _clearQuad, _render->m_colorPalette);
				}
				if (compute)
				{
					const RenderCompute& dispatch = _render->m_renderItem[_render->m_sortValues[item]].compute;
					if (dispatch.m_uniformBegin < dispatch.m_uniformEnd)
						rendererUpdateUniforms(this, _render->m_uniformBuffer[dispatch.m_uniformIdx], dispatch.m_uniformBegin, dispatch.m_uniformEnd);
					if (_render->m_debug & BGFX_DEBUG_IFH) continue;
					graphicsImagesConsume(images, exports, kInvalidHandle, true);
					endBatch(); finishRenderPass();
					bindComputeExports(exports);
					dispatchCompute(m_programs[key.m_program.idx], dispatch, _render->m_renderBind[dispatch.m_bindIdx], exports, _render->m_colorPalette, viewState, _render, view, renderView);
					++stats.numCompute;
					restore(); bindFrameBuffer(m_currentFrameBuffer); viewport(renderView); beginBatch();
					continue;
				}
				const RenderDraw& draw = _render->m_renderItem[_render->m_sortValues[item]].draw;
				if (0 == draw.m_streamMask) continue; // touch still executes the view clear; vertex-ID draws use UINT32_MAX
				++stats.numDraw;
				if (draw.m_uniformBegin < draw.m_uniformEnd)
					rendererUpdateUniforms(this, _render->m_uniformBuffer[draw.m_uniformIdx], draw.m_uniformBegin, draw.m_uniformEnd);
				const bool hasOcclusionQuery = 0 != (draw.m_stateFlags & BGFX_STATE_INTERNAL_OCCLUSION_QUERY);
				if (isValid(draw.m_occlusionQuery) && !hasOcclusionQuery
				&& !isVisible(_render, draw.m_occlusionQuery, 0 != (draw.m_submitFlags & BGFX_SUBMIT_INTERNAL_OCCLUSION_VISIBLE))) continue;
				if (_render->m_debug & BGFX_DEBUG_IFH) continue;
				Rect scissor = renderView.m_clippedRect;
				if (!renderView.m_scissor.isZero()) scissor.intersect(renderView.m_scissor);
				if (draw.m_scissor != UINT16_MAX) scissor.intersect(_render->m_frameCache.m_rectCache.m_cache[draw.m_scissor]);
				if (scissor.isZero()) continue;
				std::vector<Block*> indirect;
				const bool indexed = isValid(draw.m_indexBuffer);
				const uint32_t indirectNum = isValid(draw.m_indirectBuffer) ? indirectRecords(draw.m_indirectBuffer, draw.m_startIndirect, draw.m_numIndirect) : 0;
				const uint32_t indirectStride = indexed ? 96 : 80;
				const uint32_t topology = (draw.m_stateFlags & BGFX_STATE_PT_MASK) >> BGFX_STATE_PT_SHIFT;
				const GX2PrimitiveMode primitive = s_primitive[topology];
				if (isValid(draw.m_indirectBuffer))
				{
					if (!indirectNum) continue;
					endBatch(); finishRenderPass(); bindComputeExports(exports);
					for (uint32_t first = 0; first < indirectNum; first += indirectChunk(indexed))
					{
						const uint32_t num = bx::min(indirectNum-first, indirectChunk(indexed));
						uint32_t params[3][4] = {
							{draw.m_startIndirect+first, num, isValid(draw.m_numIndirectBuffer) ? 1u : 0u, isValid(draw.m_numIndirectBuffer) ? draw.m_numIndirectIndex : 0u},
							{0, 0, uint32_t(primitive), 0}, {0, 0, first, 0},
						};
						if (indexed)
						{
							Buffer& buffer = m_indexBuffers[draw.m_indexBuffer.idx];
							const uint32_t size = buffer.flags & BGFX_BUFFER_INDEX32 ? 4 : 2;
							params[1][0] = OSEffectiveToPhysical(uint32_t(uintptr_t(buffer.block->data)));
							params[1][1] = size;
							params[1][3] = size == 4 ? (buffer.littleEndian ? GX2_INDEX_TYPE_U32_LE : GX2_INDEX_TYPE_U32)
								: (buffer.littleEndian ? GX2_INDEX_TYPE_U16_LE : GX2_INDEX_TYPE_U16);
							touch(buffer.block);
						}
						indirect.push_back(lowerIndirect(indexed ? 1 : 0, draw.m_indirectBuffer, draw.m_numIndirectBuffer, params, num*indirectStride, exports));
					}
					restore(); bindFrameBuffer(m_currentFrameBuffer); viewport(renderView); beginBatch();
				}
				Program& program = m_programs[key.m_program.idx];
				if (graphicsImagesBind(images, exports, program, _render->m_renderBind[draw.m_bindIdx])) viewport(renderView);
				if (prepareTextures(_render->m_renderBind[draw.m_bindIdx], program)) viewport(renderView);
				setScissor(scissor);
				uniformsBegin(program);
				viewState.m_alphaRef = float((draw.m_stateFlags & BGFX_STATE_ALPHA_REF_MASK) >> BGFX_STATE_ALPHA_REF_SHIFT)/255;
				{ GX2_PROFILE_SCOPE(UniformPredefined);
					if (program.m_numPredefined == 1 && (program.m_predefined[0].m_type & ~kUniformFragmentBit) == PredefinedUniform::ModelViewProj)
					{
						const PredefinedUniform& uniform = program.m_predefined[0];
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
						float* mvp = (float*)(m_stageData[0 != (uniform.m_type & kUniformFragmentBit)]+uniform.m_loc*4);
#else
						Matrix4 storage;
						float* mvp = storage.un.val;
#endif
						multiplyMatrixPaired(mvp, _render->m_frameCache.m_matrixCache.at(draw.m_startMatrix).un.val, viewState.m_viewProj.un.val);
#if !BGFX_CONFIG_GX2_UNIFORM_REGISTERS
						setShaderUniform4x4f(uniform.m_type & kUniformFragmentBit, uniform.m_loc, mvp, bx::min<uint16_t>(4, uniform.m_count));
#endif
					}
					else viewState.setPredefined<4>(this, view, renderView, program, _render, draw);
				}
				uniformsEnd(program);
				const DepthControl& depth = draw.m_depthBias == UINT16_MAX ? renderView.m_depthBias : _render->m_frameCache.m_depthBiasCache.m_cache[draw.m_depthBias];
				depthControl(depth);
				if (draw.m_sampleMask != m_sampleMask)
				{
					const uint8_t mask = uint8_t(draw.m_sampleMask); GX2SetAAMask(mask, mask, mask, mask); m_sampleMask = draw.m_sampleMask;
				}
				state(draw.m_stateFlags, draw.m_stencil, draw.m_rgba, 0 != (_render->m_debug & BGFX_DEBUG_WIREFRAME), program.fragment->sampleFrequency);
				{
					GX2_PROFILE_SCOPE(AttachmentWrites);
					if (m_colorWritten != (1u << m_numColors)-1)
					{
						const uint32_t colorMask = colorWriteMask(draw.m_stateFlags);
						const uint32_t exports = ((const GX2PixelShader*)program.fragment->header->data)->regs.cb_shader_mask;
						for (uint32_t target = 0; target < m_numColors; ++target)
							if ((exports >> (target*4)) & colorMask) m_colorWritten |= 1u << target;
					}
					if (m_hasDepth && !m_depthWritten)
					{
						const uint32_t front = unpackStencil(0, draw.m_stencil);
						const uint32_t back = stencilFrontAndBack(draw.m_stencil) ? unpackStencil(1, draw.m_stencil) : front;
						const uint32_t ops = BGFX_STENCIL_OP_FAIL_S_MASK | BGFX_STENCIL_OP_FAIL_Z_MASK | BGFX_STENCIL_OP_PASS_Z_MASK;
						const uint32_t keep = BGFX_STENCIL_OP_FAIL_S_KEEP | BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;
						const bool stencilWrite = stencilEnabled(draw.m_stencil) && unpackStencilWriteMask(draw.m_stencil)
							&& ((front & ops) != keep || (back & ops) != keep);
						m_depthWritten = ((!m_depthReadOnly && (draw.m_stateFlags & BGFX_STATE_WRITE_Z)) || (!m_stencilReadOnly && stencilWrite));
					}
				}
				textures(_render->m_renderBind[draw.m_bindIdx], _render->m_colorPalette);

				GX2_PROFILE_ADD(VisibleDraws, 1);
				primitiveRestart(primitive, indexed ? (m_indexBuffers[draw.m_indexBuffer.idx].flags & BGFX_BUFFER_INDEX32 ? 4 : 2) : 0);
				vertexInput(program, draw);
				uint32_t count = 0;
				Block* occlusion = hasOcclusionQuery ? beginOcclusion(draw.m_occlusionQuery) : NULL;
				if (!indirect.empty())
				{
					// Generated IBs must be called from the command buffer, not inside
					// the ordinary batch IB (GX2 permits only one nested list level).
					endBatch();
					uint32_t remaining = indirectNum;
					for (Block*& commands : indirect)
					{
						const uint32_t num = bx::min(remaining, indirectChunk(indexed));
						callIndirect(commands, 0, num*indirectStride); retire(commands);
						remaining -= num;
					}
					beginBatch();
				}
				else if (indexed)
				{
					Buffer& buffer = m_indexBuffers[draw.m_indexBuffer.idx];
					const uint32_t indexSize = buffer.flags & BGFX_BUFFER_INDEX32 ? 4 : 2;
					count = draw.m_numIndices == UINT32_MAX ? buffer.size/indexSize-draw.m_startIndex : draw.m_numIndices;
					{
						GX2_PROFILE_SCOPE(DirectDraw);
						GX2DrawIndexedEx(primitive, count, indexSize == 4 ? (buffer.littleEndian ? GX2_INDEX_TYPE_U32_LE : GX2_INDEX_TYPE_U32) : (buffer.littleEndian ? GX2_INDEX_TYPE_U16_LE : GX2_INDEX_TYPE_U16),
							(uint8_t*)buffer.block->data+draw.m_startIndex*indexSize, 0, draw.m_numInstances);
					}
					touch(buffer.block);
				}
				else
				{
					count = draw.m_numVertices;
					if (count == UINT32_MAX)
						for (uint32_t ss = 0; ss < BGFX_CONFIG_MAX_VERTEX_STREAMS; ++ss)
							if (draw.m_streamMask & (1<<ss))
							{
								const Stream& stream = draw.m_stream[ss];
								const Buffer& buffer = m_vertexBuffers[stream.m_handle.idx];
								const VertexLayout& layout = m_layouts[isValid(stream.m_layoutHandle) ? stream.m_layoutHandle.idx : buffer.layout.idx];
								count = bx::min(count, buffer.size/layout.m_stride-stream.m_startVertex);
							}
					{ GX2_PROFILE_SCOPE(DirectDraw); GX2DrawEx(primitive, count, 0, draw.m_numInstances); }
				}
				graphicsImagesWritten(images, program, _render->m_renderBind[draw.m_bindIdx]);
				if (occlusion) endOcclusion(occlusion);
				static const uint8_t divisor[] = {3, 1, 2, 1, 1}, subtract[] = {0, 2, 0, 1, 0};
				stats.numPrims[topology] += (count/divisor[topology] > subtract[topology] ? count/divisor[topology]-subtract[topology] : 0)*draw.m_numInstances;
			}
			graphicsImagesConsume(images, exports);
			retire(images.layout);
			retire(exports);
			endViewTimer(_render, viewTimer);
			endBatch(); finishRenderPass(); transferRemainingBlits(blits);
			if (frameBufferAvailable(_render->m_debugFrameBuffer) && (_render->m_debug & (BGFX_DEBUG_STATS | BGFX_DEBUG_IFH)))
			{
				const TextVideoMem& target = *_render->m_textVideoMem;
				m_statsText.resize(target.m_small, target.m_width*8, target.m_height*(target.m_small ? 8 : 16));
				m_statsText.clear();
				m_statsText.printf(0, 0, 0x0f, "GX2 / Wii U");
				m_statsText.printf(0, 1, 0x0f, "Draws: %u  Blits: %u", stats.numDraw, stats.numBlit);
				m_statsText.printf(0, 2, 0x0f, "CPU: %.3f ms  GPU: %.3f ms", 1000.0*double(bx::getHPCounter()-stats.cpuTimeBegin)/stats.cpuTimerFreq, 1000.0*double(stats.gpuTimeEnd-stats.gpuTimeBegin)/stats.gpuTimerFreq);
				dbgTextSubmit(this, _blitter, m_statsText, _render->m_debugFrameBuffer, _render->m_debugTextScale);
			}
			else if (frameBufferAvailable(_render->m_debugFrameBuffer) && (_render->m_debug & BGFX_DEBUG_TEXT)) dbgTextSubmit(this, _blitter, _render->m_textVideoMem, _render->m_debugFrameBuffer, _render->m_debugTextScale);
			transferCapture(0 != (_render->m_reset & BGFX_RESET_CAPTURE));
			stats.cpuTimeEnd = bx::getHPCounter();
			GX2_PROFILE(draws += stats.numDraw);
		}

		void dbgTextRenderBegin(TextVideoMemBlitter&, FrameBufferHandle _handle) override
		{
			Window* window = windowFor(_handle);
			if (window) window->used = true;
			// Binding restores context only for a target switch or a native helper.
			bindFrameBuffer(_handle); beginBatch();
			depthControl({}); primitiveRestart(GX2_PRIMITIVE_MODE_TRIANGLES, 0);
			if (m_sampleMask != UINT32_MAX) { GX2SetAAMask(0xff, 0xff, 0xff, 0xff); m_sampleMask = UINT32_MAX; }
			GX2SetViewport(0, 0, m_targetWidth, m_targetHeight, -1, 1);
			setScissor({0, 0, uint16_t(m_targetWidth), uint16_t(m_targetHeight)});
			state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0, 0);
		}

		void dbgTextRender(TextVideoMemBlitter& _blitter, uint32_t _numIndices) override
		{
			if (!_numIndices) return;
			const uint32_t numVertices = _numIndices*4/6;
			const uint32_t stride = _blitter.m_layout.m_stride;
			Buffer& vertex = m_vertexBuffers[_blitter.m_vb->handle.idx];
			Buffer& index = m_indexBuffers[_blitter.m_ib->handle.idx];
			update(vertex, 0, numVertices*stride, _blitter.m_vb->data, true, true);
			update(index, 0, _numIndices*2, _blitter.m_ib->data, false, true);
			Program& program = m_programs[_blitter.m_program.idx];
			uniformsBegin(program);
			float proj[16];
			bx::mtxOrtho(proj, 0.0f, float(m_targetWidth), float(m_targetHeight), 0.0f, 0.0f, 1000.0f, 0.0f, false);
			for (uint32_t ii = 0; ii < program.m_numPredefined; ++ii)
			{
				const PredefinedUniform& uniform = program.m_predefined[ii];
				if ((uniform.m_type & ~kUniformFragmentBit) == PredefinedUniform::ModelViewProj)
					setShaderUniform4x4f(uniform.m_type & kUniformFragmentBit, uniform.m_loc, proj, 4);
			}
			uniformsEnd(program);
			RenderDraw draw; draw.clear(); draw.m_streamMask = 1;
			draw.m_stream[0].m_handle = _blitter.m_vb->handle;
			draw.m_stream[0].m_layoutHandle = _blitter.m_vb->layoutHandle;
			vertexInput(program, draw);
			RenderBind bind; bind.clear();
			bind.m_bind[0].m_idx = _blitter.m_texture.idx;
			bind.m_bind[0].m_type = Binding::Texture;
			bind.m_bind[0].m_samplerFlags = BGFX_SAMPLER_INTERNAL_DEFAULT;
			const float palette[16][4] = {};
			textures(bind, palette);
			GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, _numIndices, GX2_INDEX_TYPE_U16, index.block->data, 0, 1);
			touch(index.block);
		}
		void dbgTextRenderEnd(TextVideoMemBlitter&) override { endBatch(); m_colorWritten = 1; finishRenderPass(); }

		std::vector<OcclusionSample> m_pendingOcclusion;
		std::vector<Block*> m_freeQueryBlocks;
		std::vector<GpuTimerSample> m_pendingGpuTimers;
		std::vector<Block*> m_freeTimerBlocks;
		GpuTimerResult m_gpuTimerResults[BGFX_CONFIG_MAX_VIEWS + 1];
		uint32_t m_maxGpuLatency = 0;
		uint32_t m_frameGpuTimer = UINT32_MAX;

		std::vector<TransferOutput> m_transferOutputs;
		uint32_t m_transferFrame = 0;
		bool m_captureRequested = false;
		bool m_captureActive = false;
		uint32_t m_captureWidth = 0, m_captureHeight = 0, m_capturePitch = 0;
		TextureFormat::Enum m_captureFormat = TextureFormat::Unknown;

		Window m_mainWindow;
		OSTime m_presentStamps[kQueuedFrames] = {};
		uint32_t m_presentCursor = 0;
		NativeGraphics m_native;
		bool m_suspended = false;
		GX2ContextState* m_context = NULL;
		Buffer m_indexBuffers[BGFX_CONFIG_MAX_INDEX_BUFFERS];
		Buffer m_vertexBuffers[BGFX_CONFIG_MAX_VERTEX_BUFFERS];
		VertexLayout m_layouts[BGFX_CONFIG_MAX_VERTEX_LAYOUTS];
		Shader m_shaders[BGFX_CONFIG_MAX_SHADERS];
		Shader m_indirectShaders[3]; // Non-indexed, indexed, dispatch command lowering.
		Program m_programs[BGFX_CONFIG_MAX_PROGRAMS];
		const MipGen* m_mipGen = NULL;
		Texture m_textures[BGFX_CONFIG_MAX_TEXTURES];
		FrameBuffer m_frameBuffers[BGFX_CONFIG_MAX_FRAME_BUFFERS];
		FrameBufferHandle m_currentFrameBuffer = BGFX_INVALID_HANDLE;
		GX2ColorBuffer m_colors[kColorTargets] = {};
		GX2DepthBuffer m_activeDepth = {};
		uint32_t m_targetWidth = 0, m_targetHeight = 0;
		uint32_t m_numColors = 0, m_colorWritten = 0;
		bool m_hasDepth = false, m_depthReadOnly = false, m_stencilReadOnly = false, m_depthWritten = false;
		std::vector<Block*> m_allocations, m_touched, m_retired;
		SubmissionRing m_lists, m_uploads;
		std::vector<Texture*> m_pendingTextures;
		uint32_t m_listOffset = 0, m_uploadOffset = 0;
		uint32_t m_uploadUsed = 0;
		uint32_t m_batchDraws = 0;
		uint8_t* m_stageData[2];
#if BGFX_CONFIG_GX2_UNIFORM_REGISTERS
		alignas(32) uint8_t m_uniformData[2][4096];
#endif
		TextVideoMem m_statsText;
		bool m_wireframe = false;
		// Sticky: ordinary graphics frames need no export-layout traversal.
		bool m_needExportPrepass = false;
		DepthControl m_depthControl = {};
		uint32_t m_restartSize = UINT32_MAX;
		uint32_t m_rasterMode = UINT32_MAX;
		uint32_t m_sampleMask = UINT32_MAX;
		uint64_t m_cachedState, m_cachedStencil;
		uint32_t m_cachedRgba;
		void* m_boundExportData = NULL;
		uint32_t m_boundExportSize = 0;
		GX2Sampler m_sampler;
		uint64_t m_samplerKey = UINT64_MAX;
		Program* m_boundProgram = NULL;
		Block* m_boundFetch = NULL;
		Block* m_defaultVertex = NULL;
		const void* m_boundAttributes[BGFX_CONFIG_MAX_VERTEX_STREAMS];
		uint32_t m_boundStrides[BGFX_CONFIG_MAX_VERTEX_STREAMS];
		const GX2Texture* m_boundTextures[kTextureSamplers];
		Binding m_boundBindings[kTextureSamplers];
		uint8_t m_boundDimensions[kTextureSamplers];
		uint8_t m_boundStages[kTextureSamplers];
		float m_borderColors[2][kTextureSamplers][4];
		uint32_t m_borderValid[2] = {};
		Rect m_cachedScissor;
		GX2TexAnisoRatio m_anisotropy = GX2_TEX_ANISO_RATIO_NONE;
	};

	static RendererContextGX2* s_renderGX2;
	RendererContextI* rendererCreate(const Init& _init)
	{
		if (NULL == _init.swapChain.nwh && (0 != _init.swapChain.width || 0 != _init.swapChain.height)) return NULL;
		s_renderGX2 = BX_ALIGNED_NEW(g_allocator, RendererContextGX2, alignof(RendererContextGX2))(_init);
		return s_renderGX2;
	}
	void rendererDestroy()
	{
		bx::deleteObject(g_allocator, s_renderGX2, alignof(RendererContextGX2)); s_renderGX2 = NULL;
	}
} }
#else
namespace bgfx { namespace gx2
{
	RendererContextI* rendererCreate(const Init&) { return NULL; }
	void rendererDestroy() {}
} }
#endif // BGFX_CONFIG_RENDERER_GX2
