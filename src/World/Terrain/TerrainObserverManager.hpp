#pragma once
#include "World/Terrain/TerrainChunkManager.hpp"

namespace EESG
{
struct TerrainLODLevel
{
  double distance;
  uint32_t lod;
};
struct TerrainObserver
{
  WorldPosition position{0};
  std::vector<TerrainLODLevel> lodLevels{{8, 0}, {32, 3}, {64, 5}};
  bool enabled = true;
  int priority = 0;
};
// Observers need no renderer/entity. Keeping the returned handle registers an
// observer; releasing it unregisters it. Weak registrations survive camera moves
// and allow cameras to be destroyed after their world without dangling pointers.
class TerrainObserverManager
{
public:
  using Handle = std::shared_ptr<TerrainObserver>;
  Handle Register(TerrainObserver observer = {})
  {
    Validate(observer);
    auto handle = std::make_shared<TerrainObserver>(std::move(observer));
    observers_.push_back(handle);
    return handle;
  }
  // Validate before committing settings so malformed editor values cannot
  // reach streaming on the following frame.
  void Configure(const Handle& handle, TerrainObserver observer)
  {
    if (!Contains(handle)) throw std::invalid_argument("Observer is not registered with this world");
    Validate(observer);
    *handle=std::move(observer);
  }
  bool Contains(const Handle& handle) const
  {
    for (const auto& observer : observers_)
      if (auto live = observer.lock(); live && live == handle) return true;
    return false;
  }
  std::vector<Handle> Observers() const
  {
    std::vector<Handle> result;
    for (const auto& observer : observers_)
      if (auto live = observer.lock()) result.push_back(std::move(live));
    return result;
  }
  TerrainRequests Requests()
  {
    std::erase_if(observers_, [](const auto& observer) { return observer.expired(); });
    TerrainRequests requests;
    for (const auto& weak : observers_)
    {
      auto observer = weak.lock();
      if (!observer || !observer->enabled || observer->lodLevels.empty()) continue;
      Validate(*observer);
      const double radius = observer->lodLevels.back().distance;
      const auto low = WorldTerrain::key_at(observer->position - WorldPosition(radius));
      const auto high = WorldTerrain::key_at(observer->position + WorldPosition(radius));
      // Bound synchronous work even for accidentally enormous editor values.
      const long double count = (static_cast<long double>(high.x) - low.x + 1) *
          (static_cast<long double>(high.y) - low.y + 1) * (static_cast<long double>(high.z) - low.z + 1);
      if (count > 65536) throw std::length_error("Observer exceeds 65536 chunks");
      for (auto z = low.z; z <= high.z; ++z)
        for (auto y = low.y; y <= high.y; ++y)
          for (auto x = low.x; x <= high.x; ++x)
          {
            const RegionKey key{x,y,z,0};
            const auto origin = WorldTerrain::origin(key);
            double distanceSquared = 0;
            for (int axis = 0; axis < 3; ++axis)
            {
              const double minimum = double(origin[axis]) * WorldTerrain::voxel_width;
              const auto delta = observer->position[axis] -
                  std::clamp(observer->position[axis], minimum, minimum + WorldTerrain::chunk_width);
              distanceSquared += delta * delta;
            }
            for (const auto& band : observer->lodLevels)
              if (distanceSquared <= band.distance * band.distance)
              {
                const auto [it, added] = requests.emplace(key, TerrainRequest{band.lod, observer->priority});
                if (!added) {
                  it->second.lod = std::min(it->second.lod, band.lod);
                  it->second.priority = std::max(it->second.priority, observer->priority);
                }
                break;
              }
          }
    }
    return requests;
  }
private:
  std::vector<std::weak_ptr<TerrainObserver>> observers_;
  static void Validate(const TerrainObserver& observer)
  {
    WorldTerrain::voxel_position(observer.position);
    double previous = -1;
    uint32_t previousLOD = 0;
    for (const auto& band : observer.lodLevels)
    {
      if (!std::isfinite(band.distance) || band.distance < 0 ||
          band.distance <= previous || band.lod > WorldTerrain::chunk_depth ||
          band.lod < previousLOD)
        throw std::invalid_argument("Observer LOD bands must increase in distance and not refine outward");
      previous = band.distance;
      previousLOD = band.lod;
    }
    if (!observer.lodLevels.empty())
    {
      const auto radius=observer.lodLevels.back().distance;
      const auto low=WorldTerrain::key_at(observer.position-WorldPosition(radius));
      const auto high=WorldTerrain::key_at(observer.position+WorldPosition(radius));
      const long double count=(static_cast<long double>(high.x)-low.x+1)*
          (static_cast<long double>(high.y)-low.y+1)*(static_cast<long double>(high.z)-low.z+1);
      if (count>65536) throw std::length_error("Observer exceeds 65536 chunks");
    }
  }
};
} // namespace EESG
