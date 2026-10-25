// Copyright 2026. License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
#version 430
layout(binding=0) uniform sampler2DMS s_texMipSrc;
uniform vec4 u_mipGen;
layout(location=0) out vec4 o_color;
void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy) + ivec2(u_mipGen.xy);
	float weight = 1.0 / u_mipGen.z;
	vec4 color = texelFetch(s_texMipSrc, pixel, 0) * weight;
	for (int sampleIndex = 1; sampleIndex < int(u_mipGen.z); ++sampleIndex)
		color += texelFetch(s_texMipSrc, pixel, sampleIndex) * weight;
	o_color = color;
}
