#pragma once

#include "World/Terrain/WorldTerrain.hpp"
#include <Lighthouse/Engine/Assets/Mesh.hpp>

namespace EESG
{
// Block surface mesher. Positive density is solid; material zero is valid.
// World-space vertices allow all chunks to share one identity model matrix.
class TerrainMesher
{
public:
  static Lighthouse::Mesh3D Build(const WorldTerrain& terrain, RegionKey key)
  {
    if (key.scale != 0)
      throw std::invalid_argument("Voxel mesher requires a source chunk");
    Lighthouse::Mesh3D mesh;
    const auto* tree = terrain.source(key);
    if (!tree) return mesh;
    const auto origin = WorldTerrain::origin(key);
    tree->visit([&](const TerrainRegion::Tree::Entry& entry) {
      if (!entry.terminal || tree->cell(entry.handle).density <= 0) return;
      const auto width = 1u << (WorldTerrain::chunk_depth - entry.region.depth);
      for (uint32_t z = 0; z < width; ++z)
        for (uint32_t y = 0; y < width; ++y)
          for (uint32_t x = 0; x < width; ++x)
          {
            VoxelPosition voxel = origin;
            voxel[0] += entry.region.origin[0] + x;
            voxel[1] += entry.region.origin[1] + y;
            voxel[2] += entry.region.origin[2] + z;
            for (int axis = 0; axis < 3; ++axis)
              for (int sign : {-1, 1})
              {
                auto neighbor = voxel;
                // At the coordinate limits, the outside is air.
                const bool outside = sign > 0
                    ? neighbor[axis] == std::numeric_limits<int64_t>::max()
                    : neighbor[axis] == std::numeric_limits<int64_t>::min();
                if (!outside) neighbor[axis] += sign;
                if (!outside && terrain.sample_voxel(neighbor).density > 0) continue;
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                Vec3 base;
                for (int a = 0; a < 3; ++a)
                  base[a] = float(double(voxel[a]) * WorldTerrain::voxel_width);
                if (sign > 0) base[axis] += float(WorldTerrain::voxel_width);
                Vec3 du(0), dv(0), normal(0);
                du[u] = dv[v] = float(WorldTerrain::voxel_width);
                normal[axis] = float(sign);
                if (sign < 0) std::swap(du, dv);
                const auto first = static_cast<uint32_t>(mesh.vertices.size());
                for (auto position : {base, base + du, base + du + dv, base + dv})
                  mesh.vertices.push_back({position, normal, {}});
                for (auto i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(first + i);
              }
          }
    });
    return mesh;
  }
};
} // namespace EESG
