#pragma once
#include "Component.hpp"
#include "World/CameraView.hpp"
namespace EESG
{
struct Player
{
  EESG_COMPONENT_INSPECTOR(Player, {
    auto& camera = component.cameraView;
    auto size = camera.GetResolution(); int resolution[]{int(size.x), int(size.y)};
    bool rebuild = ImGui::DragInt2("Resolution", resolution, 16, 64, 4096);
    if (rebuild) camera.SetResolution({unsigned(std::clamp(resolution[0],64,4096)), unsigned(std::clamp(resolution[1],64,4096))});
    ImGui::InputInt("Terrain priority", &camera.terrainPriority);
    ImGui::DragFloat("Vertical FOV", &camera.verticalFov, .1f, 1, 179);
    ImGui::DragFloat("Near plane", &camera.nearPlane, .01f, .001f, camera.farPlane - .001f);
    ImGui::DragFloat("Far plane", &camera.farPlane, 1, camera.nearPlane + .001f, 100000);
    return rebuild;
})
  CameraView cameraView;
};
} // namespace EESG
