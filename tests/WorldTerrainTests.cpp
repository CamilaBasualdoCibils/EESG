#include "World/Terrain/WorldTerrain.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <random>

using namespace EESG;
static TerrainVoxel cached(const TerrainRegion& region,
                           std::array<uint32_t, 3> local)
{
  const auto h = region.data().find(local);
  return h ? region.data().cell(h) : TerrainVoxel{};
}
template <class F> static void rejects(F f)
{
  bool rejected = false;
  try
  {
    f();
  }
  catch (const std::exception&)
  {
    rejected = true;
  }
  assert(rejected);
}
int main()
{
  WorldTerrain world, other;
  assert(world.source_chunk_count() == 0);
  assert(world.sample({0, 0, 0}) == TerrainVoxel{});
  assert(WorldTerrain::region_width(0) == 32);
  assert(WorldTerrain::region_width(2) == 128);
  assert((WorldTerrain::key_at({-0.01, -32.0, -32.01}) ==
          RegionKey{-1, -1, -2, 0}));
  assert((WorldTerrain::parent({-1, -2, -3, 0}) == RegionKey{-1, -1, -2, 1}));
  for (auto key : WorldTerrain::children({-1, 0, 1, 2}))
    assert((WorldTerrain::parent(key) == RegionKey{-1, 0, 1, 2}));
  world.set_voxel({-1, 0, 0}, {5, 1});
  assert((world.sample({-0.01, 0, 0}) == TerrainVoxel{5, 1}));
  assert(world.sample({0, 0, 0}) == TerrainVoxel{});
  assert(other.sample({-0.01, 0, 0}) == TerrainVoxel{});
  assert(cached(world.ensure({-1, 0, 0, 0}), {255, 0, 0}).material == 5);
  world.apply({{0, 0, 0, 0}, {0, 0, 0}, 0, {7, 1}});
  assert((world.sample({31.999, 31.999, 31.999}) == TerrainVoxel{7, 1}));
  assert(world.sample({32, 0, 0}) == TerrainVoxel{});
  world.ensure({0, 0, 0, 0});
  world.ensure({0, 0, 0, 1}, 0);
  world.ensure({4, 0, 0, 0});
  world.set_voxel({0, 0, 0}, {8, 0.5f});
  assert(!world.find({0, 0, 0, 0}));
  assert(!world.find({0, 0, 0, 1}));
  assert(world.find({4, 0, 0, 0}));
  assert((world.sample_voxel({1, 0, 0}) == TerrainVoxel{7, 1}));
  world.set_voxel({0, 0, 0}, {});
  assert(world.sample_voxel({0, 0, 0}) == TerrainVoxel{});
  assert((world.sample_voxel({1, 0, 0}) == TerrainVoxel{7, 1}));
  world.clear();
  // One full chunk in a 2x2x2 meta region contributes 1/8 occupied volume.
  world.apply({{0, 0, 0, 0}, {0, 0, 0}, 0, {3, 1}});
  const auto& meta = world.ensure({0, 0, 0, 1}, 0);
  assert((cached(meta, {0, 0, 0}) == TerrainVoxel{3, 0.125f}));
  assert(meta.data().terminal_count() == 1);
  assert(meta.detail_depth() == 0);
  auto& detailed = world.ensure({0, 0, 0, 1}, 8);
  assert(detailed.data().terminal_count() ==
         1); // uniform region stays adaptive
  assert((cached(detailed, {127, 127, 127}) == TerrainVoxel{3, 1}));
  assert(cached(detailed, {128, 0, 0}) == TerrainVoxel{});
  // Material selection uses occupied mass; equal masses choose lowest ID.
  world.apply({{1, 0, 0, 0}, {0, 0, 0}, 0, {2, 1}});
  assert(cached(world.ensure({0, 0, 0, 1}, 0), {0, 0, 0}).material == 2);
  const auto keys = world.subdivide({0, 0, 0, 1}, 0);
  assert(!world.find({0, 0, 0, 1}));
  for (auto key : keys)
    assert(world.find(key));
  world.collapse({0, 0, 0, 1});
  for (auto key : keys)
    assert(!world.find(key));
  assert(world.source_chunk_count() == 2);
  assert(world.sample_voxel({0, 0, 0}).material == 3);
  world.unload({0, 0, 0, 1});
  assert(world.sample_voxel({0, 0, 0}).material == 3);
  assert((cached(world.ensure({0, 0, 0, 0}), {0, 0, 0}) == TerrainVoxel{3, 1}));
  // Dense reference verifies weighted reduction and signed chunk boundaries.
  world.clear();
  std::mt19937 rng(456);
  std::array<TerrainVoxel, 16 * 16 * 16> dense{};
  auto index = [](int x, int y, int z)
  { return (x + 8) * 256 + (y + 8) * 16 + z + 8; };
  for (int n = 0; n < 1500; ++n)
  {
    int x = int(rng() % 16) - 8, y = int(rng() % 16) - 8,
        z = int(rng() % 16) - 8;
    TerrainVoxel value{uint32_t(rng() % 4 + 1), float(rng() % 5) / 4};
    if (value.density == 0)
      value = {};
    world.set_voxel({x, y, z}, value);
    dense[index(x, y, z)] = value;
  }
  for (int x = -8; x < 8; ++x)
    for (int y = -8; y < 8; ++y)
      for (int z = -8; z < 8; ++z)
        assert(world.sample_voxel({x, y, z}) == dense[index(x, y, z)]);
  for (int cx = -1; cx <= 0; ++cx)
    for (int cy = -1; cy <= 0; ++cy)
      for (int cz = -1; cz <= 0; ++cz)
      {
        const auto& region =
            world.ensure({cx, cy, cz, 0}, 6); // leaf extent = 4 voxels
        for (int x = -8; x < 8; x += 4)
          for (int y = -8; y < 8; y += 4)
            for (int z = -8; z < 8; z += 4)
            {
              if (WorldTerrain::key_at_voxel({x, y, z}) !=
                  RegionKey{cx, cy, cz, 0})
                continue;
              double density = 0;
              std::array<double, 5> mass{};
              for (int dx = 0; dx < 4; ++dx)
                for (int dy = 0; dy < 4; ++dy)
                  for (int dz = 0; dz < 4; ++dz)
                  {
                    const auto value = dense[index(x + dx, y + dy, z + dz)];
                    density += value.density / 64;
                    mass[value.material] += value.density / 64;
                  }
              std::array<uint32_t, 3> local{uint32_t(x - cx * 256),
                                            uint32_t(y - cy * 256),
                                            uint32_t(z - cz * 256)};
              const auto value = cached(region, local);
              assert(value.density == float(density));
              uint32_t winner = 0;
              for (uint32_t m = 1; m < 5; ++m)
                if (mass[m] > mass[winner])
                  winner = m;
              assert(value.material == winner);
            }
      }
  world.clear();
  for (const auto coordinate : {std::numeric_limits<int64_t>::min(),
                                std::numeric_limits<int64_t>::max()})
  {
    world.set_voxel({coordinate, coordinate, coordinate}, {9, 1});
    assert(world.sample_voxel({coordinate, coordinate, coordinate}).material ==
           9);
    world.ensure(
        WorldTerrain::key_at_voxel({coordinate, coordinate, coordinate}, 54),
        0);
    world.set_voxel({coordinate, coordinate, coordinate}, {});
  }
  assert(world.source_chunk_count() == 0);
  rejects([&] { world.ensure({0, 0, 0, 55}); });
  rejects([&] { world.ensure({0, 0, 0, 0}, 9); });
  rejects([&]
          { world.ensure({std::numeric_limits<int64_t>::max(), 0, 0, 0}); });
  rejects([&] { world.apply({{0, 0, 0, 1}, {}, 0, {1, 1}}); });
  rejects([&] { world.apply({{0, 0, 0, 0}, {1, 0, 0}, 0, {1, 1}}); });
  rejects([&] { world.set_voxel({0, 0, 0}, {1, std::nanf("")}); });
  rejects([&]
          { world.sample({std::numeric_limits<double>::infinity(), 0, 0}); });
  rejects([&] { WorldTerrain::children({0, 0, 0, 0}); });
  rejects([&] { WorldTerrain::parent({0, 0, 0, 54}); });
  std::cout << "WorldTerrain tests passed\n";
}
