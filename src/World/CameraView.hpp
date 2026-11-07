#pragma once
#include "Math/Types.hpp"
#include "World/PostProcess.hpp"
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
  float verticalFov = 45.0f;
  float nearPlane = 0.1f;
  float farPlane = 100.0f;
  std::vector<std::unique_ptr<IPostProcess>> postProcesses;

private:
  friend class WorldRenderer;
  UVec2 m_resolution{1280, 720};
  // Valid only for the current compiled graph; the graph owns the image.
  Lighthouse::RenderGraphResource m_output{Lighthouse::kInvalidGraphResource};
};
} // namespace EESG
