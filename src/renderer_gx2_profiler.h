/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

// 0: absent; 1: CPU cycles/instructions/L2 misses/castouts + GPU counters;
// 2: CPU unresolved stalls/instructions/L1 load-miss cycles/branch misses;
// 3: low-overhead whole-cycle CPU/GPU and coarse renderer timing, without per-draw probes.
// Detailed scopes are inclusive. Compare final changes with mode 3 as well.
#ifndef BGFX_CONFIG_GX2_PROFILER
# define BGFX_CONFIG_GX2_PROFILER 0
#endif

#if BGFX_CONFIG_GX2_PROFILER
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/performancemonitor.h>
#include <coreinit/thread.h>
#include <gx2/perf.h>

extern "C" OSTime OSGetThreadCoreTime(const OSThread*, uint16_t);

namespace bgfx { namespace gx2
{
	struct Gx2Profiler
	{
		enum Phase
		{
			ListAcquire, UniformAcquire, UniformFill, UniformZero, UniformInitial,
			UniformApplication, UniformPredefined, UniformEnd, UniformPublish, UniformBind,
			UniformInvalidate, ListFinish, ListCall, VertexUpload, IndexUpload,
			BufferCopy, BufferPublish, BufferPreserve, StageReuse, TextureStage,
			TextureUpload, TexturePublish, Textures, VertexInput, PipelineState,
			Sort, ResolveTimers, Collect, Present, Scissor, FrameBufferBuild, OcclusionResolve,
			// Coarse phases/waits also measure actual thread CPU, excluding sleep.
			Submit, EndFrame, Flip, ListWait, UniformWait, BufferReadWait,
			ReadbackWait, FlipWait, PresentGpuWait, PhaseCount
		};
		enum Bytes
		{
			ListAllocated, UniformAllocated, ListBytes, UniformBytes,
			VertexBytes, IndexBytes, BufferRenames, PreserveBytes,
			StageRenames, StageAllocated, TextureSets, ContextRestores, ScissorSets, FetchCreates, PipelineSets, TextureViewBuilds, RasterBlendSets, SamplerBuilds, OcclusionResults, VisibleDraws, UniformBinds, UniformReuses, ColorDisabledSets, BytesCount
		};

#if BGFX_CONFIG_GX2_PROFILER < 3
		struct Sum
		{
			uint64_t ticks = 0, thread = 0, events[4] = {};
			uint32_t calls = 0;
		};
		struct Scope
		{
			Sum& sum;
			Phase phase;
			uint64_t ticks = OSGetSystemTime(), thread;
			uint32_t events[4] = { OSGetUPMC1(), OSGetUPMC2(), OSGetUPMC3(), OSGetUPMC4() };
			Scope(Gx2Profiler& _profiler, Phase _phase)
				: sum(_profiler.phases[_phase]), phase(_phase)
				, thread(_phase >= Submit ? OSGetThreadCoreTime(OSGetCurrentThread(), 7) : 0) {}
			~Scope()
			{
				sum.ticks += OSGetSystemTime()-ticks;
				const uint32_t end[] = { OSGetUPMC1(), OSGetUPMC2(), OSGetUPMC3(), OSGetUPMC4() };
				for (uint32_t ii = 0; ii < 4; ++ii) sum.events[ii] += uint32_t(end[ii]-events[ii]);
				if (phase >= Submit) sum.thread += OSGetThreadCoreTime(OSGetCurrentThread(), 7)-thread;
				++sum.calls;
			}
		};

#endif

#if BGFX_CONFIG_GX2_PROFILER == 3
		struct ThreadScope
		{
			uint64_t& sum;
			uint64_t start = OSGetThreadCoreTime(OSGetCurrentThread(), 7);
			ThreadScope(uint64_t& _sum) : sum(_sum) {}
			~ThreadScope() { sum += OSGetThreadCoreTime(OSGetCurrentThread(), 7)-start; }
		};
#endif

