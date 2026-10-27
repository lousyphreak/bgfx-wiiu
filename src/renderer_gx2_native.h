/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */
#ifndef BGFX_RENDERER_GX2_NATIVE_H_HEADER_GUARD
#define BGFX_RENDERER_GX2_NATIVE_H_HEADER_GUARD

#include <coreinit/memfrmheap.h>
#include <gx2/display.h>

namespace bgfx { namespace gx2
{
	// One GX2 session, owned exclusively by the renderer and its submission core.
	struct NativeGraphics
	{
		static constexpr uint32_t kHeapTag = 0x42474658;
		void* fifo = NULL;
		GX2ContextState* context = NULL;
		MEMHeapHandle mem1 = NULL, foreground = NULL;
		uint32_t tvWidth = 0, tvHeight = 0;
		void* tvBuffer = NULL;

		void start()
		{
			fifo = MEMAllocFromDefaultHeapEx(4*1024*1024, GX2_COMMAND_BUFFER_ALIGNMENT);
			uint32_t attributes[] = { GX2_INIT_CMD_BUF_BASE, uint32_t(uintptr_t(fifo)), GX2_INIT_CMD_BUF_POOL_SIZE, 4*1024*1024, GX2_INIT_END };
			GX2Init(attributes);
			GX2SetGPUTimeout(10000);
			context = (GX2ContextState*)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
			GX2SetupContextStateEx(context, TRUE);
		}

		static MEMHeapHandle acquireHeap(MEMBaseHeapType _type)
		{
			MEMHeapHandle base = MEMGetBaseHeapHandle(_type);
			MEMRecordStateForFrmHeap(base, kHeapTag);
			const uint32_t size = MEMGetAllocatableSizeForFrmHeapEx(base, 4);
			MEMHeapHandle heap = MEMCreateExpHeapEx(MEMAllocFromFrmHeapEx(base, size, 4), size, 0);
			return heap;
		}

		void configureTV(uint32_t _height)
		{
			const GX2TVScanMode system = GX2GetSystemTVScanMode();
			const bool hd = system == GX2_TV_SCAN_MODE_1080I || system == GX2_TV_SCAN_MODE_1080P;
			const bool wide = hd || system == GX2_TV_SCAN_MODE_720P;
			const GX2TVRenderMode mode = hd && _height > 720 ? GX2_TV_RENDER_MODE_WIDE_1080P
				: wide && _height > 480 ? GX2_TV_RENDER_MODE_WIDE_720P : GX2_TV_RENDER_MODE_WIDE_480P;
			const uint32_t height = mode == GX2_TV_RENDER_MODE_WIDE_1080P ? 1080 : mode == GX2_TV_RENDER_MODE_WIDE_720P ? 720 : 480;
			if (height == tvHeight) return;
			if (tvBuffer) { GX2SetTVEnable(FALSE); GX2DrawDone(); MEMFreeToExpHeap(foreground, tvBuffer); }
			tvWidth = mode == GX2_TV_RENDER_MODE_WIDE_1080P ? 1920 : mode == GX2_TV_RENDER_MODE_WIDE_720P ? 1280 : 854;
			tvHeight = height;
			uint32_t size, scale;
			GX2CalcTVSize(mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &size, &scale);
			tvBuffer = MEMAllocFromExpHeapEx(foreground, size, GX2_SCAN_BUFFER_ALIGNMENT);
			DCZeroRange(tvBuffer, size); GX2Invalidate(GX2_INVALIDATE_MODE_CPU, tvBuffer, size);
			GX2SetTVBuffer(tvBuffer, size, mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
			GX2SetTVScale(tvWidth, tvHeight);
		}

		void acquire(uint32_t _height)
		{
			mem1 = acquireHeap(MEM_BASE_HEAP_MEM1);
			foreground = acquireHeap(MEM_BASE_HEAP_FG);
			configureTV(_height);
			uint32_t size, scale;
			GX2CalcDRCSize(GX2_DRC_RENDER_MODE_SINGLE, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &size, &scale);
			void* drc = MEMAllocFromExpHeapEx(foreground, size, GX2_SCAN_BUFFER_ALIGNMENT);
			DCZeroRange(drc, size); GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drc, size);
			GX2SetDRCBuffer(drc, size, GX2_DRC_RENDER_MODE_SINGLE, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
			GX2SetDRCScale(854, 480);
			GX2SetContextState(context);
		}

		void drain()
		{
			GX2SetTVEnable(FALSE); GX2SetDRCEnable(FALSE);
			GX2DrawDone();
		}

		void release()
		{
			tvBuffer = NULL; tvWidth = tvHeight = 0;
			for (MEMBaseHeapType type : { MEM_BASE_HEAP_MEM1, MEM_BASE_HEAP_FG })
			{
				MEMHeapHandle& heap = type == MEM_BASE_HEAP_MEM1 ? mem1 : foreground;
				MEMDestroyExpHeap(heap); heap = NULL;
				MEMFreeByStateToFrmHeap(MEMGetBaseHeapHandle(type), kHeapTag);
			}
		}

		void shutdown()
		{
			GX2Shutdown();
			MEMFreeToDefaultHeap(context); MEMFreeToDefaultHeap(fifo);
		}
	};
} }
#endif
