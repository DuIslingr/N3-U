#version 450

// Particle quads, built on the CPU in world space.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(set = 1, binding = 0) uniform ParticleUniforms {
    mat4 view_proj;
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    gl_Position = view_proj * vec4(in_position, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
