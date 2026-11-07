// Shared incident-light evaluation. No material/BRDF behavior belongs here.
const uint LIGHT_DIRECTIONAL = 0u;
const uint LIGHT_POINT = 1u;
const uint LIGHT_SPOT = 2u;
struct LightData {
    vec4 colorIntensity;
    vec4 positionRange;
    vec4 directionType;
    vec4 spotCosines;
};
struct LightSample { vec3 direction; vec3 radiance; };
layout(set=1, binding=0, std430) readonly buffer SceneLighting {
    vec4 cameraPosition;
    vec4 ambientRadiance;
    uvec4 lightCount; // total count; fullscreen, point, spot range starts.
    mat4 viewProjection;
    LightData lights[];
} sceneLighting;

LightSample EvaluateLight(LightData light, vec3 position) {
    uint type = uint(light.directionType.w);
    vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
    if (type == LIGHT_DIRECTIONAL)
        return LightSample(-light.directionType.xyz, radiance);
    if (type != LIGHT_POINT && type != LIGHT_SPOT)
        return LightSample(vec3(0.0), vec3(0.0));
    vec3 offset = light.positionRange.xyz - position;
    float d2 = dot(offset, offset);
    float range = light.positionRange.w;
    if (range <= 0.0 || d2 <= 1e-8 || d2 >= range * range)
        return LightSample(vec3(0.0), vec3(0.0));
    vec3 direction = offset * inversesqrt(d2);
    float ratio = d2 / (range * range);
    float window = max(1.0 - ratio * ratio, 0.0);
    float attenuation = window * window / max(d2, 1e-4);
    if (type == LIGHT_SPOT) {
        float cosine = dot(-direction, light.directionType.xyz);
        float inner = light.spotCosines.x;
        float outer = light.spotCosines.y;
        attenuation *= inner > outer ? smoothstep(outer, inner, cosine)
                                     : step(outer, cosine);
    }
    return LightSample(direction, radiance * attenuation);
}
