LitSurface evaluateMaterial(SurfaceInput inputSurface, MaterialParameters parameters)
{
    ivec3 cell = ivec3(floor(inputSurface.position * parameters.frequency + vec3(0.001)));
    float checker = float((cell.x + cell.y + cell.z) & 1);
    return LitSurface(mix(parameters.darkColor, parameters.lightColor, checker),
                      1.0, 0.0, 0.9, vec3(0.0, 0.0, 1.0), 1.0, vec3(0.0));
}
