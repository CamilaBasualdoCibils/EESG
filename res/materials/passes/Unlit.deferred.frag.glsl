#version 450
layout(set=0,binding=0) uniform sampler2D colorAttribute;
layout(location=0) out vec4 color;
void main() {
    vec4 surface = texelFetch(colorAttribute, ivec2(gl_FragCoord.xy), 0);
    color = surface;
}
