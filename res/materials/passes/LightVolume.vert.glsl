layout(location=0) in vec3 position;
layout(location=1) in vec3 volumeShape;
layout(location=0) flat out uint lightIndex;
void main() {
    uint shape = uint(volumeShape.x);
    // Three instanced indirect draws use firstInstance=0. Sorted per-shape
    // ranges avoid depending on OpenGL base-instance shader emulation.
    lightIndex = sceneLighting.lightCount[shape + 1u] + uint(gl_InstanceIndex);
    LightData light = sceneLighting.lights[lightIndex];
    if (shape == 0u) {
        gl_Position = vec4(position.xy, 0.0, 1.0);
        return;
    }
    vec3 world;
    if (shape == 1u) {
        world = light.positionRange.xyz + position * light.positionRange.w;
    } else {
        vec3 forward = light.directionType.xyz;
        vec3 up = abs(forward.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0);
        vec3 right = normalize(cross(forward, up));
        up = cross(right, forward);
        float cosine = max(light.spotCosines.y, 1e-6);
        float radius = light.positionRange.w * sqrt(max(1.0-cosine*cosine,0.0)) / cosine;
        world = light.positionRange.xyz - forward * position.z * light.positionRange.w
              + (right*position.x + up*position.y)*radius;
    }
    gl_Position = sceneLighting.viewProjection * vec4(world, 1.0);
    // Preserve projected coverage when an exit face crosses near/far depth.
    // XY/W remain unchanged; behind-camera triangles are still clipped.
    gl_Position.z = clamp(gl_Position.z, 0.0, max(gl_Position.w, 0.0));
}
