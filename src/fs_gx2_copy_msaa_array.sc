// Copyright 2026. License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
#version 430
layout(binding=0) uniform sampler2DMSArray s_texMipSrc;
uniform vec4 u_mipGen;
layout(location=0) out vec4 o_color;
void main()
{
	ivec3 pixel = ivec3(ivec2(gl_FragCoord.xy) + ivec2(u_mipGen.xy), 0);
	o_color = texelFetch(s_texMipSrc, pixel, gl_SampleID);
}
