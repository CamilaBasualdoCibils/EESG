#pragma once
#include "Component.hpp"
#include "Materials/Materials.hpp"
#include <Lighthouse/Engine/Rendering/RenderMesh.hpp>
namespace EESG
{
struct MeshRender
{
  EESG_COMPONENT_INSPECTOR(MeshRender, {
    ImGui::TextUnformatted(component.mesh ? "Mesh assigned" : "No mesh");
    ImGui::TextUnformatted(component.material ? "Material assigned" : "No material");
    return false;
})
  Lighthouse::RenderMeshHandle mesh{};
  std::shared_ptr<const MaterialInstance> material;
};
} // namespace EESG
