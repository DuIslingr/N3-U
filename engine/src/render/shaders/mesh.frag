#version 450

layout(location = 0) in vec2 v_uv0;
layout(location = 1) in vec2 v_uv1;
layout(location = 2) in vec3 v_normal;
layout(location = 3) in vec4 v_color;
layout(location = 4) in vec3 v_world;

layout(set = 2, binding = 0) uniform sampler2D diffuse_tex;
layout(set = 2, binding = 1) uniform sampler2D lightmap_tex;

layout(set = 3, binding = 0) uniform MaterialUniforms {
    vec4 flags;        // x: has lightmap, y: alpha-test reference (0 = off)
    vec4 sun_dir;      // xyz: direction the light travels (world, Z up)
    vec4 sun_color;
    vec4 ambient;
    vec4 fog;          // rgb: colour, w: density
    vec4 camera;       // xyz: camera position
};

layout(location = 0) out vec4 out_color;

void main() {
    vec4 base = texture(diffuse_tex, v_uv0) * v_color;
    if (flags.y > 0.0 && base.a < flags.y) discard;
    vec3 lit;
    if (flags.x > 0.5) {
        // The game's NV_Lightmap shader: diffuse * lightmap * 2.
        lit = base.rgb * texture(lightmap_tex, v_uv1).rgb * 2.0;
    } else {
        float ndl = max(dot(normalize(v_normal), -sun_dir.xyz), 0.0);
        lit = base.rgb * (ambient.rgb + sun_color.rgb * ndl);
    }
    if (fog.w > 0.0) {
        float d = length(v_world - camera.xyz);
        lit = mix(lit, fog.rgb, 1.0 - exp(-fog.w * d));
    }
    out_color = vec4(lit, base.a);
}
