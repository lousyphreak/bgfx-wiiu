// Copyright 2026. License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
#version 430
#extension GL_ARB_shader_stencil_export : require
layout(binding=0) uniform sampler2DMS s_depthCopy;
layout(binding=1) uniform usampler2DMS s_stencilCopy;
uniform vec4 u_mipGen;
void main()
{
	gl_FragDepth = texelFetch(s_depthCopy, ivec2(gl_FragCoord.xy) + ivec2(u_mipGen.xy), gl_SampleID).r;
	gl_FragStencilRefARB = u_mipGen.w == 0.0 ? 0 : int(texelFetch(s_stencilCopy, ivec2(gl_FragCoord.xy) + ivec2(u_mipGen.xy), gl_SampleID).r);
}
