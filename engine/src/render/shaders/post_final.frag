#version 450

// Scene post effects in one pass, ported from the game's PostEffect.bdae shaders:
//   DOF (DOFFS.glsl): mix towards the blurred image by |focus - linear depth| * range,
//     with the game's depth linearisation (a 0.1..100 projection) evaluated at the
//     pixel's view depth.
//   HeightFog (HeightFogFS.glsl): mix towards the fog colour by smoothstep(top, bottom,
//     world z), or the camera's own height if that is foggier.
//   ColorCorrection (CCFS.glsl): the level's LUT read with the colour as coordinates, red
//     and green from (r, g), blue from (b, 0.5); its LUTs are per-channel curves, which
//     the game code bakes (blended between two LUTs while they change) into 256 entries.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D scene;
layout(set = 2, binding = 1) uniform sampler2D blurred;
layout(set = 2, binding = 2) uniform sampler2D depth;
layout(set = 2, binding = 3) uniform sampler2D grading_curve;

layout(set = 3, binding = 0) uniform PostUniforms {
    mat4 inv_view_proj;
    vec4 camera;      // xyz
    vec4 forward;     // xyz: view direction
    vec4 dof;         // focus, range, factor, enabled
    vec4 fog;         // top, bottom, opacity, enabled
    vec4 fog_color;
    vec4 grading;     // enabled
};

float smooth_edge(float e0, float e1, float x) {   // GLSL smoothstep, allowing e0 > e1
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    vec4 color = texture(scene, v_uv);
    float d = texture(depth, v_uv).r;   // reversed: 1 near, 0 far (sky)
    vec4 p = inv_view_proj * vec4(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0, max(d, 1e-7), 1.0);
    vec3 world = p.xyz / p.w;

    if (dof.w > 0.5) {
        const float n = 0.1, f = 100.0;
        float view_depth = d <= 0.0 ? 1e6 : max(dot(world - camera.xyz, forward.xyz), n);
        float z_ndc = (f + n) / (f - n) - 2.0 * f * n / ((f - n) * view_depth);
        float z_win = z_ndc * 0.5 + 0.5;
        float linear = (2.0 * n) / (f + n - z_win * (f - n));
        float k = clamp(dof.y * abs(dof.x - linear), 0.0, 1.0);
        color = mix(color, texture(blurred, v_uv), k * dof.z);
    }
    if (fog.w > 0.5) {
        float amount = smooth_edge(fog.x, fog.y, world.z);   // the sky: the far point along the ray
        float camera_fog = smooth_edge(fog.x, fog.y, camera.z);
        color = mix(color, vec4(fog_color.rgb, 1.0), max(amount, camera_fog) * fog.z);
    }
    if (grading.x > 0.5) {
        vec3 c = clamp(color.rgb, 0.0, 1.0) * (255.0 / 256.0) + 0.5 / 256.0;
        color.rgb = vec3(texture(grading_curve, vec2(c.r, 0.5)).r, texture(grading_curve, vec2(c.g, 0.5)).g,
                         texture(grading_curve, vec2(c.b, 0.5)).b);
    }
    out_color = vec4(color.rgb, 1.0);
}
