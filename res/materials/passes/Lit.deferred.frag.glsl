// EvaluateLight and LitBRDF are prepended by Materials::LoadShadingModel.
layout(set=0,binding=0) uniform sampler2D colorAttribute;
layout(set=0,binding=1) uniform sampler2D normalAttribute;
layout(set=0,binding=2) uniform sampler2D positionAttribute;
layout(set=0,binding=3) uniform sampler2D propertiesAttribute;
layout(set=0,binding=4) uniform sampler2D emissionAttribute;
layout(location=0) out vec4 color;
vec3 EncodeSRGB(vec3 value) {
    value = max(value, vec3(0.0));
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0/2.4)) - 0.055,
               greaterThan(value, vec3(0.0031308)));
}
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 albedo = texelFetch(colorAttribute, pixel, 0);
    vec3 n = normalize(texelFetch(normalAttribute, pixel, 0).xyz);
    vec3 position = texelFetch(positionAttribute, pixel, 0).xyz;
    vec3 properties = texelFetch(propertiesAttribute, pixel, 0).xyz;
    vec3 toCamera = sceneLighting.cameraPosition.xyz - position;
    vec3 v = toCamera * inversesqrt(max(dot(toCamera, toCamera), 1e-8));
    LitShadingSurface surface = LitShadingSurface(albedo.rgb, n, properties.x, properties.y);
    vec3 result = texelFetch(emissionAttribute, pixel, 0).rgb;
    // A small configurable ambient approximation until environment lighting exists.
    // AO affects indirect diffuse only, never direct lighting or emission.
    result += sceneLighting.ambientRadiance.rgb * albedo.rgb * (1.0 - properties.x) * properties.z;
    for (uint i = 0u; i < sceneLighting.lightCount.x; ++i)
        result += EvaluateSurface(surface, v, EvaluateLight(sceneLighting.lights[i], position));
    color = vec4(EncodeSRGB(result), albedo.a);
}
