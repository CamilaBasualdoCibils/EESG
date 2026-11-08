#pragma once

#include "World/Terrain/WorldTerrain.hpp"
#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <algorithm>

namespace EESG
{
// Immutable shared source chunk and its six face neighbors. Captured on
// the world thread; workers never read live terrain or its mutable allocator.
struct TerrainMeshInput
{
  using Tree = TerrainRegion::Tree;
  struct Chunk
  {
    std::shared_ptr<const Tree> owner;
    std::span<const Tree::GpuNode> nodes;
    std::span<const TerrainVoxel> cells;
    uint32_t root = Tree::null;
    TerrainVoxel Sample(std::array<int, 3> local) const
    {
      auto index = root;
      for (int bit = 7; index != Tree::null; --bit)
      {
        const auto& node = nodes[index];
        if (node.tags & Tree::terminalBit)
          return cells[node.payload];
        const auto child = ((local[0] >> bit) & 1) |
                           (((local[1] >> bit) & 1) << 1) |
                           (((local[2] >> bit) & 1) << 2);
        if (!(node.childBits & (1u << child)))
          return {};
        index = node.childBase + child;
      }
      return {};
    }
  };
  struct Leaf
  {
    Tree::Region region;
    TerrainVoxel value;
  };
  RegionKey key;
  uint32_t lod = 0;
  // Center, -X, +X, -Y, +Y, -Z, +Z.
  std::array<Chunk, 7> chunks;
  std::vector<Leaf> leaves;
  static RegionKey Neighbor(RegionKey key, size_t i)
  {
    if (!i)
      return key;
    auto* coordinate = i <= 2 ? &key.x : i <= 4 ? &key.y : &key.z;
    *coordinate += i % 2 ? -1 : 1;
    return key;
  }
  static bool ValidNeighbor(RegionKey key)
  {
    constexpr auto low =
        std::numeric_limits<int64_t>::min() / WorldTerrain::chunk_voxels;
    constexpr auto high =
        std::numeric_limits<int64_t>::max() / WorldTerrain::chunk_voxels;
    return key.x >= low && key.x <= high && key.y >= low && key.y <= high &&
           key.z >= low && key.z <= high;
  }
  static TerrainMeshInput Capture(const WorldTerrain& terrain, RegionKey key,
                                  uint32_t lod)
  {
    WorldTerrain::validate(key);
    if (key.scale || lod > WorldTerrain::chunk_depth)
      throw std::invalid_argument("Meshing requires a base chunk and LOD 0..8");
    TerrainMeshInput input;
    input.key = key;
    input.lod = lod;
    for (size_t i = 0; i < input.chunks.size(); ++i)
    {
      const auto neighbor = Neighbor(key, i);
      if (!ValidNeighbor(neighbor))
        continue;
      auto& copy = input.chunks[i];
      copy.owner = terrain.share_source(neighbor);
      if (!copy.owner)
        continue;
      const auto buffers = copy.owner->upload();
      copy.nodes = buffers.topology;
      copy.cells = buffers.cells;
      copy.root = buffers.root;
    }
    return input;
  }
  bool Solid(std::array<int, 3> local) const
  {
    size_t chunk = 0;
    for (size_t axis = 0; axis < 3; ++axis)
    {
      if (local[axis] < 0)
      {
        chunk = 1 + axis * 2;
        local[axis] += 256;
      }
      else if (local[axis] >= 256)
      {
        chunk = 2 + axis * 2;
        local[axis] -= 256;
      }
    }
    return chunks[chunk].Sample(local).density > 0;
  }
};

// Axis-aligned cube faces only: LOD n uses cells 2^n finest voxels wide.
// Greedy rectangles on each adaptive leaf's exposed faces. Uniform leaf
// interiors are never scanned, and adjacent visible squares become one quad.
class TerrainMesher
{
public:
  static Lighthouse::Mesh3D Build(const WorldTerrain& terrain, RegionKey key,
                                  uint32_t lod = 0)
  {
    return Build(TerrainMeshInput::Capture(terrain, key, lod));
  }
  static Lighthouse::Mesh3D Build(const TerrainMeshInput& input)
  {
    Lighthouse::Mesh3D mesh;
    const auto origin = WorldTerrain::origin(input.key);
    std::vector<uint8_t> mask;
    auto append = [&](const TerrainMeshInput::Leaf& leaf)
    {
      const auto width = 1u << (WorldTerrain::chunk_depth - leaf.region.depth);
      const auto step = std::min(width, 1u << input.lod);
      const auto size = width / step;
      for (int axis = 0; axis < 3; ++axis)
        for (int sign : {-1, 1})
        {
          const int u = (axis + 1) % 3, v = (axis + 2) % 3;
          mask.assign(size * size, 0);
          for (uint32_t b = 0; b < size; ++b)
            for (uint32_t a = 0; a < size; ++a)
            {
              std::array<int, 3> local;
              for (int d = 0; d < 3; ++d)
                local[d] = int(leaf.region.origin[d] + step / 2);
              // Test the voxel touching this face, even when adjacent chunks
              // use different LODs. Interior edits cannot occlude a boundary.
              local[axis] =
                  int(leaf.region.origin[axis]) + (sign > 0 ? int(width) : -1);
              local[u] += a * step;
              local[v] += b * step;
              mask[b * size + a] = !input.Solid(local);
            }
          for (uint32_t b = 0; b < size; ++b)
            for (uint32_t a = 0; a < size;)
            {
              if (!mask[b * size + a])
              {
                ++a;
                continue;
              }
              uint32_t w = 1, h = 1;
              while (a + w < size && mask[b * size + a + w])
                ++w;
              while (b + h < size)
              {
                bool full = true;
                for (uint32_t x = 0; x < w; ++x)
                  if (!mask[(b + h) * size + a + x])
                  {
                    full = false;
                    break;
                  }
                if (!full)
                  break;
                ++h;
              }
              for (uint32_t y = 0; y < h; ++y)
                std::fill_n(mask.begin() + (b + y) * size + a, w, uint8_t{0});
              Vec3 base;
              for (int d = 0; d < 3; ++d)
                base[d] = float((double(origin[d]) + leaf.region.origin[d]) *
                                WorldTerrain::voxel_width);
              base[axis] +=
                  sign > 0 ? float(width * WorldTerrain::voxel_width) : 0;
              base[u] += float(a * step * WorldTerrain::voxel_width);
              base[v] += float(b * step * WorldTerrain::voxel_width);
              Vec3 du(0), dv(0), normal(0);
              du[u] = float(w * step * WorldTerrain::voxel_width);
              dv[v] = float(h * step * WorldTerrain::voxel_width);
              normal[axis] = float(sign);
              if (sign < 0)
                std::swap(du, dv);
              // Bound each worker's output even for pathological fine terrain.
              constexpr size_t maximumMeshBytes = 64u * 1024 * 1024;
              if ((mesh.vertices.size() + 4) *
                          sizeof(Lighthouse::Mesh3DVertex) +
                      (mesh.indices.size() + 6) * sizeof(uint32_t) >
                  maximumMeshBytes)
                throw std::length_error(
                    "Terrain chunk mesh exceeds 64 MiB; use a coarser LOD");
              const auto first = uint32_t(mesh.vertices.size());
              for (auto position : {base, base + du, base + du + dv, base + dv})
                mesh.vertices.push_back({position, normal, {}});
              for (auto i : {0u, 1u, 2u, 0u, 2u, 3u})
                mesh.indices.push_back(first + i);
              a += w;
            }
        }
    };
    if (const auto& tree = input.chunks[0].owner)
      tree->visit(
          [&](const TerrainRegion::Tree::Entry& entry)
          {
            if (entry.terminal && tree->cell(entry.handle).density > 0)
              append({entry.region, tree->cell(entry.handle)});
          });
    return mesh;
  }
};
} // namespace EESG
