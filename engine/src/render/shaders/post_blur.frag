#version 450

// One direction of the game's separable blur (BlurFS.glsl, HBLUR / VBLUR): five taps
// with bilinear weights at 1.385 and 3.231 steps (PostEffects::PostEffect::RenderBlur).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D source;

layout(set = 3, binding = 0) uniform BlurUniforms {
    vec4 delta;   // xy: first tap offset (UV), zw: second
};

void main() {
    vec3 sum = texture(source, v_uv).rgb * 0.2270270270;
    sum += texture(source, v_uv + delta.xy).rgb * 0.3162162162;
    sum += texture(source, v_uv - delta.xy).rgb * 0.3162162162;
    sum += texture(source, v_uv + delta.zw).rgb * 0.0702702703;
    sum += texture(source, v_uv - delta.zw).rgb * 0.0702702703;
    out_color = vec4(sum, 1.0);
}
