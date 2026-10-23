/* GPU-generated indirect command stream. Arguments remain GPU-resident. */
#include "bgfx_compute.sh"
BUFFER_RO(indirectArgs, uvec4, 0);
BUFFER_RO(indirectCount, uint, 1);
BUFFER_WO(nativeCommands, uvec4, 2);
uniform vec4 u_indirectLower[3];
NUM_THREADS(64, 1, 1)

// MEM_EXPORT stores LE words. Native GX2 command memory and dispatch params
// are BE, so make the byte representation match GX2's own command writer.
uvec4 nativeBytes(uvec4 value)
{
	return ((value & uvec4(0xffu)) << 24u)
		 | ((value & uvec4(0xff00u)) << 8u)
		 | ((value >> 8u) & uvec4(0xff00u))
		 | (value >> 24u);
}

void main()
{
	uvec4 range = floatBitsToUint(u_indirectLower[0]);
	uvec4 draw = floatBitsToUint(u_indirectLower[1]);
	uint record = gl_GlobalInvocationID.x;
#ifdef INDIRECT_DISPATCH
	if (record != 0u) return;
	uvec4 options = floatBitsToUint(u_indirectLower[2]);
	uvec4 groups = indirectArgs[range.x * 2u];
	groups.x *= options.y; // same physical-wave factor as the direct renderer
	uint workItems = groups.x * groups.y * groups.z * 64u;
	nativeCommands[0] = nativeBytes(uvec4(groups.xyz, 0u));
	// Prefix is 64 bytes; the target RID147 descriptor points at a retained
	// copy of nativeCommands[0], not at the original logical argument record.
	nativeCommands[4] = nativeBytes(uvec4(0xc0076d00u, 0x865u, options.x, 15u));
	nativeCommands[5] = nativeBytes(uvec4(0x86201000u, 1u, 0xabcd1234u, 0xabcd1234u));
	nativeCommands[6] = nativeBytes(uvec4(0xc0000000u, 0xc0004600u, 0x0eu, 0xc0016800u));
	nativeCommands[7] = nativeBytes(uvec4(0x256u, 1u, 0xc0002f00u, 1u));
	nativeCommands[8] = nativeBytes(uvec4(0xc0012d01u, workItems, 2u, 0x80000000u));
#else
	if (record >= range.y) return;
	uvec4 arg = indirectArgs[(range.x + record) * 2u];
	uint count = arg.y != 0u ? arg.x : 0u;
	if (range.z != 0u && record + floatBitsToUint(u_indirectLower[2].z) >= indirectCount[range.w]) count = 0u;
	uint instances = max(arg.y, 1u);
	uint majorMode = (draw.z & 0x80u) != 0u ? 4u : 0u;
#ifdef INDIRECT_INDEXED
	uint startInstance = indirectArgs[(range.x + record) * 2u + 1u].x;
	uint indexAddress = draw.x + arg.z * draw.y;
	uint dest = record * 6u;
	nativeCommands[dest + 0u] = nativeBytes(uvec4(0xc0016f00u, 0u, arg.w, 0xc0016f00u));
	nativeCommands[dest + 1u] = nativeBytes(uvec4(1u, startInstance, 0xc0016800u, 0x256u));
	nativeCommands[dest + 2u] = nativeBytes(uvec4(draw.z & 31u, 0xc0002a00u, draw.w, 0xc0002f00u));
	nativeCommands[dest + 3u] = nativeBytes(uvec4(instances, 0xc0042701u, 0xffffffffu, indexAddress));
	nativeCommands[dest + 4u] = nativeBytes(uvec4(0u, count, majorMode, 0xc0016f00u));
	nativeCommands[dest + 5u] = nativeBytes(uvec4(1u, 0u, 0x80000000u, 0x80000000u));
#else
	uint dest = record * 5u;
	nativeCommands[dest + 0u] = nativeBytes(uvec4(0xc0016f00u, 0u, arg.z, 0xc0016f00u));
	nativeCommands[dest + 1u] = nativeBytes(uvec4(1u, arg.w, 0xc0016800u, 0x256u));
	nativeCommands[dest + 2u] = nativeBytes(uvec4(draw.z & 31u, 0xc0002f00u, instances, 0xc0012d01u));
	nativeCommands[dest + 3u] = nativeBytes(uvec4(count, 2u | majorMode, 0xc0016f00u, 1u));
	nativeCommands[dest + 4u] = nativeBytes(uvec4(0u, 0x80000000u, 0x80000000u, 0x80000000u));
#endif
#endif
}
