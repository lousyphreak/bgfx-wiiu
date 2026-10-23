// Copyright 2011-2026 Branimir Karadzic. All rights reserved.
// License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE

#version 430
uniform vec4 bgfx_clear_color;
uniform vec4 bgfx_clear_depth;
layout(location=0) out uvec4 o_color;
void main()
{
	o_color = uvec4(bgfx_clear_color);
	gl_FragDepth = bgfx_clear_depth.x;
}
