#version 450
layout(set=0,binding=0) uniform sampler2D colorAttribute;
layout(location=0) out vec4 color;
vec3 EncodeSRGB(vec3 value) {
    value = max(value, vec3(0.0));
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0/2.4)) - 0.055,
               greaterThan(value, vec3(0.0031308)));
}
void main() {
    vec4 surface = texelFetch(colorAttribute, ivec2(gl_FragCoord.xy), 0);
    color = vec4(EncodeSRGB(surface.rgb), surface.a);
}
