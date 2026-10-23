// Copyright 2011-2026 Branimir Karadzic. All rights reserved.
// License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE

#version 430
layout(binding=0) uniform isampler3D s_texMipSrc;
uniform vec4 u_mipGen; // z is the source texel slice.
layout(location=0) out ivec4 o_color;
void main()
{
	o_color = texelFetch(s_texMipSrc, ivec3(ivec2(gl_FragCoord.xy), int(u_mipGen.z)), 0);
}