		void init()
		{
			bx::memSet(this, 0, sizeof(*this));
#if BGFX_CONFIG_GX2_PROFILER < 3
			active = -1;
			asm volatile("mfspr %0,936" : "=r"(oldControl[0]));
			asm volatile("mfspr %0,940" : "=r"(oldControl[1]));
			oldEvents[0] = OSGetUPMC1(); oldEvents[1] = OSGetUPMC2();
			oldEvents[2] = OSGetUPMC3(); oldEvents[3] = OSGetUPMC4();
			oldAffinity = OSGetThreadAffinity(OSGetCurrentThread());
			OSSetThreadAffinity(OSGetCurrentThread(), 1u << OSGetCoreId());
#if BGFX_CONFIG_GX2_PROFILER == 1
			OSSetPerformanceMonitor(63, OS_PM_MMCR0_PMC1_CPU_CYCLES | OS_PM_MMCR0_PMC2_INSTRUCTIONS_COMPLETED,
				OS_PM_MMCR1_PMC3_L2_DATA_MISSES | OS_PM_MMCR1_PMC4_L2_CASTOUTS, 0, 0, 0, 0);
#else
			OSSetPerformanceMonitor(63, OS_PM_MMCR0_PMC1_UNRESOLVED_STALL_CYCLES | OS_PM_MMCR0_PMC2_INSTRUCTIONS_COMPLETED,
				OS_PM_MMCR1_PMC3_L1_LOAD_MISS_CYCLES | OS_PM_MMCR1_PMC4_BRANCHES_MISPREDICTED, 0, 0, 0, 0);
#endif
			GX2ResetCounterInfo(&info);
			for (uint32_t ii = 0; ii < 8; ++ii)
				GX2InitCounterInfo(&info, counterIds[ii], statIds[ii]);
			Sum& calibration = phases[Collect];
			const uint64_t start = OSGetSystemTime();
			for (uint32_t ii = 0; ii < 10000; ++ii) { Scope probe(*this, Collect); }
			OSReport("GX2_PROFILE_PROBE count=10000 selfUs=%.3f totalUs=%.3f\n",
				double(calibration.ticks)/62.15625, double(OSGetSystemTime()-start)/62.15625);
			calibration = {};
#endif
			OSReport("GX2_PROFILE_INIT mode=%u cpuTimebase=62156250 gpuTimebase=27000000\n", BGFX_CONFIG_GX2_PROFILER);
		}

		void finish()
		{
#if BGFX_CONFIG_GX2_PROFILER < 3
			OSSetPerformanceMonitor(63, oldControl[0], oldControl[1], oldEvents[0], oldEvents[1], oldEvents[2], oldEvents[3]);
			OSSetThreadAffinity(OSGetCurrentThread(), oldAffinity);
#endif
		}

		void begin(uint32_t _frame, bool _draws, bool _vsync)
		{
			rendered[_frame%512] = _draws;
#if BGFX_CONFIG_GX2_PROFILER < 3
			for (GpuSample& sample : samples) if (sample.pending)
			{
				DCInvalidateRange(sample.data, sizeof(sample.data));
				uint64_t values[8]; bool ready = true;
				for (uint32_t ii = 0; ii < 8; ++ii)
				{
					values[ii] = GX2GetCounterResultEx(&info, sample.data, statIds[ii]);
					ready &= values[ii] != UINT64_MAX;
				}
				if (ready)
				{
					for (uint32_t ii = 0; ii < 8; ++ii) gpuEvents[ii] += values[ii];
					++gpuSamples; sample.pending = false;
				}
			}
#endif
#if BGFX_CONFIG_GX2_PROFILER < 3
			for (UploadSample& sample : uploads) if (sample.pending)
			{
				DCInvalidateRange(sample.begin, 64); DCInvalidateRange(sample.end, 64);
				if (sample.begin[0] != GX2_GPU_TIMESTAMP_INVALID && sample.end[0] != GX2_GPU_TIMESTAMP_INVALID)
				{
					uploadTicks += sample.end[0]-sample.begin[0]; ++uploadSamples; sample.pending = false;
				}
			}
#endif
			if (!_draws) return;
			const uint64_t now = OSGetSystemTime(), cpu = OSGetThreadCoreTime(OSGetCurrentThread(), 7);
			if (lastWall)
			{
				intervals[frames++] = double(now-lastWall)/62156.25;
				wall += now-lastWall; work += cpu-lastCpu;
				if (frames == 120) report(_frame, _vsync);
			}
			lastWall = now; lastCpu = cpu;
		}

#if BGFX_CONFIG_GX2_PROFILER < 3
		void startGpu(bool _draws)
		{
			if (_draws && (++drawFrames)%16 == 0)
				for (uint32_t ii = 0; ii < BX_COUNTOF(samples); ++ii) if (!samples[ii].pending)
				{
					active = int(ii); GX2SetCounterInfo(&info); GX2ResetCounters(&info); GX2StartCounters(&info);
					break;
				}
		}

		int beginUpload()
		{
			for (uint32_t ii = 0; ii < BX_COUNTOF(uploads); ++ii) if (!uploads[ii].pending)
			{
				GX2SampleBottomGPUCycle(uploads[ii].begin); return int(ii);
			}
			return -1; // Profiling never waits for a free sample.
		}
		void endUpload(int _index)
		{
			if (_index < 0) return;
			GX2SampleBottomGPUCycle(uploads[_index].end); uploads[_index].pending = true;
		}
#endif

		void end()
		{
#if BGFX_CONFIG_GX2_PROFILER < 3
			if (active >= 0)
			{
				GX2StopCounters(&info); GX2SampleCountersEx(&info, samples[active].data);
				samples[active].pending = true; active = -1;
			}
#endif
		}

