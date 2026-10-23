#version 450

// Full-screen triangle for post effects (no vertex buffer).
layout(location = 0) out vec2 v_uv;   // 0,0 = top left of the source image

void main() {
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
