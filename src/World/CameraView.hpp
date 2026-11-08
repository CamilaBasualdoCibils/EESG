#pragma once
#include "Math/Types.hpp"
#include "World/PostProcess.hpp"
#include "World/Terrain/TerrainObserverManager.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <stdexcept>
#include <vector>
namespace EESG
{
class CameraView
{
public:
  CameraView() = default;
  CameraView(const CameraView&) = delete;
  CameraView& operator=(const CameraView&) = delete;
  CameraView(CameraView&&) noexcept = default;
  CameraView& operator=(CameraView&&) noexcept = default;
  void SetResolution(UVec2 size)
  {
    if (!size.x || !size.y)
      throw std::invalid_argument("Camera resolution must be nonzero");
    m_resolution = size;
  }
  UVec2 GetResolution() const noexcept
  {
    return m_resolution;
  }
  Lighthouse::RenderGraphResource GetOutput() const noexcept
  {
    return m_output;
  }
  // Call for any camera owner (player, security camera, editor, ...).
  void UpdateTerrainObserver(TerrainObserverManager& manager,
                             WorldPosition position,
                             Quat rotation = Quat(1, 0, 0, 0))
  {
    if (!manager.Contains(terrainObserver_))
      terrainObserver_ = manager.Register(terrainObserver_ ? *terrainObserver_
                                                           : TerrainObserver{});
    terrainObserver_->position = position;
    terrainObserver_->viewportSize = m_resolution;
    terrainObserver_->verticalFovDegrees = verticalFov;
    if (!(verticalFov > 0 && verticalFov < 180 && nearPlane > 0 &&
          farPlane > nearPlane))
      throw std::invalid_argument("Invalid camera projection settings");
    terrainObserver_->viewProjection =
        glm::perspectiveRH_NO(glm::radians(verticalFov),
                              float(m_resolution.x) / float(m_resolution.y),
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
  float verticalFov = 45.0f;
  float nearPlane = 0.1f;
  float farPlane = 100.0f;
  std::vector<std::unique_ptr<IPostProcess>> postProcesses;

private:
  TerrainObserverManager::Handle terrainObserver_;
  friend class WorldRenderer;
  UVec2 m_resolution{1280, 720};
  // Valid only for the current compiled graph; the graph owns the image.
  Lighthouse::RenderGraphResource m_output{Lighthouse::kInvalidGraphResource};
};
} // namespace EESG
