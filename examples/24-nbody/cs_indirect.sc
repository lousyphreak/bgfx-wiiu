/*
 * Copyright 2014 Stanlo Slasinski. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include "bgfx_compute.sh"
#include "uniforms.sh"

BUFFER_WO(indirectBuffer, uvec4, 0);

NUM_THREADS(1, 1, 1)
void main()
{
	drawIndexedIndirect(indirectBuffer, 0, 6, u_dispatchSize * threadGroupUpdateSize, 0, 0, 0);
#if BGFX_SHADER_LANGUAGE_GX2
	dispatchIndirect(indirectBuffer, 1, u_dispatchSize * (threadGroupUpdateSize / threadGroupLdsSize), 1, 1);
#else
	dispatchIndirect(indirectBuffer, 1, u_dispatchSize, 1, 1);
#endif // BGFX_SHADER_LANGUAGE_GX2
}
