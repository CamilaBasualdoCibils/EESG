// Linear metallic/roughness surface contract; evaluated during G-buffer fill.
struct SurfaceInput { vec3 position; vec3 normal; vec2 uv; };
struct LitSurface {
    vec3 color;
    float opacity;
    float metallic;
    float roughness;
    vec3 tangentNormal;
    float occlusion;
    vec3 emission;
};
