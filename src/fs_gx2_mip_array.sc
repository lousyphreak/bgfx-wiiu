/* License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE */
#include "bgfx_shader.sh"
SAMPLER2DARRAY(s_texMipSrc, 0);
uniform vec4 u_mipGen;
void main()
{
	gl_FragColor = textureLod(s_texMipSrc, vec3(gl_FragCoord.xy * u_mipGen.xy, 0.0), 0.0);
}
