// Lit's surface response. Other shading models can replace this function
// while continuing to use the same EvaluateLight and LightSample interface.
struct LitShadingSurface {
    vec3 albedo;
    vec3 normal;
    float metallic;
    float roughness;
};
vec3 EvaluateSurface(LitShadingSurface surface, vec3 viewDirection, LightSample light) {
    const float pi = 3.141592653589793;
    float nl = max(dot(surface.normal, light.direction), 0.0);
    float nv = max(dot(surface.normal, viewDirection), 0.0);
    if (nl <= 0.0 || nv <= 0.0 || !any(greaterThan(light.radiance, vec3(0.0))))
        return vec3(0.0);
    vec3 halfway = viewDirection + light.direction;
    float h2 = dot(halfway, halfway);
    if (h2 <= 1e-8) return vec3(0.0);
    halfway *= inversesqrt(h2);
    float nh = max(dot(surface.normal, halfway), 0.0);
    float vh = clamp(dot(viewDirection, halfway), 0.0, 1.0);
    float metallic = clamp(surface.metallic, 0.0, 1.0);
    float roughness = clamp(surface.roughness, 0.045, 1.0);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    // GGX distribution, height-correlated Smith visibility, Schlick Fresnel.
    float denominator = nh * nh * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(pi * denominator * denominator, 1e-12);
    float masking = nl * sqrt(nv * nv * (1.0 - alpha2) + alpha2)
                  + nv * sqrt(nl * nl * (1.0 - alpha2) + alpha2);
    float visibility = 0.5 / max(masking, 1e-6);
    vec3 f0 = mix(vec3(0.04), surface.albedo, metallic);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * surface.albedo / pi;
    vec3 specular = distribution * visibility * fresnel;
    return (diffuse + specular) * light.radiance * nl;
}