		void gpu(uint32_t _frame, uint16_t _view, uint64_t _ticks)
		{
			if (!rendered[_frame%512]) return;
			if (_view == BGFX_CONFIG_MAX_VIEWS) { gpuTicks += _ticks; ++gpuFrames; }
#if BGFX_CONFIG_GX2_PROFILER < 3
			else { viewTicks[_view] += _ticks; ++viewFrames[_view]; }
#endif
		}

		void report(uint32_t _frame, bool _vsync)
		{
			std::sort(intervals, intervals+120);
			OSReport("GX2_PROFILE_FRAME frame=%u n=120 vsync=%u cpuMs=%.6f frameMs=%.6f frame95=%.6f gpuMs=%.6f gpuN=%u draws=%.3f\n",
				_frame, unsigned(_vsync), double(work)/62156.25/120, double(wall)/62156.25/120,
				intervals[113], gpuFrames ? double(gpuTicks)/27000/gpuFrames : 0, gpuFrames, double(draws)/120);
#if BGFX_CONFIG_GX2_PROFILER == 3
			OSReport("GX2_PROFILE_RENDER_CPU frame=%u submitMs=%.6f endFrameMs=%.6f flipMs=%.6f\n",
				_frame, double(coarse[0])/62156.25/120, double(coarse[1])/62156.25/120, double(coarse[2])/62156.25/120);
			coarse[0] = coarse[1] = coarse[2] = 0;
#endif
#if BGFX_CONFIG_GX2_PROFILER < 3
			static const char* names[] = {
				"ListAcquire", "UniformAcquire", "UniformFill", "UniformZero", "UniformInitial", "UniformApplication",
				"UniformPredefined", "UniformEnd", "UniformPublish", "UniformBind", "UniformInvalidate", "ListFinish", "ListCall",
				"VertexUpload", "IndexUpload", "BufferCopy", "BufferPublish", "BufferPreserve", "StageReuse",
				"TextureStage", "TextureUpload", "TexturePublish", "Textures", "VertexInput", "PipelineState",
				"Sort", "ResolveTimers", "Collect", "Present", "Scissor", "FrameBufferBuild", "OcclusionResolve", "Submit", "EndFrame", "Flip", "ListWait",
				"UniformWait", "BufferReadWait", "ReadbackWait", "FlipWait", "PresentGpuWait" };
			OSReport("GX2_PROFILE_EVENTS bank=%u names=%s\n", BGFX_CONFIG_GX2_PROFILER,
#if BGFX_CONFIG_GX2_PROFILER == 1
				"cycles,instructions,L2-data-misses,L2-castouts");
#else
				"unresolved-stall-cycles,instructions,L1-load-miss-cycles,branch-misses");
#endif
			for (uint32_t ii = 0; ii < PhaseCount; ++ii) if (phases[ii].calls)
			{
				const Sum& sum = phases[ii];
				OSReport("GX2_PROFILE_PHASE phase=%s n=%u inclusiveUs=%.3f threadUs=%.3f events=%llu,%llu,%llu,%llu\n",
					names[ii], sum.calls, double(sum.ticks)/62.15625, ii >= Submit ? double(sum.thread)/62.15625 : -1.0,
					(unsigned long long)sum.events[0], (unsigned long long)sum.events[1],
					(unsigned long long)sum.events[2], (unsigned long long)sum.events[3]);
			}
			static const char* byteNames[] = { "ListAllocated", "UniformAllocated", "ListBytes", "UniformBytes",
				"VertexBytes", "IndexBytes", "BufferRenames", "PreserveBytes", "StageRenames", "StageAllocated",
				"TextureSets", "ContextRestores", "ScissorSets", "FetchCreates", "PipelineSets", "TextureViewBuilds", "RasterBlendSets", "SamplerBuilds", "OcclusionResults", "VisibleDraws", "UniformBinds", "UniformReuses", "ColorDisabledSets" };
			for (uint32_t ii = 0; ii < BytesCount; ++ii) if (bytes[ii])
				OSReport("GX2_PROFILE_COUNT name=%s value=%llu\n", byteNames[ii], (unsigned long long)bytes[ii]);
			static const char* gpuNames[] = { "GRBM-total", "GRBM-active", "TCP-TCC-stall", "TCP-tag-stall",
				"TCP-pixels", "TCC-hits", "TCC-misses", "TA-aligner" };
			for (uint32_t ii = 0; ii < 8; ++ii) if (gpuSamples)
				OSReport("GX2_PROFILE_GPU event=%s n=%u mean=%.3f\n", gpuNames[ii], gpuSamples, double(gpuEvents[ii])/gpuSamples);
			for (uint32_t ii = 0; ii < BGFX_CONFIG_MAX_VIEWS; ++ii) if (viewFrames[ii])
				OSReport("GX2_PROFILE_VIEW view=%u n=%u meanUs=%.3f\n", ii, viewFrames[ii], double(viewTicks[ii])/27/viewFrames[ii]);
			if (uploadSamples) OSReport("GX2_PROFILE_UPLOAD_GPU n=%u meanUs=%.3f\n", uploadSamples, double(uploadTicks)/27/uploadSamples);
			uploadSamples = 0; uploadTicks = 0;
			for (Sum& sum : phases) sum = {};
			bx::memSet(bytes, 0, sizeof(bytes)); bx::memSet(gpuEvents, 0, sizeof(gpuEvents));
			bx::memSet(viewTicks, 0, sizeof(viewTicks)); bx::memSet(viewFrames, 0, sizeof(viewFrames));
			gpuSamples = 0;
#endif
			frames = gpuFrames = 0; wall = work = gpuTicks = draws = 0;
		}

