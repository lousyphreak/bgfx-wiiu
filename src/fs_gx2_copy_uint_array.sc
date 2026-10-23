// Copyright 2011-2026 Branimir Karadzic. All rights reserved.
// License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE

#version 430
layout(binding=0) uniform usampler2DArray s_texMipSrc;
layout(location=0) out uvec4 o_color;
void main()
{
	o_color = texelFetch(s_texMipSrc, ivec3(ivec2(gl_FragCoord.xy), 0), 0);
}
