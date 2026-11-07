// Shared Unlit contract. Loaded automatically before a material definition.
// Material files supply evaluateMaterial, without #version or main.
struct SurfaceInput
{
    vec3 position;
    vec3 normal;
    vec2 uv;
};

struct MaterialParameters
{
    vec4 baseColor;
    vec4 emission;
    vec4 custom[4];
};

struct UnlitSurface
{
    vec3 color; // Linear RGB; no lighting is applied.
    float opacity;
};
