#version 450

// Skinned meshes: each vertex blends up to four joint matrices. The palette holds
// joint world matrices times their inverse bind matrices, so `model` is identity.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in vec2 in_uv1;
layout(location = 4) in vec4 in_color;
layout(location = 5) in uvec4 in_joints;
layout(location = 6) in vec4 in_weights;

layout(set = 1, binding = 0) uniform DrawUniforms {
    mat4 view_proj;
    mat4 model;
};

layout(set = 1, binding = 1) uniform JointUniforms {
    mat4 joints[128];
};

layout(location = 0) out vec2 v_uv0;
layout(location = 1) out vec2 v_uv1;
layout(location = 2) out vec3 v_normal;
layout(location = 3) out vec4 v_color;
layout(location = 4) out vec3 v_world;

void main() {
    mat4 skin = joints[in_joints.x] * in_weights.x + joints[in_joints.y] * in_weights.y +
                joints[in_joints.z] * in_weights.z + joints[in_joints.w] * in_weights.w;
    vec4 world = model * skin * vec4(in_position, 1.0);
    gl_Position = view_proj * world;
    v_world = world.xyz;
    v_normal = mat3(model) * mat3(skin) * in_normal;
    v_uv0 = in_uv0;
    v_uv1 = in_uv1;
    v_color = in_color;
}