		bool rendered[512] = {};
		uint64_t lastWall = 0, lastCpu = 0, wall = 0, work = 0, gpuTicks = 0, draws = 0;
		uint32_t frames = 0, gpuFrames = 0;
		double intervals[120];
#if BGFX_CONFIG_GX2_PROFILER == 3
		uint64_t coarse[3] = {};
#endif
#if BGFX_CONFIG_GX2_PROFILER < 3
		static constexpr GX2PerfCounter counterIds[] = { GX2_PERF_COUNTER_GRBM_0, GX2_PERF_COUNTER_GRBM_1,
			GX2_PERF_COUNTER_TCP_0_0, GX2_PERF_COUNTER_TCP_1_0, GX2_PERF_COUNTER_TCP_2_0,
			GX2_PERF_COUNTER_TCC_0_0, GX2_PERF_COUNTER_TCC_1_0, GX2_PERF_COUNTER_TA_0_0 };
		static constexpr GX2PerfEvent statIds[] = { GX2_PERF_EVENT_GRBM_COUNT, GX2_PERF_EVENT_GRBM_GUI_ACTIVE,
			GX2_PERF_EVENT_TCP_TCC_STALL_CYCLES, GX2_PERF_EVENT_TCP_TAG_CONFLICT_STALL_CYCLES,
			GX2_PERF_EVENT_TCP_TOTAL_PIXELS, GX2_PERF_EVENT_TCC_TEXTURE_REQUEST_HITS, GX2_PERF_EVENT_TCC_TEXTURE_REQUEST_MISSES,
			GX2_PERF_EVENT_TA_ALIGNER_CYCLES };
		struct GpuSample { alignas(64) uint64_t data[128]; bool pending = false; };
		alignas(64) GX2PerfCounterInfo info;
		GpuSample samples[8];
		struct UploadSample { alignas(64) uint64_t begin[8], end[8]; bool pending = false; };
		UploadSample uploads[32];
		uint64_t uploadTicks = 0;
		uint32_t uploadSamples = 0;
		int active = -1;
		uint32_t oldControl[2], oldEvents[4], oldAffinity, drawFrames = 0, gpuSamples = 0;
		Sum phases[PhaseCount];
		uint64_t bytes[BytesCount] = {}, gpuEvents[8] = {}, viewTicks[BGFX_CONFIG_MAX_VIEWS] = {};
		uint32_t viewFrames[BGFX_CONFIG_MAX_VIEWS] = {};
#endif
	};
	static Gx2Profiler s_gx2Profiler;
} }
#define GX2_PROFILE(_call) s_gx2Profiler._call
#else
#define GX2_PROFILE(_call) do {} while (0)
#endif

#if BGFX_CONFIG_GX2_PROFILER && BGFX_CONFIG_GX2_PROFILER < 3
#define GX2_PROFILE_SCOPE_SELECT(_phase) Gx2Profiler::Scope BX_CONCATENATE(gx2ProfileScope, __LINE__)(s_gx2Profiler, _phase)
#define GX2_PROFILE_SCOPE(_phase) GX2_PROFILE_SCOPE_SELECT(Gx2Profiler::_phase)
#define GX2_PROFILE_ADD(_name, _value) s_gx2Profiler.bytes[Gx2Profiler::_name] += (_value)
#else
#define GX2_PROFILE_SCOPE(_phase)
#define GX2_PROFILE_SCOPE_SELECT(_phase)
#define GX2_PROFILE_ADD(_name, _value) do {} while (0)
#endif

#if BGFX_CONFIG_GX2_PROFILER == 3
#define GX2_PROFILE_COARSE(_phase) Gx2Profiler::ThreadScope BX_CONCATENATE(gx2ProfileThread, __LINE__)(s_gx2Profiler.coarse[Gx2Profiler::_phase-Gx2Profiler::Submit])
#else
#define GX2_PROFILE_COARSE(_phase) GX2_PROFILE_SCOPE(_phase)
#endif
