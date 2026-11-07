UnlitSurface evaluateMaterial(SurfaceInput inputSurface, MaterialParameters parameters)
{
    return UnlitSurface(parameters.baseColor.rgb + parameters.emission.rgb,
                        parameters.baseColor.a);
}
