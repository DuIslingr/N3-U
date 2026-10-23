#version 450

// NV_DiffuseVC as the effects use it: texture times vertex colour.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D diffuse;

void main() {
    out_color = texture(diffuse, v_uv) * v_color;
}
