// Material hit-group library. A scene pipeline supplies raygen, miss, SBT,
// acceleration structures, and dispatch. Payload location 0 is linear RGBA.
layout(location = 0) rayPayloadInEXT vec4 materialColor;
hitAttributeEXT vec2 barycentrics;
void main()
{
    // No mesh attribute bindings yet: UV is the triangle barycentric pair,
    // and normal is unavailable (zero).
    SurfaceInput inputSurface = SurfaceInput(
        gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT,
        vec3(0.0), barycentrics);
    MaterialParameters parameters = EESG_LoadParameters();
    LitSurface surface = evaluateMaterial(inputSurface, parameters);
    // Color-only payload until scene ray-hit lighting and mesh attributes exist.
    materialColor = vec4(surface.color, surface.opacity);
}
