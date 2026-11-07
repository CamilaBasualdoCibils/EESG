UnlitSurface evaluateMaterial(SurfaceInput inputSurface, MaterialParameters parameters)
{
    ivec3 cell = ivec3(floor(inputSurface.position * parameters.frequency + vec3(0.001)));
    float checker = float((cell.x + cell.y + cell.z) & 1);
    return UnlitSurface(mix(parameters.darkColor, parameters.lightColor, checker), 1.0);
}
