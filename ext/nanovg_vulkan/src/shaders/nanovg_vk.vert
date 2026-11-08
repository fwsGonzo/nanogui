#version 450
// Port of nanovg_gl.h's GL3 fill vertex shader. Clip space is the GL one: the
// Vulkan backend does not flip Y (NDC y=-1 is memory row 0, as in GL).
layout(push_constant) uniform Push {
	vec2 viewSize;
} pc;

layout(location = 0) in vec2 vertex;
layout(location = 1) in vec4 tcoord;

layout(location = 0) out vec2 ftcoord;
layout(location = 1) out vec2 fpos;
layout(location = 2) smooth out vec2 uv;

void main(void) {
	ftcoord = tcoord.xy;
	uv = 0.5 * tcoord.zw;
	fpos = vertex;
	gl_Position = vec4(2.0*vertex.x/pc.viewSize.x - 1.0, 1.0 - 2.0*vertex.y/pc.viewSize.y, 0.0, 1.0);
}
