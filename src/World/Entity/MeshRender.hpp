#pragma once
#include "Materials/Materials.hpp"
#include <Lighthouse/Engine/Rendering/RenderMesh.hpp>
namespace EESG
{
struct MeshRender
{
  Lighthouse::RenderMeshHandle mesh{};
  std::shared_ptr<const MaterialInstance> material;
};
} // namespace EESG
