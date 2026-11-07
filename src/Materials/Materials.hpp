#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace EESG
{
enum class SurfaceModel
{
  Unlit
};

// A loaded material definition, not a parameterized instance or GPU pipeline.
struct Material
{
  SurfaceModel model = SurfaceModel::Unlit;
  std::filesystem::path path;
  // Complete GLSL library: base declarations followed by evaluateMaterial.
  // No #version or main; a rendering pass supplies those later.
  std::string source;
};

class Materials
{
public:
  // Defaults to the project's root-level res directory, independent of cwd.
  Materials();
  explicit Materials(std::filesystem::path resourceRoot);

  // Relative paths resolve against resourceRoot. Absolute paths also work.
  // Validates the Unlit contract using GLSL compilation before caching.
  // Repeated loads of the same canonical path return the same definition.
  std::shared_ptr<const Material> Load(const std::filesystem::path& path);

private:
  std::filesystem::path m_resourceRoot;
  std::unordered_map<std::string, std::shared_ptr<const Material>> m_loaded;
};
} // namespace EESG
