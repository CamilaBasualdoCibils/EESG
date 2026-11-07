vec3 HelmetDecodeSRGB(vec3 value) {
    return mix(value / 12.92, pow((value + 0.055) / 1.055, vec3(2.4)),
               greaterThan(value, vec3(0.04045)));
}
LitSurface evaluateMaterial(SurfaceInput inputSurface, MaterialParameters parameters) {
    vec2 uv = inputSurface.uv;
    vec4 albedo = SampleTexture2D(parameters.albedo, uv);
    // glTF: roughness in G, metallic in B; data textures remain linear.
    vec4 mr = SampleTexture2D(parameters.metallicRoughness, uv);
    vec3 normal = SampleTexture2D(parameters.normalMap, uv).xyz * 2.0 - 1.0;
    float ao = SampleTexture2D(parameters.occlusion, uv).r;
    vec3 emissive = HelmetDecodeSRGB(SampleTexture2D(parameters.emissive, uv).rgb);
    return LitSurface(HelmetDecodeSRGB(albedo.rgb), albedo.a,
                      mr.b, mr.g, normal, ao, emissive);
}
