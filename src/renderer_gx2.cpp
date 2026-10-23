/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#include "bgfx_p.h"

#if BGFX_CONFIG_RENDERER_GX2
#include "renderer.h"
#include "renderer_gx2.h"

namespace bgfx { namespace gx2
{
	static const OSTime kPending = INT64_MAX;

	static void unsupported(const char* _operation)
	{
		BGFX_FATAL(false, Fatal::UnableToInitialize, "GX2: %s is not supported by the initial graphics backend.", _operation);
	}

	static uint32_t readLE(const uint8_t*& _ptr, uint32_t _bytes)
	{
		uint32_t value = 0;
		for (uint32_t ii = 0; ii < _bytes; ++ii) value |= uint32_t(*_ptr++) << (ii*8);
		return value;
	}

	static void writeFloats(void* _dst, const void* _src, uint32_t _count)
	{
		uint32_t* dst = (uint32_t*)_dst;
		const uint32_t* src = (const uint32_t*)_src;
		for (uint32_t ii = 0; ii < _count; ++ii) dst[ii] = bx::toLittleEndian(src[ii]);
	}

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
		case TextureFormat::BC4: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_BC4;
		case TextureFormat::BC5: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_UNORM_BC5;
		case TextureFormat::BC4S: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_SNORM_BC4;
		case TextureFormat::BC5S: _compMap = GX2_COMP_MAP(0, 1, 2, 3); return GX2_SURFACE_FORMAT_SNORM_BC5;
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
			: m_platform(*(const Gx2PlatformData*)_init.swapChain.nwh)
		{
			g_caps.supported = BGFX_CAPS_INDEX32 | BGFX_CAPS_BLEND_INDEPENDENT | BGFX_CAPS_VERTEX_ATTRIB_UINT10 | BGFX_CAPS_TEXTURE_EXTERNAL;
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
					if (MipGen::isSupported(format)) g_caps.formats[ff] |= BGFX_CAPS_FORMAT_TEXTURE_MIP_AUTOGEN;
				}
				if (format == TextureFormat::RGBA8 || format == TextureFormat::BGRA8 || (format >= TextureFormat::BC1 && format <= TextureFormat::BC3))
					g_caps.formats[ff] |= BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB | BGFX_CAPS_FORMAT_TEXTURE_3D_SRGB | BGFX_CAPS_FORMAT_TEXTURE_CUBE_SRGB;
			}
			g_caps.limits.maxTextureSize = 8192;
			g_caps.limits.maxTextureLayers = 2048;
			g_caps.limits.maxFBAttachments = 8;
			g_caps.limits.maxVertexStreams = BGFX_CONFIG_MAX_VERTEX_STREAMS;
			g_caps.limits.maxVertexAttributes = 32;
			g_caps.limits.maxTextureSamplers = 16;
			g_caps.limits.maxComputeBindings = 0;
			g_caps.limits.maxBlits = 0;
			g_caps.limits.maxOcclusionQueries = BGFX_CONFIG_MAX_OCCLUSION_QUERIES;
			g_caps.limits.maxInstanceData = 16;
		}

		~RendererContextGX2()
		{
			// SDL is still alive here. One shutdown drain covers live and retired storage.
			if (m_depthImage) GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, m_depthImage->data, m_depthImage->size);
			GX2DrawDone();
			for (Block* block : m_allocations)
			{
				MEMFreeToDefaultHeap(block->data);
				bx::deleteObject(g_allocator, block);
			}
		}

		RendererType::Enum getRendererType() const override { return RendererType::Gx2; }
		const char* getRendererName() const override { return "GX2"; }
		bool isDeviceRemoved() override { return false; }

		Block* allocate(uint32_t _size, uint32_t _alignment = 64)
		{
			Block* block = BX_NEW(g_allocator, Block);
			block->size = bx::alignUp(_size, 64);
			block->data = MEMAllocFromDefaultHeapEx(block->size, bx::max(64u, _alignment));
			m_allocations.push_back(block);
			return block;
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
			const OSTime completed = GX2GetRetiredTimeStamp();
			for (size_t ii = 0; ii < m_retired.size();)
			{
				Block* block = m_retired[ii];
				if (block->stamp > completed) { ++ii; continue; }
				auto it = std::find(m_allocations.begin(), m_allocations.end(), block);
				m_allocations.erase(it);
				MEMFreeToDefaultHeap(block->data);
				bx::deleteObject(g_allocator, block);
				m_retired.erase(m_retired.begin()+ii);
			}
		}

		void seal()
		{
			const OSTime stamp = GX2GetLastSubmittedTimeStamp();
			for (Block* block : m_touched) block->stamp = stamp;
			m_touched.clear();
		}

		void endFrame(bool _willPresent) override
		{
			if ((!_willPresent || !m_target.color) && !m_touched.empty())
			{
				GX2Flush();
				seal();
			}
		}

		void flip() override
		{
			if (m_target.color)
			{
				uint32_t swaps, flips;
				OSTime lastFlip, lastVsync;
				GX2GetSwapStatus(&swaps, &flips, &lastFlip, &lastVsync);
				if (swaps - flips >= 2) GX2WaitForFlip();
				m_platform.present(m_platform.userData);
				seal();
			}
		}

		Block* acquire(std::vector<Block*>& _pool, uint32_t _size, bool _displayList = false)
		{
			const OSTime completed = GX2GetRetiredTimeStamp();
			for (Block* block : _pool)
				if (block->stamp <= completed && block->size >= _size) return block;
			if (_pool.size() >= 16)
			{
				GX2Flush(); seal();
				Block* oldest = *std::min_element(_pool.begin(), _pool.end(), [](Block* a, Block* b) { return a->stamp < b->stamp; });
				// A timed-out wait is not permission to overwrite GPU-owned bytes.
				while (!GX2WaitTimeStamp(oldest->stamp)) {}
				return oldest;
			}
			Block* block = allocate(_size, 256);
			// List payloads are only written through write-gather, including on reuse.
			if (_displayList) DCFlushRange(block->data, block->size);
			_pool.push_back(block);
			return block;
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

		void update(Buffer& _buffer, uint32_t _offset, uint32_t _size, const void* _data, bool _vertex, bool _discard = false)
		{
			_size = bx::min(_size, _buffer.size - _offset);
			if (_buffer.block->stamp > GX2GetRetiredTimeStamp())
			{
				Block* replacement = reuse(_buffer.spare, _buffer.size);
				const bool preserve = !_discard && (_offset != 0 || _size != _buffer.size);
				if (preserve)
					bx::memCopy(replacement->data, _buffer.block->data, _buffer.size);
				_buffer.spare.push_back(_buffer.block);
				_buffer.block = replacement;
				// Preserved bytes have also been CPU-written into the new allocation.
				bx::memCopy((uint8_t*)replacement->data+_offset, _data, _size);
				GX2Invalidate(_vertex ? GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER : GX2_INVALIDATE_MODE_CPU, (uint8_t*)replacement->data+(preserve ? 0 : _offset), preserve ? _buffer.size : _size);
			}
			else
			{
				void* dst = (uint8_t*)_buffer.block->data+_offset;
				bx::memCopy(dst, _data, _size);
				GX2Invalidate(_vertex ? GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER : GX2_INVALIDATE_MODE_CPU, dst, _size);
			}
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
			if (_flags & (BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_WRITE | BGFX_BUFFER_DRAW_INDIRECT)) unsupported("compute/indirect buffers");
			buffer.size = _size; buffer.flags = _flags; buffer.block = allocate(_size);
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
			if (_flags & (BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_WRITE | BGFX_BUFFER_DRAW_INDIRECT)) unsupported("compute/indirect buffers");
			buffer.size = _size; buffer.flags = _flags; buffer.block = allocate(_size);
		}
		void updateDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _offset, uint32_t _size, const Memory* _mem) override
		{ update(m_vertexBuffers[_handle.idx], _offset, bx::min(_size, _mem->size), _mem->data, true); }
		void destroyVertexBuffer(VertexBufferHandle _handle) override { destroyBuffer(m_vertexBuffers[_handle.idx]); }
		void destroyDynamicVertexBuffer(VertexBufferHandle _handle) override { destroyVertexBuffer(_handle); }

		void createShader(ShaderHandle _handle, const Memory* _mem) override
		{
			Shader& shader = m_shaders[_handle.idx];
			const uint8_t* ptr = _mem->data;
			const uint32_t magic = readLE(ptr, 4);
			shader.fragment = isShaderType(magic, 'F');
			if (!shader.fragment && !isShaderType(magic, 'V')) { unsupported("compute shaders"); return; }
			readLE(ptr, 4); readLE(ptr, 4); // interface hashes
			readLE(ptr, 4); readLE(ptr, 4); // raw masks (zero in GX2 profile)
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
				readLE(ptr, 1); readLE(ptr, 1); readLE(ptr, 2); // texture metadata
				if (type == UniformType::Sampler) continue; // explicit binding, no numeric block data
				const PredefinedUniform::Enum predefined = nameToPredefinedUniformEnum(name);
				if (predefined != PredefinedUniform::Count)
				{
					PredefinedUniform binding;
					binding.m_loc = offset; binding.m_count = rows;
					binding.m_type = uint8_t(predefined) | (shader.fragment ? kUniformFragmentBit : 0);
					shader.predefined.push_back(binding);
				}
				else
				{
					const UniformRegInfo* info = m_uniformReg.find(name);
					BGFX_FATAL(NULL != info, Fatal::InvalidShader, "GX2 uniform %s is not registered.", name);
					Uniform uniform = { info->m_handle, offset, rows, type, num };
					shader.uniforms.push_back(uniform);
				}
			}
			const uint32_t gfdSize = readLE(ptr, 4);
			BX_UNUSED(gfdSize);
			// The envelope is byte-packed; GFD reads native words and needs alignment.
			void* gfd = bx::alloc(g_allocator, gfdSize, 4);
			bx::memCopy(gfd, ptr, gfdSize);
			const uint32_t headerSize = shader.fragment ? GFDGetPixelShaderHeaderSize(0, gfd) : GFDGetVertexShaderHeaderSize(0, gfd);
			const uint32_t programSize = shader.fragment ? GFDGetPixelShaderProgramSize(0, gfd) : GFDGetVertexShaderProgramSize(0, gfd);
			shader.header = allocate(headerSize);
			shader.program = allocate(programSize, GX2_SHADER_PROGRAM_ALIGNMENT);
			BOOL loaded;
			GX2UniformBlock* block;
			if (shader.fragment)
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
			shader.uniformSize = block ? block->size : 0;
			if (shader.uniformSize > 65536) unsupported("uniform blocks larger than 64 KiB");
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.program->data, programSize);
			bx::free(g_allocator, gfd, 4);
		}

		void destroyShader(ShaderHandle _handle) override
		{
			Shader& shader = m_shaders[_handle.idx];
			retire(shader.program); retire(shader.header);
			shader.uniforms.clear(); shader.predefined.clear(); shader.uniformSize = 0;
		}

		void createProgram(ProgramHandle _handle, ShaderHandle _vsh, ShaderHandle _fsh) override
		{
			if (!isValid(_fsh)) { unsupported("compute programs"); return; }
			Program& program = m_programs[_handle.idx];
			program.vertex = &m_shaders[_vsh.idx]; program.fragment = &m_shaders[_fsh.idx];
			program.m_numPredefined = 0;
			for (Shader* shader : { program.vertex, program.fragment })
				for (const PredefinedUniform& predefined : shader->predefined)
					program.m_predefined[program.m_numPredefined++] = predefined;
		}

		void destroyProgram(ProgramHandle _handle) override
		{
			Program& program = m_programs[_handle.idx];
			for (Fetch& fetch : program.fetches) retire(fetch.program);
			program.fetches.clear(); program.vertex = NULL; program.fragment = NULL;
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
			TextureLevel& level = _texture.levels[_mip];
			TextureStage& stage = level.slices[_slice];
			if (!stage.block) stage.block = allocate(level.source.imageSize, level.source.alignment);
			else if (stage.block->stamp > GX2GetRetiredTimeStamp())
			{
				Block* replacement = reuse(stage.spare, level.source.imageSize, level.source.alignment);
				stage.spare.push_back(stage.block); stage.block = replacement;
			}
			return stage;
		}

		void stageTexture(Texture& _texture, uint32_t _mip, uint32_t _slice, const Rect& _rect, const void* _data, uint32_t _pitch)
		{
			TextureLevel& level = _texture.levels[_mip];
			TextureStage& stage = textureStage(_texture, _mip, _slice);
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(_texture.format));
			const uint32_t rowBytes = ((_rect.m_width+info.blockWidth-1)/info.blockWidth)*info.blockSize;
			const uint32_t rows = (_rect.m_height+info.blockHeight-1)/info.blockHeight;
			const uint32_t pitch = _pitch == UINT32_MAX ? rowBytes : _pitch;
			const uint32_t offset = (_rect.m_y/info.blockHeight*level.source.pitch+_rect.m_x/info.blockWidth)*info.blockSize;
			for (uint32_t yy = 0; yy < rows; ++yy)
				bx::memCopy((uint8_t*)stage.block->data+offset+yy*level.source.pitch*info.blockSize, (const uint8_t*)_data+yy*pitch, rowBytes);
			Rect merged = _rect;
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

		void uploadTextures()
		{
			bool published = false;
			for (Texture* entry : m_pendingTextures)
			{
				Texture& texture = *entry;
				if (!texture.pending) continue;
				const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(texture.format));
				for (TextureLevel& level : texture.levels)
					for (TextureStage& stage : level.slices)
					{
						for (const Rect& rect : stage.dirty)
						{
						const uint32_t offset = (rect.m_y/info.blockHeight*level.source.pitch+rect.m_x/info.blockWidth)*info.blockSize;
						const uint32_t rows = (rect.m_height+info.blockHeight-1)/info.blockHeight;
						const uint32_t bytes = ((rows-1)*level.source.pitch+(rect.m_width+info.blockWidth-1)/info.blockWidth)*info.blockSize;
						DCStoreRangeNoSync((uint8_t*)stage.block->data+offset, bytes);
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
				if (texture.image->stamp > GX2GetRetiredTimeStamp())
				{
					Block* old = texture.image;
					GX2Surface previous = texture.texture.surface;
					texture.image = reuse(texture.spareImages, old->size, previous.alignment, true);
					textureStorage(texture, texture.image);
					// Preserve only on rename; all reads of the old backing remain immutable.
					for (uint32_t mip = 0; mip < previous.mipLevels; ++mip)
						for (uint32_t slice = 0; slice < texture.levels[mip].slices.size(); ++slice)
						{
							const TextureLevel& level = texture.levels[mip];
							const std::vector<Rect>& dirty = level.slices[slice].dirty;
							const bool replaced = dirty.size() == 1 && !dirty[0].m_x && !dirty[0].m_y && dirty[0].m_width == level.source.width && dirty[0].m_height == level.source.height;
							if (!replaced) GX2CopySurface(&previous, mip, slice, &texture.texture.surface, mip, slice);
						}
					touch(old); texture.spareImages.push_back(old);
				}
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
						touch(stage.block); stage.dirty.clear();
					}
				}
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, texture.image->data, texture.image->size);
				GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, texture.image->data, texture.image->size);
				if (texture.image) touch(texture.image); texture.pending = false;
			}
			m_pendingTextures.clear();
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
			texture.flags = _flags; texture.format = TextureFormat::Enum(image.m_format);
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
				texture.texture = *(const GX2Texture*)(uintptr_t)_external;
				textureLevels(texture); return NULL;
			}
			const uint32_t nativeType = uint32_t(surface.format) & 0x300;
			if ((_flags & BGFX_TEXTURE_RT_MSAA_MASK) && (nativeType == 0x100 || nativeType == 0x300) && !(_flags & (BGFX_TEXTURE_MSAA_SAMPLE | BGFX_TEXTURE_RT_WRITE_ONLY)))
			{ unsupported("automatic integer color resolve"); return NULL; }
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
			allocateTexture(texture); textureLevels(texture);
			texture.texture.viewNumMips = surface.mipLevels;
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
			if ((_flags & BGFX_TEXTURE_RT_MASK) && depth && texture.renderSurface.mipLevels == 1)
			{
				GX2DepthBuffer buffer = {}; buffer.surface = texture.renderSurface; buffer.viewNumSlices = buffer.surface.depth;
				uint32_t bytes, alignment; GX2CalcDepthBufferHiZInfo(&buffer, &bytes, &alignment);
				if (bytes) { texture.hiZ = allocate(bytes, alignment); DCInvalidateRange(texture.hiZ->data, bytes); }
			}
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(texture.format));
			for (uint32_t mip = 0; mip < surface.mipLevels; ++mip)
			{
				const Rect rect = {0, 0, uint16_t(bx::max(1u, surface.width >> mip)), uint16_t(bx::max(1u, surface.height >> mip))};
				const uint32_t rowBytes = ((rect.m_width+info.blockWidth-1)/info.blockWidth)*info.blockSize;
				const uint32_t sliceBytes = rowBytes*((rect.m_height+info.blockHeight-1)/info.blockHeight);
				for (uint32_t slice = 0; slice < texture.levels[mip].slices.size(); ++slice)
				{
					bimg::ImageMip raw;
					const bool volume = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D;
					if (bimg::imageGetRawData(image, volume ? 0 : slice, mip+_skip, _mem->data, _mem->size, raw))
						stageTexture(texture, mip, slice, rect, raw.m_data+(volume ? slice*sliceBytes : 0), rowBytes);
				}
			}
			return NULL;
		}

		void updateTexture(TextureHandle _handle, uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem) override
		{
			Texture& texture = m_textures[_handle.idx];
			const bimg::ImageBlockInfo& info = bimg::getBlockInfo(bimg::TextureFormat::Enum(texture.format));
			const uint32_t rowBytes = ((_rect.m_width+info.blockWidth-1)/info.blockWidth)*info.blockSize;
			const uint32_t pitch = _pitch == UINT16_MAX ? rowBytes : _pitch;
			const uint32_t sliceBytes = pitch*((_rect.m_height+info.blockHeight-1)/info.blockHeight);
			for (uint32_t slice = 0; slice < _depth; ++slice)
				stageTexture(texture, _mip, (texture.texture.surface.dim == GX2_SURFACE_DIM_TEXTURE_CUBE ? _z*6+_side : _z)+slice, _rect, _mem->data+slice*sliceBytes, pitch);
		}

		void destroyTexture(TextureHandle _handle) override
		{
			Texture& texture = m_textures[_handle.idx];
			retire(texture.image); retire(texture.renderImage); retire(texture.auxiliary); retire(texture.hiZ);
			retire(texture.depthSamplesImage); retire(texture.resolvedDepthImage);
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
		void clearTexture(TextureHandle, uint8_t, uint8_t, uint16_t, uint16_t) override { unsupported("texture clear"); }
		void readTexture(TextureHandle, void*, uint16_t, uint8_t) override { unsupported("texture readback"); }
		void readBuffer(Handle, void*, uint32_t, uint32_t) override { unsupported("buffer readback"); }
		void resizeTexture(TextureHandle, uint16_t, uint16_t, uint8_t, uint16_t) override { unsupported("texture resize"); }
		void createFrameBuffer(FrameBufferHandle _handle, uint8_t _num, const Attachment* _attachments) override
		{ m_frameBuffers[_handle.idx].attachments.assign(_attachments, _attachments+_num); }
		void createFrameBuffer(FrameBufferHandle, const SwapChain&) override { unsupported("additional swap chains"); }
		void destroyFrameBuffer(FrameBufferHandle _handle) override { m_frameBuffers[_handle.idx].attachments.clear(); }
		void requestScreenShot(FrameBufferHandle, const char*) override { unsupported("screenshots"); }
		void invalidateOcclusionQuery(OcclusionQueryHandle _handle) override
		{
			// A reused bgfx handle must never receive an old allocation's result.
			for (OcclusionSample& sample : m_pendingOcclusion)
				if (sample.handle.idx == _handle.idx) sample.handle = BGFX_INVALID_HANDLE;
		}

		void resolveOcclusion(Frame* _render)
		{
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
				}
				m_freeQueryBlocks.push_back(sample.block);
			}
			m_pendingOcclusion.erase(m_pendingOcclusion.begin(), m_pendingOcclusion.begin() + consumed);
		}

		bool isVisible(Frame* _render, OcclusionQueryHandle _handle, bool _visible)
		{
			resolveOcclusion(_render);
			// Same last-available CPU result policy as this pin's GL/VK/WebGPU.
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
			Block* begin = reuse(m_freeQueryBlocks, 64, 64, true);
			// The native sampling call publishes its sentinel in an owned 64-byte slot.
			GX2SampleBottomGPUCycle((uint64_t*)begin->data);
			touch(begin);
			const uint32_t index = uint32_t(m_pendingGpuTimers.size());
			m_pendingGpuTimers.push_back({ begin, NULL, _frameNum, _view });
			return index;
		}

		void endGpuTimer(uint32_t _index)
		{
			GpuTimerSample& sample = m_pendingGpuTimers[_index];
			sample.end = reuse(m_freeQueryBlocks, 64, 64, true);
			GX2SampleBottomGPUCycle((uint64_t*)sample.end->data);
			touch(sample.end);
		}

		void resolveGpuTimers(Frame* _render)
		{
			const OSTime completed = GX2GetRetiredTimeStamp();
			size_t consumed = 0;
			for (; consumed < m_pendingGpuTimers.size(); ++consumed)
			{
				const GpuTimerSample& sample = m_pendingGpuTimers[consumed];
				// All intervals are closed before the next submit's resolver. The end's
				// submission covers its earlier begin, even across batch splits/seals.
				if (sample.end->stamp > completed) break;
				DCInvalidateRange(sample.begin->data, sample.begin->size);
				DCInvalidateRange(sample.end->data, sample.end->size);
				// GX2's cycle API exposes native u64 values, unlike raw query regs.
				const uint64_t begin = *(const uint64_t*)sample.begin->data;
				const uint64_t end = *(const uint64_t*)sample.end->data;
				if (begin != GX2_GPU_TIMESTAMP_INVALID && end != GX2_GPU_TIMESTAMP_INVALID)
				{
					GpuTimerResult& result = m_gpuTimerResults[sample.view];
					result.begin = int64_t(begin);
					result.end = int64_t(end);
					result.frameNum = sample.frameNum;
					if (sample.view == BGFX_CONFIG_MAX_VIEWS)
						m_maxGpuLatency = bx::max(m_maxGpuLatency, _render->m_frameNum - sample.frameNum);
				}
				// An invalid retired timestamp cannot become valid later: keep the
				// last valid stats, and recycle without a readiness spin or a wait.
				m_freeQueryBlocks.push_back(sample.begin);
				m_freeQueryBlocks.push_back(sample.end);
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
			if (0 == (_render->m_debug & BGFX_DEBUG_PROFILER)) return UINT32_MAX;
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
			ViewStats& stats = _render->m_perfStats.viewStats[_render->m_perfStats.numViews++];
			stats.cpuTimeEnd = bx::getHPCounter();
			const GpuTimerResult& result = m_gpuTimerResults[stats.view];
			stats.gpuTimeBegin = result.begin;
			stats.gpuTimeEnd = result.end;
			stats.gpuFrameNum = result.frameNum;
		}
		void setMarker(const char*, uint16_t) override {}
		void setName(Handle, const char*, uint16_t) override {}

		GX2ColorBuffer colorBuffer(Texture& _texture, const RenderView& _view, bool _srgb = false)
		{
			GX2ColorBuffer color = {};
			color.surface = _texture.renderSurface;
			if (!_texture.renderImage) color.surface = _texture.texture.surface;
			if (_srgb) { uint32_t comp; color.surface.format = textureFormat(_texture.format, true, comp); }
			color.viewMip = _view.mip; color.viewFirstSlice = _view.layer; color.viewNumSlices = _view.count;
			if (_texture.auxiliary) { color.aaBuffer = _texture.auxiliary->data; color.aaSize = _texture.auxiliary->size; }
			GX2InitColorBufferRegs(&color); return color;
		}

		GX2DepthBuffer depthBuffer(Texture& _texture, const RenderView& _view)
		{
			GX2DepthBuffer depth = {};
			depth.surface = _texture.renderImage ? _texture.renderSurface : _texture.texture.surface;
			depth.depthClear = _texture.depthClear; depth.stencilClear = _texture.stencilClear;
			depth.viewMip = _view.mip; depth.viewFirstSlice = _view.layer; depth.viewNumSlices = _view.count;
			if (_texture.hiZ) { depth.hiZPtr = _texture.hiZ->data; depth.hiZSize = _texture.hiZ->size; }
			GX2InitDepthBufferRegs(&depth);
			GX2InitDepthBufferHiZEnable(&depth, depth.hiZPtr != NULL); return depth;
		}

		void bindFrameBuffer(FrameBufferHandle _handle)
		{
			m_currentFrameBuffer = _handle;
			m_numColors = 0; m_hasDepth = false; m_depthReadOnly = false; m_stencilReadOnly = false;
			if (!isValid(_handle))
			{
				m_colors[0] = *m_target.color; m_numColors = 1;
				m_activeDepth = m_depth; m_hasDepth = true;
				m_targetWidth = m_colors[0].surface.width; m_targetHeight = m_colors[0].surface.height;
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
					else m_colors[m_numColors++] = colorBuffer(texture, view, 0 != (attachment.flags & BGFX_ATTACHMENT_SRGB));
					m_targetWidth = bx::max(1u, texture.texture.surface.width >> attachment.mip);
					m_targetHeight = bx::max(1u, texture.texture.surface.height >> attachment.mip);
					if (texture.image) touch(texture.image);
					if (texture.renderImage) touch(texture.renderImage);
					if (texture.auxiliary) touch(texture.auxiliary);
					if (texture.hiZ) touch(texture.hiZ);
				}
			}
			for (uint32_t ii = 0; ii < m_numColors; ++ii) GX2SetColorBuffer(&m_colors[ii], GX2RenderTarget(ii));
			if (m_hasDepth) GX2SetDepthBuffer(&m_activeDepth);
			GX2SetAAMode(m_numColors ? m_colors[0].surface.aa : m_activeDepth.surface.aa);
			m_cachedState = UINT64_MAX;
		}

		void markRendered(Texture& _texture, const RenderView& _view)
		{
			for (RenderView& view : _texture.rendered)
				if (view.mip == _view.mip && view.layer == _view.layer && view.count == _view.count) { view.autoMips |= _view.autoMips; return; }
			_texture.rendered.push_back(_view);
		}

		void finishRenderPass()
		{
			uint32_t colorIndex = 0;
			if (isValid(m_currentFrameBuffer))
				for (const Attachment& attachment : m_frameBuffers[m_currentFrameBuffer.idx].attachments)
				{
					Texture& texture = m_textures[attachment.handle.idx];
					const bool depth = bimg::isDepth(bimg::TextureFormat::Enum(texture.format));
					const bool written = depth ? m_depthWritten : 0 != (m_colorWritten & (1u << colorIndex++));
					if (!written) continue;
					Block* block = texture.renderImage ? texture.renderImage : texture.image;
					if (block) GX2Invalidate(depth ? GX2_INVALIDATE_MODE_DEPTH_BUFFER : GX2_INVALIDATE_MODE_COLOR_BUFFER, block->data, block->size);
					if (!(texture.flags & BGFX_TEXTURE_RT_WRITE_ONLY)) markRendered(texture, {attachment.mip, attachment.layer, attachment.numLayers, 0 != (attachment.flags & BGFX_ATTACHMENT_AUTO_GEN_MIPS)});
				}
			else if (m_depthWritten) GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, m_depthImage->data, m_depthImage->size);
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

		void fullscreen(Program& _program, const GX2Texture& _source, GX2ColorBuffer* _color, GX2DepthBuffer* _depth, uint32_t _width, uint32_t _height, const float* _params = NULL)
		{
			restore();
			if (_color) GX2SetColorBuffer(_color, GX2_RENDER_TARGET_0);
			if (_depth) GX2SetDepthBuffer(_depth);
			GX2SetAAMode(GX2_AA_MODE1X);
			GX2SetViewport(0, 0, _width, _height, -1, 1); GX2SetScissor(0, 0, _width, _height);
			GX2SetDepthStencilControl(_depth != NULL, _depth != NULL, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE,
				GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
				GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
			GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, _color != NULL); channelMasks(15, _color ? 1 : 0);
			GX2Sampler sampler; GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
			GX2InitSamplerZMFilter(&sampler, GX2_TEX_Z_FILTER_MODE_LINEAR, GX2_TEX_MIP_FILTER_MODE_NONE); GX2InitSamplerLOD(&sampler, 0, 0, 0);
			GX2SetPixelSampler(&sampler, 0); GX2SetPixelTexture(&_source, 0);
			if (_params) updateUniform(m_uniformReg.find("u_mipGen")->m_handle.idx, _params, 16);
			beginBatch(); uniformsBegin(_program); uniformsEnd(_program);
			RenderDraw draw; draw.clear(); draw.m_streamMask = 0; vertexInput(_program, draw);
			GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, 1); endBatch();
		}

		void resolveDepth(Texture& _texture, const RenderView& _view)
		{
			GX2DepthBuffer depth = depthBuffer(_texture, _view);
			if (depth.surface.aa == GX2_AA_MODE1X || (_texture.flags & BGFX_TEXTURE_MSAA_SAMPLE))
			{
				if (_texture.renderImage) GX2ConvertDepthBufferToTextureSurface(&depth, &_texture.texture.surface, _view.mip, _view.layer);
				else if (_texture.hiZ) GX2ExpandDepthBuffer(&depth);
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
			for (uint32_t slice = _view.layer; slice < uint32_t(_view.layer)+_view.count; ++slice)
			{
				GX2Texture source = _texture.texture; source.surface = _texture.depthSamples;
				source.viewFirstMip = 0; source.viewNumMips = 1; source.viewFirstSlice = slice; source.viewNumSlices = 1; GX2InitTextureRegs(&source);
				GX2DepthBuffer dest = {}; dest.surface = _texture.resolvedDepth; dest.viewFirstSlice = slice; dest.viewNumSlices = 1; GX2InitDepthBufferRegs(&dest);
				Program& program = m_programs[g_depthResolve->m_program[source.surface.depth > 1 ? 1 : 0].idx];
				fullscreen(program, source, NULL, &dest, dest.surface.width, dest.surface.height);
			}
			GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, _texture.resolvedDepthImage->data, _texture.resolvedDepthImage->size);
			GX2DepthBuffer resolved = {}; resolved.surface = _texture.resolvedDepth; resolved.viewFirstSlice = _view.layer; resolved.viewNumSlices = _view.count; GX2InitDepthBufferRegs(&resolved);
			GX2ConvertDepthBufferToTextureSurface(&resolved, &_texture.texture.surface, 0, _view.layer);
			touch(_texture.depthSamplesImage); touch(_texture.resolvedDepthImage);
		}

		void generateMips(Texture& _texture, const RenderView& _view)
		{
			if (!_view.autoMips || !MipGen::isSupported(_texture.format) || _texture.texture.surface.mipLevels <= _view.mip+1) return;
			const GX2Surface& surface = _texture.texture.surface;
			const bool volume = surface.dim == GX2_SURFACE_DIM_TEXTURE_3D;
			const bool array = surface.dim != GX2_SURFACE_DIM_TEXTURE_2D && !volume;
			Program& program = m_programs[m_mipGen->m_program[volume ? 2 : array ? 1 : 0].idx];
			for (uint32_t mip = _view.mip+1; mip < surface.mipLevels; ++mip)
			{
				const uint32_t width = bx::max(1u, surface.width >> mip), height = bx::max(1u, surface.height >> mip);
				const uint32_t first = volume ? 0 : _view.layer, count = volume ? bx::max(1u, surface.depth >> mip) : _view.count;
				for (uint32_t slice = first; slice < first+count; ++slice)
				{
					GX2Texture source = _texture.texture; source.viewFirstMip = mip-1; source.viewNumMips = 1;
					if (!volume) { source.surface.dim = array ? GX2_SURFACE_DIM_TEXTURE_2D_ARRAY : GX2_SURFACE_DIM_TEXTURE_2D; source.viewFirstSlice = slice; source.viewNumSlices = 1; }
					GX2InitTextureRegs(&source);
					GX2ColorBuffer dest = {}; dest.surface = surface; dest.viewMip = mip; dest.viewFirstSlice = slice; dest.viewNumSlices = 1; GX2InitColorBufferRegs(&dest);
					const float params[4] = {1.0f/width, 1.0f/height, (float(slice)+0.5f)/count, 0};
					fullscreen(program, source, &dest, NULL, width, height, params);
				}
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _texture.image->data, _texture.image->size);
				GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _texture.image->data, _texture.image->size);
			}
		}


		bool textureNeedsHelper(const Texture& _texture)
		{
			if (_texture.renderImage || _texture.hiZ || _texture.renderSurface.aa != GX2_AA_MODE1X) return true;
			for (const RenderView& view : _texture.rendered)
				if (view.autoMips && MipGen::isSupported(_texture.format) && _texture.texture.surface.mipLevels > view.mip+1) return true;
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
					resolveDepth(_texture, view); helperWrites |= _texture.renderImage || _texture.hiZ;
				}
				else if (_texture.renderSurface.aa != GX2_AA_MODE1X)
				{
					GX2ColorBuffer color = colorBuffer(_texture, view);
					if (_texture.flags & BGFX_TEXTURE_MSAA_SAMPLE) GX2ExpandAAColorBuffer(&color);
					else GX2ResolveAAColorBuffer(&color, &_texture.texture.surface, view.mip, view.layer);
					helperWrites = true;
				}
			}
			if (helperWrites)
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _texture.texture.surface.image, _texture.image ? _texture.image->size : _texture.texture.surface.imageSize);
			GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _texture.texture.surface.image, _texture.image ? _texture.image->size : _texture.texture.surface.imageSize);
			for (const RenderView& view : _texture.rendered) generateMips(_texture, view);
			_texture.rendered.clear();
			if (_texture.image) touch(_texture.image);
		}

		bool prepareTextures(const RenderBind& _bind)
		{
			bool restored = false;
			for (const Binding& bind : _bind.m_bind)
			{
				if (bind.m_idx == kInvalidHandle || bind.m_type != Binding::Texture) continue;
				Texture& texture = m_textures[bind.m_idx];
				if (texture.rendered.empty()) continue;
				if (!restored && textureNeedsHelper(texture)) { endBatch(); restored = true; }
				resolveTexture(texture);
			}
			if (restored) { restore(); bindFrameBuffer(m_currentFrameBuffer); beginBatch(); }
			return restored;
		}

		void restore()
		{
			GX2SetContextState(m_target.context);
			GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
			GX2SetRasterizerClipControlEx(TRUE, TRUE, TRUE);
			GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
			GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
			GX2SetAlphaToMask(FALSE, GX2_ALPHA_TO_MASK_MODE_NON_DITHERED);
			GX2SetLineWidth(1.0f);
			m_depthControl = {};
			GX2SetAAMask(0xff, 0xff, 0xff, 0xff); m_sampleMask = UINT32_MAX;
			m_cachedState = UINT64_MAX;
			m_cachedStencil = UINT64_MAX;
			m_cachedRgba = UINT32_MAX;
			m_boundProgram = NULL;
			m_boundFetch = NULL;
			bx::memSet(m_boundAttributes, 0, sizeof(m_boundAttributes));
			bx::memSet(m_boundTextures, 0, sizeof(m_boundTextures));
			m_borderValid = 0;

		}

		void beginBatch()
		{
			m_list = acquire(m_lists, 256*1024, true);
			m_upload = acquire(m_uploads, 256*1024);
			touch(m_list); touch(m_upload);
			m_uploadUsed = 0;
			m_batchDraws = 0;
			GX2BeginDisplayListEx(m_list->data, m_list->size, TRUE);
		}

		void endBatch()
		{
			const uint32_t bytes = GX2EndDisplayList(m_list->data);
			if (m_uploadUsed)
			{
				DCFlushRange(m_upload->data, m_uploadUsed);
				GX2Invalidate(GX2_INVALIDATE_MODE_UNIFORM_BLOCK, NULL, UINT32_MAX);
			}
			GX2CallDisplayList(m_list->data, bytes);
		}

		void uniformsBegin(Program& _program)
		{
			const uint32_t bytes = bx::alignUp(_program.vertex->uniformSize, 256)+bx::alignUp(_program.fragment->uniformSize, 256);
			if (m_batchDraws == 32 || m_uploadUsed+bytes > m_upload->size)
			{
				endBatch(); beginBatch();
			}
			++m_batchDraws;
			for (uint32_t stage = 0; stage < 2; ++stage)
			{
				Shader& shader = *(stage ? _program.fragment : _program.vertex);
				m_stageData[stage] = (uint8_t*)m_upload->data + m_uploadUsed;
				m_uploadUsed += bx::alignUp(shader.uniformSize, 256);
				// Every active field is copied below or supplied by the predefined-uniform pass.
				for (const Uniform& uniform : shader.uniforms)
				{
					uint8_t* dst = m_stageData[stage]+uniform.offset*4;
					const float* src = (const float*)m_uniforms[uniform.handle.idx];
					if (uniform.type == UniformType::Mat3)
					{
						for (uint32_t ii = 0; ii < uint32_t(uniform.count)*3; ++ii)
							writeFloats(dst+ii*16, src+ii*3, 3);
					}
					else writeFloats(dst, src, uniform.rows*4);
				}
			}
		}

		void setShaderUniform4f(uint8_t _flags, uint32_t _loc, const void* _data, uint32_t _num)
		{ writeFloats(m_stageData[0 != (_flags & kUniformFragmentBit)]+_loc*4, _data, _num*4); }
		void setShaderUniform4x4f(uint8_t _flags, uint32_t _loc, const void* _data, uint32_t _num)
		{ setShaderUniform4f(_flags, _loc, _data, _num); }

		void uniformsEnd(Program& _program)
		{
			if (_program.vertex->uniformSize) GX2SetVertexUniformBlock(0, _program.vertex->uniformSize, m_stageData[0]);
			if (_program.fragment->uniformSize) GX2SetPixelUniformBlock(0, _program.fragment->uniformSize, m_stageData[1]);
			if (m_boundProgram != &_program)
			{
				GX2SetVertexShader((const GX2VertexShader*)_program.vertex->header->data);
				GX2SetPixelShader((const GX2PixelShader*)_program.fragment->header->data);
				touch(_program.vertex->program); touch(_program.vertex->header);
				touch(_program.fragment->program); touch(_program.fragment->header);
				m_boundProgram = &_program;
			}
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
			stream.endianSwap = type <= AttribType::Uint8 ? GX2_ENDIAN_SWAP_NONE
				: (type == AttribType::Int16 || type == AttribType::Uint16 || type == AttribType::Half) ? GX2_ENDIAN_SWAP_8_IN_16 : GX2_ENDIAN_SWAP_8_IN_32;
			return stream;
		}

		void vertexInput(Program& _program, const RenderDraw& _draw)
		{
			const uint32_t streamMask = _draw.m_streamMask == UINT32_MAX ? 0 : _draw.m_streamMask;
			uint32_t layouts[BGFX_CONFIG_MAX_VERTEX_STREAMS] = {};
			for (uint32_t ii = 0; ii < BGFX_CONFIG_MAX_VERTEX_STREAMS; ++ii)
			{
				if (0 == (streamMask & (1<<ii))) continue;
				const Stream& stream = _draw.m_stream[ii];
				Buffer& buffer = m_vertexBuffers[stream.m_handle.idx];
				const VertexLayout& layout = m_layouts[isValid(stream.m_layoutHandle) ? stream.m_layoutHandle.idx : buffer.layout.idx];
				layouts[ii] = layout.m_hash;
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
				const void* ptr = (uint8_t*)buffer.block->data+_draw.m_instanceDataOffset;
				GX2SetAttribBuffer(BGFX_CONFIG_MAX_VERTEX_STREAMS, buffer.size-_draw.m_instanceDataOffset, instanceStride, ptr);
				touch(buffer.block);
			}
			Fetch* fetch = NULL;
			for (Fetch& candidate : _program.fetches)
				if (candidate.streamMask == streamMask && candidate.instanceStride == instanceStride
				&& 0 == bx::memCmp(layouts, candidate.layouts, sizeof(layouts))) { fetch = &candidate; break; }
			if (!fetch)
			{
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
							if (layout.has(Attrib::Enum(aa)) && 0 == bx::strCmp(var.name, getAttribName(Attrib::Enum(aa))))
							{ attributes[count++] = attribute(var, layout, Attrib::Enum(aa), ss); found = true; break; }
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
							stream.mask = GX2_SEL_MASK(0, 1, 2, 3); stream.endianSwap = GX2_ENDIAN_SWAP_8_IN_32;
							attributes[count++] = stream; found = true;
						}
					}
					if (!found) { unsupported("missing vertex attribute"); return; }
				}
				Fetch native = {};
				bx::memCopy(native.layouts, layouts, sizeof(layouts));
				native.streamMask = _draw.m_streamMask; native.instanceStride = instanceStride;
				const uint32_t size = GX2CalcFetchShaderSizeEx(count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
				native.program = allocate(size, GX2_SHADER_PROGRAM_ALIGNMENT);
				GX2InitFetchShaderEx(&native.shader, (uint8_t*)native.program->data, count, attributes, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
				GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, native.program->data, size);
				_program.fetches.push_back(native); fetch = &_program.fetches.back();
			}
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

		void state(uint64_t _state, uint64_t _stencil, uint32_t _rgba)
		{
			if (_state & BGFX_STATE_CONSERVATIVE_RASTER) unsupported("conservative raster");
			const bool invalid = m_cachedState == UINT64_MAX;
			if (_state != m_cachedState || _stencil != m_cachedStencil || ((_state & BGFX_STATE_BLEND_INDEPENDENT) && _rgba != m_cachedRgba))
			{
				const uint32_t front = uint32_t(_stencil);
				const uint32_t back = uint32_t(_stencil >> 32) ? uint32_t(_stencil >> 32) : front;
				GX2SetDepthStencilControl(m_hasDepth && 0 != (_state & BGFX_STATE_DEPTH_TEST_MASK), m_hasDepth && !m_depthReadOnly && 0 != (_state & BGFX_STATE_WRITE_Z),
					s_compare[(_state & BGFX_STATE_DEPTH_TEST_MASK) >> BGFX_STATE_DEPTH_TEST_SHIFT],
					m_hasDepth && 0 != ((front | back) & BGFX_STENCIL_TEST_MASK), front != back,
					s_compare[(front & BGFX_STENCIL_TEST_MASK) >> BGFX_STENCIL_TEST_SHIFT],
					s_stencil[(front & BGFX_STENCIL_OP_PASS_Z_MASK) >> BGFX_STENCIL_OP_PASS_Z_SHIFT],
					s_stencil[(front & BGFX_STENCIL_OP_FAIL_Z_MASK) >> BGFX_STENCIL_OP_FAIL_Z_SHIFT],
					s_stencil[(front & BGFX_STENCIL_OP_FAIL_S_MASK) >> BGFX_STENCIL_OP_FAIL_S_SHIFT],
					s_compare[(back & BGFX_STENCIL_TEST_MASK) >> BGFX_STENCIL_TEST_SHIFT],
					s_stencil[(back & BGFX_STENCIL_OP_PASS_Z_MASK) >> BGFX_STENCIL_OP_PASS_Z_SHIFT],
					s_stencil[(back & BGFX_STENCIL_OP_FAIL_Z_MASK) >> BGFX_STENCIL_OP_FAIL_Z_SHIFT],
					s_stencil[(back & BGFX_STENCIL_OP_FAIL_S_MASK) >> BGFX_STENCIL_OP_FAIL_S_SHIFT]);
				GX2SetStencilMask(uint8_t(front >> 8), m_stencilReadOnly ? 0 : 0xff, uint8_t(front), uint8_t(back >> 8), m_stencilReadOnly ? 0 : 0xff, uint8_t(back));
				const bool ccw = 0 != (_state & BGFX_STATE_FRONT_CCW);
				const bool bias = m_depthControl.m_constant != 0 || m_depthControl.m_slopeScale != 0;
				GX2SetPolygonControl(ccw ? GX2_FRONT_FACE_CCW : GX2_FRONT_FACE_CW,
					0 != (_state & BGFX_STATE_CULL_CW), 0 != (_state & BGFX_STATE_CULL_CCW), FALSE,
					GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE, bias, bias, bias);
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
				GX2SetColorControl(GX2_LOGIC_OP_COPY, blendMask, FALSE, m_numColors != 0);
				const uint32_t mask = (_state & BGFX_STATE_WRITE_R ? 1 : 0) | (_state & BGFX_STATE_WRITE_G ? 2 : 0)
					| (_state & BGFX_STATE_WRITE_B ? 4 : 0) | (_state & BGFX_STATE_WRITE_A ? 8 : 0);
				channelMasks(mask, (1u << m_numColors)-1);
				GX2SetAlphaToMask(0 != (_state & BGFX_STATE_BLEND_ALPHA_TO_COVERAGE), GX2_ALPHA_TO_MASK_MODE_NON_DITHERED);
				GX2SetPointSize(float((_state & BGFX_STATE_POINT_SIZE_MASK) >> BGFX_STATE_POINT_SIZE_SHIFT), float((_state & BGFX_STATE_POINT_SIZE_MASK) >> BGFX_STATE_POINT_SIZE_SHIFT));
				m_cachedState = _state; m_cachedStencil = _stencil;
			}
			if (invalid || m_cachedRgba != _rgba)
			{
				GX2SetBlendConstantColor(float(_rgba >> 24)/255, float((_rgba >> 16)&255)/255, float((_rgba >> 8)&255)/255, float(_rgba&255)/255);
				m_cachedRgba = _rgba;
			}
		}

		void textures(const RenderBind& _bind, const float _palette[][4])
		{
			static const GX2TexClampMode clamps[] = { GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_CLAMP_MODE_MIRROR, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_CLAMP_BORDER };
			for (uint32_t ii = 0; ii < 16; ++ii)
			{
				const Binding& bind = _bind.m_bind[ii];
				if (bind.m_idx == kInvalidHandle) continue;
				if (bind.m_type != Binding::Texture) { unsupported("image and compute bindings"); continue; }
				Texture& texture = m_textures[bind.m_idx];
				const uint32_t flags = bind.m_samplerFlags & BGFX_SAMPLER_INTERNAL_DEFAULT ? uint32_t(texture.flags) : bind.m_samplerFlags;
				if (m_boundTextures[ii] != &texture.texture || !(m_boundBindings[ii] == bind))
				{
					GX2Texture view = texture.texture;
					view.viewFirstMip = bind.m_firstMip;
					view.viewNumMips = bx::min<uint32_t>(bind.m_numMips, view.surface.mipLevels-bind.m_firstMip);
					view.viewFirstSlice = bind.m_firstLayer;
					view.viewNumSlices = bx::min<uint32_t>(bind.m_numLayers, view.surface.depth-bind.m_firstLayer);
					if (flags & BGFX_SAMPLER_SRGB) view.surface.format = textureFormat(texture.format, true, view.compMap);
					if (flags & BGFX_SAMPLER_SAMPLE_STENCIL)
					{
						view.surface.format = GX2_SURFACE_FORMAT_UINT_X24_G8;
						view.compMap = GX2_COMP_MAP(1, 4, 4, 5);
					}
					GX2InitTextureRegs(&view);
					GX2SetPixelTexture(&view, ii); GX2SetVertexTexture(&view, ii);
					GX2Sampler sampler;
					GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);
					GX2InitSamplerClamping(&sampler, clamps[(flags & BGFX_SAMPLER_U_MASK) >> BGFX_SAMPLER_U_SHIFT],
						clamps[(flags & BGFX_SAMPLER_V_MASK) >> BGFX_SAMPLER_V_SHIFT], clamps[(flags & BGFX_SAMPLER_W_MASK) >> BGFX_SAMPLER_W_SHIFT]);
					GX2InitSamplerXYFilter(&sampler, flags & BGFX_SAMPLER_MAG_POINT ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR,
						flags & BGFX_SAMPLER_MIN_POINT ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR,
						flags & (BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC) ? GX2_TEX_ANISO_RATIO_4_TO_1 : GX2_TEX_ANISO_RATIO_NONE);
					GX2InitSamplerZMFilter(&sampler, flags & BGFX_SAMPLER_MIN_POINT ? GX2_TEX_Z_FILTER_MODE_POINT : GX2_TEX_Z_FILTER_MODE_LINEAR,
						view.viewNumMips <= 1 ? GX2_TEX_MIP_FILTER_MODE_NONE : flags & BGFX_SAMPLER_MIP_POINT ? GX2_TEX_MIP_FILTER_MODE_POINT : GX2_TEX_MIP_FILTER_MODE_LINEAR);
					const float maxLod = float(view.viewNumMips-1);
					GX2InitSamplerLOD(&sampler, bx::min(maxLod, float(bind.m_lod.min)*0.25f),
						bind.m_lod.max == UINT8_MAX ? maxLod : bx::min(maxLod, float(bind.m_lod.max)*0.25f), 0);
					if (flags & BGFX_SAMPLER_SAMPLE_STENCIL)
					{
						GX2InitSamplerXYFilter(&sampler, GX2_TEX_XY_FILTER_MODE_POINT, GX2_TEX_XY_FILTER_MODE_POINT, GX2_TEX_ANISO_RATIO_NONE);
						GX2InitSamplerZMFilter(&sampler, GX2_TEX_Z_FILTER_MODE_POINT, view.viewNumMips > 1 ? GX2_TEX_MIP_FILTER_MODE_POINT : GX2_TEX_MIP_FILTER_MODE_NONE);
					}
					GX2InitSamplerDepthCompare(&sampler, s_compare[(flags & BGFX_SAMPLER_COMPARE_MASK) >> BGFX_SAMPLER_COMPARE_SHIFT]);
					GX2InitSamplerBorderType(&sampler, GX2_TEX_BORDER_TYPE_VARIABLE);
					GX2SetPixelSampler(&sampler, ii); GX2SetVertexSampler(&sampler, ii);
					m_boundTextures[ii] = &texture.texture; m_boundBindings[ii] = bind;
				}
				if ((flags & BGFX_SAMPLER_U_MASK) == BGFX_SAMPLER_U_BORDER
				|| (flags & BGFX_SAMPLER_V_MASK) == BGFX_SAMPLER_V_BORDER
				|| (flags & BGFX_SAMPLER_W_MASK) == BGFX_SAMPLER_W_BORDER)
				{
					const float* color = _palette[(flags & BGFX_SAMPLER_BORDER_COLOR_MASK) >> BGFX_SAMPLER_BORDER_COLOR_SHIFT];
					if (!(m_borderValid & (1u << ii)) || 0 != bx::memCmp(m_borderColors[ii], color, 16))
					{
						GX2SetPixelSamplerBorderColor(ii, color[0], color[1], color[2], color[3]);
						GX2SetVertexSamplerBorderColor(ii, color[0], color[1], color[2], color[3]);
						bx::memCopy(m_borderColors[ii], color, 16); m_borderValid |= 1u << ii;
					}
				}
				if (texture.image) touch(texture.image);
			}
		}

		void depthBuffer(uint32_t _width, uint32_t _height)
		{
			if (m_depth.surface.width != _width || m_depth.surface.height != _height)
			{
				if (m_depthImage)
				{
					GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, m_depthImage->data, m_depthImage->size);
					retire(m_depthImage);
				}
				m_depth = {};
				m_depth.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
				m_depth.surface.width = _width; m_depth.surface.height = _height;
				m_depth.surface.depth = 1; m_depth.surface.mipLevels = 1;
				m_depth.surface.format = GX2_SURFACE_FORMAT_UNORM_R24_X8;
				m_depth.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
				m_depth.surface.tileMode = GX2_TILE_MODE_DEFAULT;
				GX2CalcSurfaceSizeAndAlignment(&m_depth.surface);
				m_depthImage = allocate(m_depth.surface.imageSize, m_depth.surface.alignment);
				DCInvalidateRange(m_depthImage->data, m_depthImage->size);
				m_depth.surface.image = m_depthImage->data;
				m_depth.viewNumSlices = 1;
				GX2InitDepthBufferRegs(&m_depth);
			}
			GX2SetDepthBuffer(&m_depth); touch(m_depthImage);
		}

		void viewport(const View& _view)
		{
			const Rect& rect = _view.m_rect;
			GX2SetViewport(rect.m_x, rect.m_y, rect.m_width, rect.m_height, 2*_view.m_minDepth-_view.m_maxDepth, _view.m_maxDepth);
		}

		void clear(const View& _view, const ClearQuad& _quad, const float _palette[][4])
		{
			const Clear& clear = _view.m_clear;
			if (0 == (clear.m_flags & BGFX_CLEAR_MASK)) return;
			float colors[8][4] = {};
			uint32_t colorMask = 0;
			if (clear.m_flags & BGFX_CLEAR_COLOR)
				for (uint32_t ii = 0; ii < m_numColors; ++ii)
				{
					if (clear.m_flags & BGFX_CLEAR_COLOR_USE_PALETTE)
					{
						if (clear.m_index[ii] == UINT8_MAX) continue;
						bx::memCopy(colors[ii], _palette[clear.m_index[ii]], 16);
					}
					else for (uint32_t cc = 0; cc < 4; ++cc) colors[ii][cc] = float(clear.m_index[cc])/255;
					colorMask |= 1u << ii;
				}
			const GX2ClearFlags depthFlags = GX2ClearFlags(((clear.m_flags & BGFX_CLEAR_DEPTH) && !m_depthReadOnly ? GX2_CLEAR_FLAGS_DEPTH : 0) | ((clear.m_flags & BGFX_CLEAR_STENCIL) && !m_stencilReadOnly ? GX2_CLEAR_FLAGS_STENCIL : 0));
			const Rect& rect = _view.m_clippedRect;
			if (rect.m_x == 0 && rect.m_y == 0 && rect.m_width == m_targetWidth && rect.m_height == m_targetHeight)
			{
				endBatch();
				for (uint32_t ii = 0; ii < m_numColors; ++ii)
					if (colorMask & (1u << ii)) GX2ClearColor(&m_colors[ii], colors[ii][0], colors[ii][1], colors[ii][2], colors[ii][3]);
				if (m_hasDepth && depthFlags)
				{
					GX2ClearDepthStencilEx(&m_activeDepth, clear.m_depth, clear.m_stencil, depthFlags);
					if (depthFlags & GX2_CLEAR_FLAGS_DEPTH) m_activeDepth.depthClear = clear.m_depth;
					if (depthFlags & GX2_CLEAR_FLAGS_STENCIL) m_activeDepth.stencilClear = clear.m_stencil;
					if (isValid(m_currentFrameBuffer))
						for (const Attachment& attachment : m_frameBuffers[m_currentFrameBuffer.idx].attachments)
						{
							Texture& texture = m_textures[attachment.handle.idx];
							if (!bimg::isDepth(bimg::TextureFormat::Enum(texture.format))) continue;
							if (depthFlags & GX2_CLEAR_FLAGS_DEPTH) texture.depthClear = clear.m_depth;
							if (depthFlags & GX2_CLEAR_FLAGS_STENCIL) texture.stencilClear = clear.m_stencil;
						}
					else m_depth = m_activeDepth;
				}
				restore(); bindFrameBuffer(m_currentFrameBuffer); beginBatch(); viewport(_view);
			}
			else
			{
				const Rect& viewportRect = _view.m_rect;
				GX2SetViewport(viewportRect.m_x, viewportRect.m_y, viewportRect.m_width, viewportRect.m_height, -1, 1);
				GX2SetScissor(rect.m_x, rect.m_y, rect.m_width, rect.m_height);
				Program& program = m_programs[_quad.m_program[bx::max<uint32_t>(1, m_numColors)-1].idx];
				const UniformRegInfo* colorUniform = m_uniformReg.find("bgfx_clear_color");
				const UniformRegInfo* depthUniform = m_uniformReg.find("bgfx_clear_depth");
				updateUniform(colorUniform->m_handle.idx, colors, sizeof(colors));
				float depth[4] = { clear.m_depth, 0, 0, 0 }; updateUniform(depthUniform->m_handle.idx, depth, 16);
				uniformsBegin(program); uniformsEnd(program);
				RenderDraw draw; draw.clear(); draw.m_streamMask = 1;
				draw.m_stream[0].m_handle = _quad.m_vb; draw.m_stream[0].m_layoutHandle = _quad.m_layout; vertexInput(program, draw);
				const uint32_t stencil = BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_OP_FAIL_S_REPLACE | BGFX_STENCIL_OP_FAIL_Z_REPLACE | BGFX_STENCIL_OP_PASS_Z_REPLACE | BGFX_STENCIL_FUNC_REF(clear.m_stencil);
				const DepthControl noBias = {}; depthControl(noBias);
				state((colorMask ? BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A : 0)
					| (clear.m_flags & BGFX_CLEAR_DEPTH ? BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_ALWAYS : 0),
					clear.m_flags & BGFX_CLEAR_STENCIL ? stencil : 0, 0);
				channelMasks(15, colorMask);
				GX2SetAAMask(0xff, 0xff, 0xff, 0xff); m_sampleMask = UINT32_MAX;
				GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1); viewport(_view); m_cachedState = UINT64_MAX;
			}
			m_colorWritten |= colorMask; m_depthWritten |= m_hasDepth && depthFlags;
		}

		void submit(Frame* _render, const ClearQuad& _clearQuad, const MipGen& _mipGen, TextVideoMemBlitter& _blitter) override
		{
			m_mipGen = &_mipGen;
			Stats& stats = _render->m_perfStats;
			stats.cpuTimeBegin = bx::getHPCounter(); stats.cpuTimerFreq = bx::getHPFrequency();
			stats.numViews = 0;
			stats.gpuMemoryMax = -INT64_MAX; stats.gpuMemoryUsed = -INT64_MAX;
			bx::memSet(stats.numPrims, 0, sizeof(stats.numPrims));
			collect();
			resolveOcclusion(_render); resolveGpuTimers(_render);
			m_target = {};
			if (!m_platform.query(m_platform.userData, &m_target) || !m_target.color || !m_target.context)
			{ m_target = {}; stats.cpuTimeEnd = bx::getHPCounter(); return; }
			if (m_target.color->surface.aa != GX2_AA_MODE1X) unsupported("multisampled presentation targets");
			const uint32_t frameTimer = beginGpuTimer(BGFX_CONFIG_MAX_VIEWS, _render->m_frameNum);
			uint32_t viewTimer = UINT32_MAX;
			uploadTextures();
			restore();
			GX2SetSwapInterval((_render->m_reset & BGFX_RESET_VSYNC) ? 1 : 0);
			depthBuffer(m_target.color->surface.width, m_target.color->surface.height);
			bindFrameBuffer(BGFX_INVALID_HANDLE);
			if (_render->m_iboffset) update(m_indexBuffers[_render->m_transientIb->handle.idx], 0, _render->m_iboffset, _render->m_transientIb->data, false, true);
			if (_render->m_vboffset) update(m_vertexBuffers[_render->m_transientVb->handle.idx], 0, _render->m_vboffset, _render->m_transientVb->data, true, true);
			_render->sort();
			UniformCacheState ucs(_render);
			ViewState viewState(_render);
			uint16_t view = UINT16_MAX;
			beginBatch();
			for (uint32_t item = 0; item < _render->m_numRenderItems; ++item)
			{
				SortKey key;
				if (key.decode(_render->m_sortKeys[item], _render->m_viewRemap)) { unsupported("compute dispatch"); continue; }
				const View& renderView = _render->view(key.m_view);
				if (view != key.m_view)
				{
					endViewTimer(_render, viewTimer); viewTimer = UINT32_MAX;
					view = key.m_view;
					if (m_currentFrameBuffer.idx != renderView.m_fbh.idx)
					{
						endBatch(); finishRenderPass();
						restore(); bindFrameBuffer(renderView.m_fbh); beginBatch();
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
				const RenderDraw& draw = _render->m_renderItem[_render->m_sortValues[item]].draw;
				if (!isValid(key.m_program)) continue; // touch still executes view clear
				if (draw.m_uniformBegin < draw.m_uniformEnd)
					rendererUpdateUniforms(this, _render->m_uniformBuffer[draw.m_uniformIdx], draw.m_uniformBegin, draw.m_uniformEnd);
				const bool hasOcclusionQuery = 0 != (draw.m_stateFlags & BGFX_STATE_INTERNAL_OCCLUSION_QUERY);
				if (isValid(draw.m_occlusionQuery) && !hasOcclusionQuery
				&& !isVisible(_render, draw.m_occlusionQuery, 0 != (draw.m_submitFlags & BGFX_SUBMIT_INTERNAL_OCCLUSION_VISIBLE))) continue;
				if (isValid(draw.m_indirectBuffer)) { unsupported("indirect drawing"); continue; }
				if (_render->m_debug & BGFX_DEBUG_IFH) continue;
				Rect scissor = renderView.m_clippedRect;
				if (!renderView.m_scissor.isZero()) scissor.intersect(renderView.m_scissor);
				if (draw.m_scissor != UINT16_MAX) scissor.intersect(_render->m_frameCache.m_rectCache.m_cache[draw.m_scissor]);
				if (scissor.isZero()) continue;
				if (prepareTextures(_render->m_renderBind[draw.m_bindIdx])) viewport(renderView);
				GX2SetScissor(scissor.m_x, scissor.m_y, scissor.m_width, scissor.m_height);
				Program& program = m_programs[key.m_program.idx];
				uniformsBegin(program);
				viewState.m_alphaRef = float((draw.m_stateFlags & BGFX_STATE_ALPHA_REF_MASK) >> BGFX_STATE_ALPHA_REF_SHIFT)/255;
				viewState.setPredefined<4>(this, view, renderView, program, _render, draw);
				uniformsEnd(program);
				const DepthControl& depth = draw.m_depthBias == UINT16_MAX ? renderView.m_depthBias : _render->m_frameCache.m_depthBiasCache.m_cache[draw.m_depthBias];
				depthControl(depth);
				if (draw.m_sampleMask != m_sampleMask)
				{
					const uint8_t mask = uint8_t(draw.m_sampleMask); GX2SetAAMask(mask, mask, mask, mask); m_sampleMask = draw.m_sampleMask;
				}
				state(draw.m_stateFlags, draw.m_stencil, draw.m_rgba);
				if (draw.m_stateFlags & (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A)) m_colorWritten |= (1u << m_numColors)-1;
				m_depthWritten |= m_hasDepth && ((!m_depthReadOnly && (draw.m_stateFlags & BGFX_STATE_WRITE_Z)) || (!m_stencilReadOnly && draw.m_stencil));
				textures(_render->m_renderBind[draw.m_bindIdx], _render->m_colorPalette);

				vertexInput(program, draw);
				const GX2PrimitiveMode primitive = s_primitive[(draw.m_stateFlags & BGFX_STATE_PT_MASK) >> BGFX_STATE_PT_SHIFT];
				Block* occlusion = hasOcclusionQuery ? beginOcclusion(draw.m_occlusionQuery) : NULL;
				if (isValid(draw.m_indexBuffer))
				{
					Buffer& buffer = m_indexBuffers[draw.m_indexBuffer.idx];
					const uint32_t indexSize = buffer.flags & BGFX_BUFFER_INDEX32 ? 4 : 2;
					const uint32_t count = draw.m_numIndices == UINT32_MAX ? buffer.size/indexSize-draw.m_startIndex : draw.m_numIndices;
					GX2DrawIndexedEx(primitive, count, indexSize == 4 ? GX2_INDEX_TYPE_U32 : GX2_INDEX_TYPE_U16,
						(uint8_t*)buffer.block->data+draw.m_startIndex*indexSize, 0, draw.m_numInstances);
					touch(buffer.block);
				}
				else
				{
					uint32_t count = draw.m_numVertices;
					if (count == UINT32_MAX)
						for (uint32_t ss = 0; ss < BGFX_CONFIG_MAX_VERTEX_STREAMS; ++ss)
							if (draw.m_streamMask & (1<<ss))
							{
								const Stream& stream = draw.m_stream[ss];
								const Buffer& buffer = m_vertexBuffers[stream.m_handle.idx];
								const VertexLayout& layout = m_layouts[isValid(stream.m_layoutHandle) ? stream.m_layoutHandle.idx : buffer.layout.idx];
								count = bx::min(count, buffer.size/layout.m_stride-stream.m_startVertex);
							}
					GX2DrawEx(primitive, count, 0, draw.m_numInstances);
				}
				if (occlusion) endOcclusion(occlusion);
			}
			endViewTimer(_render, viewTimer);
			endBatch(); finishRenderPass();
			if (_render->m_debug & BGFX_DEBUG_TEXT) dbgTextSubmit(this, _blitter, _render->m_textVideoMem, BGFX_INVALID_HANDLE, 1);
			endGpuTimer(frameTimer);
			stats.cpuTimeEnd = bx::getHPCounter();
		}

		void dbgTextRenderBegin(TextVideoMemBlitter&, FrameBufferHandle _handle) override
		{
			restore(); bindFrameBuffer(_handle); beginBatch();
			GX2SetViewport(0, 0, m_targetWidth, m_targetHeight, -1, 1);
			GX2SetScissor(0, 0, m_targetWidth, m_targetHeight);
			state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0, 0);
		}

		void dbgTextRender(TextVideoMemBlitter& _blitter, uint32_t _numIndices) override
		{
			if (!_numIndices) return;
			const uint32_t numVertices = _numIndices*4/6;
			const uint32_t stride = _blitter.m_layout.m_stride;
			// The shared blitter writes packed ABGR integers. Convert its two colors
			// into the RGBA component bytes promised by the ordinary Uint8 layout.
			for (uint32_t ii = 0; ii < numVertices; ++ii)
				for (Attrib::Enum attr : { Attrib::Color0, Attrib::Color1 })
				{
					uint32_t* color = (uint32_t*)(_blitter.m_vb->data+ii*stride+_blitter.m_layout.m_offset[attr]);
					*color = bx::toLittleEndian(*color);
				}
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
		GpuTimerResult m_gpuTimerResults[BGFX_CONFIG_MAX_VIEWS + 1];
		uint32_t m_maxGpuLatency = 0;

		Gx2PlatformData m_platform;
		Gx2Target m_target = {};
		Buffer m_indexBuffers[BGFX_CONFIG_MAX_INDEX_BUFFERS];
		Buffer m_vertexBuffers[BGFX_CONFIG_MAX_VERTEX_BUFFERS];
		VertexLayout m_layouts[BGFX_CONFIG_MAX_VERTEX_LAYOUTS];
		Shader m_shaders[BGFX_CONFIG_MAX_SHADERS];
		Program m_programs[BGFX_CONFIG_MAX_PROGRAMS];
		const MipGen* m_mipGen = NULL;
		Texture m_textures[BGFX_CONFIG_MAX_TEXTURES];
		FrameBuffer m_frameBuffers[BGFX_CONFIG_MAX_FRAME_BUFFERS];
		FrameBufferHandle m_currentFrameBuffer = BGFX_INVALID_HANDLE;
		GX2ColorBuffer m_colors[8] = {};
		GX2DepthBuffer m_activeDepth = {};
		uint32_t m_targetWidth = 0, m_targetHeight = 0;
		uint32_t m_numColors = 0, m_colorWritten = 0;
		bool m_hasDepth = false, m_depthReadOnly = false, m_stencilReadOnly = false, m_depthWritten = false;
		std::vector<Block*> m_allocations, m_touched, m_retired, m_lists, m_uploads;
		std::vector<Texture*> m_pendingTextures;
		GX2DepthBuffer m_depth = {};
		Block* m_depthImage = NULL;
		Block* m_list = NULL;
		Block* m_upload = NULL;
		uint32_t m_uploadUsed = 0;
		uint32_t m_batchDraws = 0;
		uint8_t* m_stageData[2];
		DepthControl m_depthControl = {};
		uint32_t m_sampleMask = UINT32_MAX;
		uint64_t m_cachedState, m_cachedStencil;
		uint32_t m_cachedRgba;
		Program* m_boundProgram = NULL;
		Block* m_boundFetch = NULL;
		const void* m_boundAttributes[BGFX_CONFIG_MAX_VERTEX_STREAMS];
		uint32_t m_boundStrides[BGFX_CONFIG_MAX_VERTEX_STREAMS];
		const GX2Texture* m_boundTextures[16];
		Binding m_boundBindings[16];
		float m_borderColors[16][4];
		uint32_t m_borderValid = 0;
	};

	static RendererContextGX2* s_renderGX2;
	RendererContextI* rendererCreate(const Init& _init)
	{
		if (NULL == _init.swapChain.nwh) return NULL;
		s_renderGX2 = BX_NEW(g_allocator, RendererContextGX2)(_init);
		return s_renderGX2;
	}
	void rendererDestroy()
	{
		bx::deleteObject(g_allocator, s_renderGX2); s_renderGX2 = NULL;
	}
} }
#else
namespace bgfx { namespace gx2
{
	RendererContextI* rendererCreate(const Init&) { return NULL; }
	void rendererDestroy() {}
} }
#endif // BGFX_CONFIG_RENDERER_GX2
