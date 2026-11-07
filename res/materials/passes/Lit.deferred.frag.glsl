// EvaluateLight and LitBRDF are prepended by Materials::LoadShadingModel.
layout(set=0,binding=0) uniform sampler2D colorAttribute;
layout(set=0,binding=1) uniform sampler2D normalAttribute;
layout(set=0,binding=2) uniform sampler2D positionAttribute;
layout(set=0,binding=3) uniform sampler2D propertiesAttribute;
layout(set=0,binding=4) uniform sampler2D emissionAttribute;
layout(location=0) out vec4 color;
layout(location=0) flat in uint lightIndex;
void main() {
    LightData light = sceneLighting.lights[lightIndex];
    uint type = uint(light.directionType.w);
    // Render only exit faces of closed volumes: each covered pixel is shaded
    // once, including when the camera lies inside the sphere or cone.
    if (lightIndex != 0u && (type == LIGHT_POINT || type == LIGHT_SPOT) && gl_FrontFacing) discard;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 albedo = texelFetch(colorAttribute, pixel, 0);
    vec3 n = normalize(texelFetch(normalAttribute, pixel, 0).xyz);
    vec3 position = texelFetch(positionAttribute, pixel, 0).xyz;
    vec3 properties = texelFetch(propertiesAttribute, pixel, 0).xyz;
    vec3 toCamera = sceneLighting.cameraPosition.xyz - position;
    vec3 v = toCamera * inversesqrt(max(dot(toCamera, toCamera), 1e-8));
    LitShadingSurface surface = LitShadingSurface(albedo.rgb, n, properties.x, properties.y);
    vec3 result;
    if (lightIndex == 0u) {
        // One fullscreen base instance initializes ambient and emission.
        result = texelFetch(emissionAttribute, pixel, 0).rgb;
        result += sceneLighting.ambientRadiance.rgb * albedo.rgb * (1.0 - properties.x) * properties.z;
        color = vec4(result, albedo.a);
    } else {
        result = EvaluateSurface(surface, v, EvaluateLight(light, position));
        color = vec4(result, 0.0);
    }
}
