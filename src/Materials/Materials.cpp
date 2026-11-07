#include "Materials/Materials.hpp"

#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace EESG
{
namespace
{
std::string ReadSource(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot open material GLSL: " + path.string());
  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad())
    throw std::runtime_error("Cannot read material GLSL: " + path.string());
  return text.str();
}
} // namespace

Materials::Materials() : Materials(EESG_RESOURCE_ROOT) {}

Materials::Materials(std::filesystem::path resourceRoot)
    : m_resourceRoot(std::filesystem::absolute(std::move(resourceRoot)))
{
}

std::shared_ptr<const Material>
Materials::Load(const std::filesystem::path& path)
{
  const auto resolved = std::filesystem::canonical(
      path.is_absolute() ? path : m_resourceRoot / path);
  const auto key = resolved.generic_string();
  if (const auto found = m_loaded.find(key); found != m_loaded.end())
    return found->second;

  auto material = std::make_shared<Material>();
  material->path = resolved;
  material->source = ReadSource(m_resourceRoot / "materials/Unlit.glsl") +
                     "\n#line 1 1\n" + ReadSource(resolved);

  // Compile a temporary entry point to validate the function signature and
  // body. This creates no GPU resources and is not a rendering pass.
  const std::string validation = "#version 450\n" + material->source + R"glsl(
#line 1 2
layout(location = 0) out vec4 eesgValidationColor;
void main()
{
    SurfaceInput inputSurface;
    inputSurface.position = vec3(0.0);
    inputSurface.normal = vec3(0.0, 1.0, 0.0);
    inputSurface.uv = vec2(0.0);
    MaterialParameters parameters;
    parameters.baseColor = vec4(1.0);
    parameters.emission = vec4(0.0);
    for (int i = 0; i < 4; ++i) parameters.custom[i] = vec4(0.0);
    UnlitSurface surface = evaluateMaterial(inputSurface, parameters);
    eesgValidationColor = vec4(surface.color, surface.opacity);
}
)glsl";
  try
  {
    Lighthouse::ShaderCompiler::Compile(
        validation, {.stage = Lighthouse::ShaderStage::Fragment});
  }
  catch (const std::exception& error)
  {
    throw std::runtime_error(
        "Invalid Unlit material '" + key +
        "' (GLSL source 0: Unlit base, 1: material, 2: validation):\n" +
        error.what());
  }
  m_loaded.emplace(key, material);
  return material;
}
} // namespace EESG
