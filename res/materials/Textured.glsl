UnlitSurface evaluateMaterial(SurfaceInput inputSurface, MaterialParameters parameters)
{
    vec4 color = SampleTexture2D(parameters.albedo, inputSurface.uv * parameters.uvScale)
                 * parameters.tint;
    return UnlitSurface(color.rgb, color.a);
}
