// G-buffer fill: keep color linear.
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 surfaceUV;
layout(location=0) out vec4 color;
void main() {
    UnlitSurface surface = evaluateMaterial(SurfaceInput(worldPosition, worldNormal, surfaceUV), EESG_LoadParameters());
    color = vec4(surface.color, surface.opacity);
}
