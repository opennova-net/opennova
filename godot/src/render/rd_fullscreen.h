#pragma once

namespace godot {

// The one full-screen triangle every RenderingDevice copy/composite leg
// draws (three vertices, no vertex buffer): FrameFX's passes and the
// environment cube's layer blits.
inline const char *const kRdFullscreenVertexShader = R"GLSL(#version 450
void main() {
	const vec2 positions[3] = vec2[3](
			vec2(-1.0, -1.0),
			vec2(3.0, -1.0),
			vec2(-1.0, 3.0));
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
)GLSL";

} // namespace godot
