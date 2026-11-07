#include "World/Terrain/TerrainMesher.hpp"
#include <stdexcept>

using namespace EESG;
static void check(bool value)
{
  if (!value) throw std::runtime_error("Terrain mesher check failed");
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
    check(glm::dot(glm::cross(b.position - a.position, c.position - a.position), a.normal) > 0);
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
}
