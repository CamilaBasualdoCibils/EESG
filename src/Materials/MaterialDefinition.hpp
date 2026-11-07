#pragma once

#include "Materials/Materials.hpp"
#include <array>

namespace EESG::Detail
{
struct MaterialDefinition
{
  SurfaceModel model = SurfaceModel::Unlit;
  std::filesystem::path shader;
  std::string declarations;
  std::vector<MaterialParameter> parameters;
  std::vector<std::array<uint32_t, 4>> data;
  std::vector<Lighthouse::DescriptorImageInfo> textures;
};
MaterialDefinition ReadDefinition(const std::filesystem::path& path,
                                  const MaterialLoadOptions& options,
                                  bool schemaOnly = false);
} // namespace EESG::Detail
