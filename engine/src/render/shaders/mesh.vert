#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in vec2 in_uv1;
layout(location = 4) in vec4 in_color;

layout(set = 1, binding = 0) uniform DrawUniforms {
    mat4 view_proj;
    mat4 model;
};

layout(location = 0) out vec2 v_uv0;
layout(location = 1) out vec2 v_uv1;
layout(location = 2) out vec3 v_normal;
layout(location = 3) out vec4 v_color;
layout(location = 4) out vec3 v_world;

void main() {
    vec4 world = model * vec4(in_position, 1.0);
    gl_Position = view_proj * world;
    v_world = world.xyz;
    v_normal = mat3(model) * in_normal;
    v_uv0 = in_uv0;
    v_uv1 = in_uv1;
    v_color = in_color;
}
