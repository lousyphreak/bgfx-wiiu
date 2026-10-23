// Copyright 2011-2026 Branimir Karadzic. All rights reserved.
// License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE

#version 430
#extension GL_ARB_shader_stencil_export : require
layout(binding=0) uniform sampler3D s_depthCopy;
layout(binding=1) uniform usampler3D s_stencilCopy;
uniform vec4 u_mipGen;
void main()
{
	gl_FragDepth = texelFetch(s_depthCopy, ivec3(ivec2(gl_FragCoord.xy), int(u_mipGen.z)), 0).r;
	gl_FragStencilRefARB = u_mipGen.w == 0.0 ? 0 : int(texelFetch(s_stencilCopy, ivec3(ivec2(gl_FragCoord.xy), int(u_mipGen.z)), 0).r);
}
