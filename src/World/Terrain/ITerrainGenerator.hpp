#pragma once
#include "World/Terrain/WorldTerrain.hpp"
#include <functional>
#include <optional>
#include <stop_token>

namespace EESG
{
// Arbitrary volumetric field: positive signed density is solid. Geometry and
// persistence are independent of the field. WorldTerrain stores occupancy
// [0,1].
class TerrainGenerationCancelled : public std::exception
{
public:
  const char* what() const noexcept override
  {
    return "Terrain generation cancelled";
  }
};
class ITerrainGenerator
{
public:
  virtual ~ITerrainGenerator() = default;
  // The chunk manager may call this generator concurrently for different
  // chunks. Implementations must keep sampling/generation read-only or
  // synchronize their own mutable state; each output tree has one owner.
  virtual double Density(WorldPosition position) const = 0;
  virtual TerrainVoxel Sample(WorldPosition position) const
  {
    return Density(position) > 0 ? TerrainVoxel{1, 1} : TerrainVoxel{};
  }
  // Occupancy samples become cubes, without interpolating an isosurface.
  // LOD n uses 2^n finest voxels per side: 1, 2, 4, 8, ... .
  // Finest voxels are 0.125 m; LOD 8 is one 32 m voxel per chunk.
  virtual std::unique_ptr<TerrainRegion::Tree>
  Generate(RegionKey key, uint32_t lod,
           std::shared_ptr<std::pmr::memory_resource> memory = {},
           std::stop_token stop = {}) const
  {
    WorldTerrain::validate(key);
    if (key.scale || lod > WorldTerrain::chunk_depth)
      throw std::out_of_range("Generation requires base chunk and LOD 0..8");
    auto tree = std::make_unique<TerrainRegion::Tree>(WorldTerrain::chunk_depth,
                                                      std::move(memory));
    const auto origin = WorldTerrain::origin(key);
    const uint32_t targetDepth = WorldTerrain::chunk_depth - lod;
    using Uniform = std::optional<TerrainVoxel>;
    std::function<Uniform(TerrainRegion::Tree::Point, uint32_t)> build;
    build = [&](TerrainRegion::Tree::Point local, uint32_t depth) -> Uniform
    {
      if (stop.stop_requested())
        throw TerrainGenerationCancelled{};
      const uint32_t width = 1u << (WorldTerrain::chunk_depth - depth);
      WorldPosition center;
      for (size_t axis = 0; axis < 3; ++axis)
        center[axis] = (double(origin[axis]) + local[axis] + width * 0.5) *
                       WorldTerrain::voxel_width;
      if (depth == targetDepth)
      {
        const auto value = Sample(center);
        if (!std::isfinite(value.density) || value.density < 0 ||
            value.density > 1)
          throw std::invalid_argument("Generator occupancy must be in [0,1]");
        return value;
      }
      if (auto uniform =
              UniformSample(center, width * WorldTerrain::voxel_width * 0.5))
        return uniform;
      std::array<Uniform, 8> children;
      std::array<TerrainRegion::Tree::Point, 8> positions;
      for (uint32_t i = 0; i < 8; ++i)
      {
        positions[i] = local;
        for (size_t axis = 0; axis < 3; ++axis)
          positions[i][axis] += ((i >> axis) & 1) * (width / 2);
        children[i] = build(positions[i], depth + 1);
      }
      bool uniform = children[0].has_value();
      for (size_t i = 1; i < 8; ++i)
        uniform &= children[i].has_value() && children[0].has_value() &&
                   *children[i] == *children[0];
      if (uniform)
        return children[0];
      for (uint32_t i = 0; i < 8; ++i)
        if (children[i] && children[i]->density > 0)
          tree->paint({positions[i], depth + 1}, *children[i]);
      return {};
    };
    if (auto root = build({}, 0); root && root->density > 0)
      tree->paint({{}, 0}, *root);
    return tree;
  }

protected:
  // Optional, conservative uniform-region hint. Returning a value promises
  // every point in the cube has that occupancy/material. Generic generators
  // still sample the full requested voxel grid and coalesce equal cells.
  virtual std::optional<TerrainVoxel> UniformSample(WorldPosition, double) const
  {
    return {};
  }
};
} // namespace EESG
