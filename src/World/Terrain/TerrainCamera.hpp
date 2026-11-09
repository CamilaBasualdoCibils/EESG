#pragma once
#include <Lighthouse/Extended/World/CameraView.hpp>
#include "World/Terrain/TerrainObserverManager.hpp"
namespace EESG {
using namespace Lighthouse;
class TerrainCamera : public Lighthouse::CameraView {
public:
  // Call for any camera owner (player, security camera, editor, ...).
  void UpdateTerrainObserver(TerrainObserverManager& manager,
                             WorldPosition position,
                             Quat rotation = Quat(1, 0, 0, 0))
  {
    if (!manager.Contains(terrainObserver_))
      terrainObserver_ = manager.Register(terrainObserver_ ? *terrainObserver_
                                                           : TerrainObserver{});
    terrainObserver_->position = position;
    terrainObserver_->viewportSize = GetResolution();
    terrainObserver_->verticalFovDegrees = verticalFov;
    if (!(verticalFov > 0 && verticalFov < 180 && nearPlane > 0 &&
          farPlane > nearPlane))
      throw std::invalid_argument("Invalid camera projection settings");
    terrainObserver_->viewProjection =
        glm::perspectiveRH_NO(glm::radians(verticalFov),
                              float(GetResolution().x) / float(GetResolution().y),
                              nearPlane, farPlane) *
        glm::inverse(glm::translate(Mat4(1), Vec3(position)) *
                     glm::mat4_cast(rotation));
  }
  TerrainObserverManager::Handle GetTerrainObserver() const
  {
    return terrainObserver_;
  }
  void ReleaseTerrainObserver()
  {
    terrainObserver_.reset();
  }
private:
 TerrainObserverManager::Handle terrainObserver_;
};
}
