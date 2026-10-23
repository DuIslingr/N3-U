#version 450

// Coloured primitives: world-space effects (tracers, bolts) and the screen-space HUD.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(set = 1, binding = 0) uniform ColorUniforms {
    mat4 view_proj;
};

layout(location = 0) out vec4 v_color;

void main() {
    gl_Position = view_proj * vec4(in_position, 1.0);
    v_color = in_color;
}
