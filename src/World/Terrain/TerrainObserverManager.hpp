#pragma once
#include "TerrainTypes.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

namespace EESG
{
using namespace Lighthouse;
enum class TerrainObserverType : uint32_t
{
  Point,
  Pyramid
};
struct TerrainObserver
{
  WorldPosition position{0};
  float maxCellSizePixels = 4.0f;
  UVec2 viewportSize{1280, 720};
  // Point observers use this reference perspective projection in every
  // direction. Pyramid observers derive their pixel scale and depth from
  // viewProjection.
  float verticalFovDegrees = 45.0f;
  bool enabled = true;
  TerrainObserverType type = TerrainObserverType::Point;
  // World -> clip, right-handed, clip z in [-w,+w]. Cameras supply
  // projection*view.
  Mat4 viewProjection{1};
};
class TerrainObserverManager
{
public:
  using Handle = std::shared_ptr<TerrainObserver>;
  Handle Register(TerrainObserver observer = {})
  {
    Validate(observer);
    auto handle = std::make_shared<TerrainObserver>(std::move(observer));
    std::erase_if(observers_, [](const auto& p) { return p.expired(); });
    observers_.push_back(handle);
    return handle;
  }
  void Configure(const Handle& handle, TerrainObserver observer)
  {
    if (!Contains(handle))
      throw std::invalid_argument("Observer is not registered with this world");
    Validate(observer);
    *handle = std::move(observer);
  }
  bool Contains(const Handle& handle) const
  {
    for (auto& weak : observers_)
      if (auto p = weak.lock(); p && p == handle)
        return true;
    return false;
  }
  std::vector<Handle> Observers() const
  {
    std::vector<Handle> result;
    for (auto& weak : observers_)
      if (auto p = weak.lock())
        result.push_back(std::move(p));
    return result;
  }
  static void Validate(const TerrainObserver& observer)
  {
    for (int axis = 0; axis < 3; ++axis)
      if (!std::isfinite(observer.position[axis]))
        throw std::invalid_argument("Non-finite observer position");
    if (!std::isfinite(observer.maxCellSizePixels) ||
        observer.maxCellSizePixels <= 0)
      throw std::invalid_argument(
          "Maximum cell size must be finite and positive");
    if (!observer.viewportSize.x || !observer.viewportSize.y)
      throw std::invalid_argument("Observer viewport must be nonzero");
    if (!std::isfinite(observer.verticalFovDegrees) ||
        observer.verticalFovDegrees <= 0 || observer.verticalFovDegrees >= 180)
      throw std::invalid_argument(
          "Observer vertical FOV must be between 0 and 180 degrees");
    for (int axis = 0; axis < 3; ++axis)
      if (std::abs(observer.position[axis]) >
          std::numeric_limits<float>::max() / 4.0)
        throw std::out_of_range("Observer exceeds GPU coordinate range");
    if (observer.type != TerrainObserverType::Point &&
        observer.type != TerrainObserverType::Pyramid)
      throw std::invalid_argument("Invalid observer type");
    if (observer.type == TerrainObserverType::Pyramid)
    {
      for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
          if (!std::isfinite(observer.viewProjection[c][row]))
            throw std::invalid_argument("Non-finite observer transform");
      if (glm::determinant(glm::dmat4(observer.viewProjection)) == 0.0)
        throw std::invalid_argument("Singular observer transform");
    }
  }

private:
  std::vector<std::weak_ptr<TerrainObserver>> observers_;
};
} // namespace EESG
