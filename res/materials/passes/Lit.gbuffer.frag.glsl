layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 surfaceUV;
layout(location=0) out vec4 color;
layout(location=1) out vec4 normalAttribute;
layout(location=2) out vec4 positionAttribute;
layout(location=3) out vec4 propertiesAttribute;
layout(location=4) out vec4 emissionAttribute;

vec3 MappedNormal(vec3 tangentNormal) {
    vec3 n = normalize(worldNormal);
    vec3 dx = dFdx(worldPosition), dy = dFdy(worldPosition);
    vec2 ux = dFdx(surfaceUV), uy = dFdy(surfaceUV);
    float determinant = ux.x * uy.y - ux.y * uy.x;
    if (abs(determinant) < 1e-8 || dot(tangentNormal, tangentNormal) < 1e-8)
        return n;
    vec3 tangent = (dx * uy.y - dy * ux.y) / determinant;
    vec3 bitangent = (dy * ux.x - dx * uy.x) / determinant;
    tangent -= n * dot(n, tangent);
    if (dot(tangent, tangent) < 1e-8) return n;
    tangent = normalize(tangent);
    float handedness = dot(cross(n, tangent), bitangent) < 0.0 ? -1.0 : 1.0;
    bitangent = cross(n, tangent) * handedness;
    return normalize(mat3(tangent, bitangent, n) * normalize(tangentNormal));
}
void main() {
    LitSurface surface = evaluateMaterial(SurfaceInput(worldPosition, worldNormal, surfaceUV), EESG_LoadParameters());
    color = vec4(max(surface.color, vec3(0.0)), surface.opacity);
    normalAttribute = vec4(MappedNormal(surface.tangentNormal), 0.0);
    positionAttribute = vec4(worldPosition, 1.0);
    propertiesAttribute = vec4(clamp(surface.metallic, 0.0, 1.0),
                              clamp(surface.roughness, 0.045, 1.0),
                              clamp(surface.occlusion, 0.0, 1.0), 0.0);
    emissionAttribute = vec4(max(surface.emission, vec3(0.0)), 0.0);
}
