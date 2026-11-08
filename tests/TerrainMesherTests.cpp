#include "World/Terrain/TerrainMesher.hpp"
#include <stdexcept>

using namespace EESG;
static void check(bool value)
{
  if (!value)
    throw std::runtime_error("Terrain mesher check failed");
}
int main()
{
  WorldTerrain terrain;
  check(TerrainMesher::Build(terrain, {}).indices.empty());
  terrain.set_voxel({255, 0, 0}, {0, 1});
  auto mesh = TerrainMesher::Build(terrain, {});
  check(mesh.vertices.size() == 24 && mesh.indices.size() == 36);
  for (size_t i = 0; i < mesh.indices.size(); i += 3)
  {
    const auto& a = mesh.vertices[mesh.indices[i]];
    const auto& b = mesh.vertices[mesh.indices[i + 1]];
    const auto& c = mesh.vertices[mesh.indices[i + 2]];
    check(glm::dot(glm::cross(b.position - a.position, c.position - a.position),
                   a.normal) > 0);
  }
  terrain.set_voxel({256, 0, 0}, {2, 0.1f});
  check(TerrainMesher::Build(terrain, {}).indices.size() == 30);
  check(TerrainMesher::Build(terrain, {1, 0, 0, 0}).indices.size() == 30);
  const auto revision = terrain.revision();
  terrain.set_voxel({256, 0, 0}, {});
  check(terrain.revision() != revision);
  check(TerrainMesher::Build(terrain, {}).indices.size() == 36);
  terrain.set_voxel({-1, -1, -1}, {3, 1});
  mesh = TerrainMesher::Build(terrain, {-1, -1, -1, 0});
  check(mesh.indices.size() == 36);
  for (const auto& vertex : mesh.vertices)
    check(vertex.position.x >= -0.125f && vertex.position.x <= 0);
  terrain.clear();
  check(TerrainMesher::Build(terrain, {}).indices.empty());
  // A uniform 32 m chunk used to produce 393216 quads; greedy faces need six.
  terrain.apply({{}, {}, 0, {1, 1}});
  auto full = TerrainMesher::Build(terrain, {});
  check(full.vertices.size() == 24 && full.indices.size() == 36);
  terrain.set_voxel({384, 128, 128}, {1, 1});
  check(TerrainMesher::Build(terrain, {}, 8).indices.size() == 36);
  terrain.apply({{1, 0, 0, 0}, {}, 0, {2, 1}});
  check(TerrainMesher::Build(terrain, {}).indices.size() == 30);
  // Partial coverage at the boundary must leave the uncovered face visible.
  terrain.clear();
  terrain.apply({{}, {}, 7, {1, 1}}); // 2x2x2 finest voxels
  terrain.set_voxel({2, 0, 0}, {2, 1});
  mesh = TerrainMesher::Build(terrain, {});
  double area = 0;
  for (size_t i = 0; i < mesh.indices.size(); i += 3)
  {
    const auto& a = mesh.vertices[mesh.indices[i]];
    const auto& b = mesh.vertices[mesh.indices[i + 1]];
    const auto& c = mesh.vertices[mesh.indices[i + 2]];
    const auto cross =
        glm::cross(b.position - a.position, c.position - a.position);
    check(glm::dot(cross, a.normal) > 0);
    area += glm::length(cross) * 0.5;
  }
  check(std::abs(area - 28 * WorldTerrain::voxel_width *
                            WorldTerrain::voxel_width) < 1e-6);
  // Snapshots and coordinates at the signed limits keep outside space as air.
  terrain.clear();
  terrain.set_voxel({INT64_MAX, 0, 0}, {1, 1});
  check(TerrainMesher::Build(terrain,
                             WorldTerrain::key_at_voxel({INT64_MAX, 0, 0}))
            .indices.size() == 36);
}
