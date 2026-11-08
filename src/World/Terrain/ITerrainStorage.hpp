#pragma once
#include "World/Terrain/WorldTerrain.hpp"
#include <optional>
namespace EESG
{
class ITerrainStorage
{
public:
  virtual ~ITerrainStorage() = default;
  // Smallest available LOD number, or no stored data. Load returns data at
  // exactly requested LOD (storage may downsample finer data), null on miss.
  virtual std::optional<uint32_t> Query(RegionKey key) const = 0;
  virtual std::unique_ptr<TerrainRegion::Tree> Load(RegionKey key, uint32_t lod,
      std::shared_ptr<std::pmr::memory_resource> memory) = 0;
  virtual void Save(RegionKey key, uint32_t lod, const TerrainRegion::Tree& data) = 0;
};
class NullTerrainStorage final : public ITerrainStorage
{
public:
  std::optional<uint32_t> Query(RegionKey) const override { return std::nullopt; }
  std::unique_ptr<TerrainRegion::Tree> Load(RegionKey, uint32_t,
      std::shared_ptr<std::pmr::memory_resource>) override { return {}; }
  void Save(RegionKey, uint32_t, const TerrainRegion::Tree&) override {}
};
} // namespace EESG
