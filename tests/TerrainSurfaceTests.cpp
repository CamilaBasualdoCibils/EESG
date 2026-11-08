#include "World/Terrain/PerlinTerrainGenerator.hpp"
#include "World/Terrain/TerrainMemoryBudget.hpp"
#include "World/Terrain/TerrainMesher.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace EESG;
static void BlockGeometry(const Lighthouse::Mesh3D& mesh, double width)
{
  assert(!mesh.indices.empty());
  for (const auto& vertex : mesh.vertices)
  {
    // No interpolated vertices or smoothed normals, at any LOD.
    for (int axis = 0; axis < 3; ++axis)
    {
      const auto grid = vertex.position[axis] / width;
      assert(std::abs(grid - std::round(grid)) < 1e-5);
    }
    const auto n = glm::abs(vertex.normal);
    assert(n == Vec3(1, 0, 0) || n == Vec3(0, 1, 0) || n == Vec3(0, 0, 1));
  }
}
int main()
{
  // Every LOD is a cube with twice the previous LOD's edge length.
  for (uint32_t lod = 0; lod <= 8; ++lod)
  {
    const auto step = 1u << lod;
    const auto width = step * WorldTerrain::voxel_width;
    WorldTerrain world;
    world.apply({{}, {}, WorldTerrain::chunk_depth - lod, {1, 1}});
    const auto mesh = TerrainMesher::Build(world, {}, lod);
    assert(mesh.vertices.size() == 24 && mesh.indices.size() == 36);
    BlockGeometry(mesh, width);
    Vec3 minimum(1000), maximum(-1000);
    for (const auto& vertex : mesh.vertices)
    {
      minimum = glm::min(minimum, vertex.position);
      maximum = glm::max(maximum, vertex.position);
    }
    assert(minimum == Vec3(0) && maximum == Vec3(float(width)));
  }
  PerlinTerrainGenerator perlin;
  // The visible region contains individual filled/empty voxels evaluated in
  // world space, rather than one solid chunk or an interpolated density shell.
  for (uint32_t lod : {4u, 5u, 6u})
  {
    const RegionKey key{-1, 0, 0, 0};
    auto data = perlin.Generate(key, lod);
    const auto step = 1u << lod;
    const auto origin = WorldTerrain::origin(key);
    size_t occupied = 0, empty = 0;
    for (uint32_t z = 0; z < 256; z += step)
      for (uint32_t y = 0; y < 256; y += step)
        for (uint32_t x = 0; x < 256; x += step)
        {
          const WorldPosition center{
              (double(origin[0]) + x + step * 0.5) * WorldTerrain::voxel_width,
              (double(origin[1]) + y + step * 0.5) * WorldTerrain::voxel_width,
              (double(origin[2]) + z + step * 0.5) * WorldTerrain::voxel_width};
          const auto handle = data->find({x, y, z});
          const auto density = handle ? data->cell(handle).density : 0;
          assert(density == 0 || density == 1);
          assert((density > 0) == (perlin.Density(center) > 0));
          if (density > 0)
            ++occupied;
          else
            ++empty;
        }
    assert(occupied > 0 && empty > 0);
    WorldTerrain world;
    world.SetGeneratedSource(key, lod, std::move(data));
    const auto mesh = TerrainMesher::Build(world, key, lod);
    BlockGeometry(mesh, step * WorldTerrain::voxel_width);
    // Capturing for the worker preserves the exact same block geometry.
    const auto snapshot = TerrainMeshInput::Capture(world, key, lod);
    const auto workerMesh = TerrainMesher::Build(snapshot);
    assert(workerMesh.indices == mesh.indices);
    assert(workerMesh.vertices.size() == mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i)
    {
      assert(workerMesh.vertices[i].position == mesh.vertices[i].position);
      assert(workerMesh.vertices[i].normal == mesh.vertices[i].normal);
    }
  }
  // Finest generation must fit the streaming budget and actually retain
  // 0.125 m cells at the surface, while compressing uniform solid interiors.
  auto budget = std::make_shared<TerrainMemoryBudget>(256u * 1024 * 1024);
  auto finest = perlin.Generate({}, 0, budget);
  size_t finestCells = 0;
  finest->visit(
      [&](const TerrainRegion::Tree::Entry& entry)
      {
        if (entry.terminal && entry.region.depth == 8)
          ++finestCells;
      });
  assert(finestCells > 0 && budget->Used() < budget->Limit());
  uint32_t random = 12345;
  for (size_t i = 0; i < 8192; ++i)
  {
    TerrainRegion::Tree::Point local;
    for (auto& axis : local)
    {
      random = random * 1664525u + 1013904223u;
      axis = (random >> 16) & 255;
    }
    const auto handle = finest->find(local);
    const bool solid = handle && finest->cell(handle).density > 0;
    const WorldPosition center{(local[0] + 0.5) * 0.125,
                               (local[1] + 0.5) * 0.125,
                               (local[2] + 0.5) * 0.125};
    assert(solid == (perlin.Density(center) > 0));
  }
  WorldTerrain fineWorld;
  fineWorld.SetGeneratedSource({}, 0, std::move(finest));
  const auto fineMesh = TerrainMesher::Build(fineWorld, {}, 0);
  BlockGeometry(fineMesh, 0.125);
  bool halfMeterGridBroken = false;
  for (const auto& vertex : fineMesh.vertices)
    for (int axis = 0; axis < 3; ++axis)
      halfMeterGridBroken |=
          std::abs(vertex.position[axis] / 0.25f -
                   std::round(vertex.position[axis] / 0.25f)) > 0.1f;
  assert(halfMeterGridBroken); // geometry includes odd 0.125 m grid coordinates
  std::cout << "Block voxel LOD tests passed\n";
}
