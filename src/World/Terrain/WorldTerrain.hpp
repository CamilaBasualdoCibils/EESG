#pragma once

#include "Lighthouse/Engine/Core/Types.hpp"
#include "Math/Types.hpp"
#include "Types/AdaptiveOctree.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EESG
{

// Density is occupied volume in [0,1]; zero is air. Material IDs are
// world-defined.
struct TerrainVoxel
{
  uint32_t material = 0;
  float density = 0;
  bool operator==(const TerrainVoxel&) const = default;
};

using WorldPosition = Dvec3; // metres
using VoxelPosition =
    Lighthouse::Vec<3, int64_t>; // finest (0.125 m) voxel coordinates

// Coordinates belong to this scale's grid, not the base chunk grid.
struct RegionKey
{
  int64_t x = 0, y = 0, z = 0;
  uint32_t scale = 0;
  bool operator==(const RegionKey&) const = default;
};
struct RegionKeyHash
{
  size_t operator()(const RegionKey& key) const noexcept
  {
    uint64_t hash = 0;
    for (uint64_t v : {uint64_t(key.x), uint64_t(key.y), uint64_t(key.z),
                       uint64_t(key.scale)})
    {
      v += 0x9e3779b97f4a7c15ULL;
      v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
      v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
      hash ^= (v ^ (v >> 31)) + (hash << 6) + (hash >> 2);
    }
    return size_t(hash);
  }
};

struct TerrainEdit
{
  RegionKey chunk;                  // scale must be zero
  std::array<uint32_t, 3> origin{}; // local finest voxels, aligned to depth
  uint32_t depth = 8; // 0 = whole 32 m chunk; 8 = one 0.125 m voxel
  TerrainVoxel value;
};

class WorldTerrain;
class TerrainRegion
{
public:
  using Tree = AdaptiveOctree<TerrainVoxel>;
  RegionKey key() const noexcept
  {
    return key_;
  }
  uint32_t detail_depth() const noexcept
  {
    return detailDepth_;
  }
  const Tree& data() const noexcept
  {
    return data_;
  }

private:
  friend class WorldTerrain;
  TerrainRegion(RegionKey key, uint32_t depth)
      : key_(key), detailDepth_(depth), data_(8)
  {
  }
  RegionKey key_;
  uint32_t detailDepth_;
  Tree data_;
};

// One instance owns one world's terrain. No renderer, mesher, or streaming
// policy. Missing authoritative terrain is air. Loaded representations are
// disposable caches, derived from authoritative base-chunk octrees on demand.
class WorldTerrain
{
public:
  static constexpr double voxel_width = 0.125;
  static constexpr double chunk_width = 32.0;
  static constexpr uint32_t chunk_depth = 8;
  static constexpr int64_t chunk_voxels = 256;
  // Largest region width fits signed 64-bit finest-voxel coordinates.
  static constexpr uint32_t maximum_scale = 54;

  WorldTerrain() = default;
  WorldTerrain(const WorldTerrain&) = delete;
  WorldTerrain& operator=(const WorldTerrain&) = delete;

  static int64_t floor_divide(int64_t value, int64_t divisor)
  {
    if (divisor <= 0)
      throw std::invalid_argument("Positive terrain divisor required");
    const auto quotient = value / divisor;
    return quotient - (value % divisor < 0);
  }
  static int64_t region_voxels(uint32_t scale)
  {
    if (scale > maximum_scale)
      throw std::out_of_range("Terrain region scale");
    return int64_t{1} << (chunk_depth + scale);
  }
  static double region_width(uint32_t scale)
  {
    return double(region_voxels(scale)) * voxel_width;
  }
  static void validate(RegionKey key)
  {
    const auto width = region_voxels(key.scale);
    const auto low = std::numeric_limits<int64_t>::min() / width;
    const auto high = std::numeric_limits<int64_t>::max() / width;
    for (auto axis : {key.x, key.y, key.z})
      if (axis < low || axis > high)
        throw std::out_of_range("Terrain region coordinates");
  }
  static VoxelPosition origin(RegionKey key)
  {
    validate(key);
    const auto width = region_voxels(key.scale);
    return {key.x * width, key.y * width, key.z * width};
  }
  static VoxelPosition voxel_position(WorldPosition position)
  {
    VoxelPosition result;
    // Compare to an exclusive upper bound, avoiding rounding INT64_MAX to 2^63.
    const long double limit = std::ldexp(1.0L, 63);
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
      const auto value =
          std::floor(static_cast<long double>(position[axis]) / voxel_width);
      if (!std::isfinite(value) || value < -limit || value >= limit)
        throw std::out_of_range("Terrain world position");
      result[axis] = static_cast<int64_t>(value);
    }
    return result;
  }
  static RegionKey key_at_voxel(VoxelPosition position, uint32_t scale = 0)
  {
    const auto width = region_voxels(scale);
    return {floor_divide(position[0], width), floor_divide(position[1], width),
            floor_divide(position[2], width), scale};
  }
  static RegionKey key_at(WorldPosition position, uint32_t scale = 0)
  {
    return key_at_voxel(voxel_position(position), scale);
  }
  static RegionKey parent(RegionKey key)
  {
    validate(key);
    if (key.scale == maximum_scale)
      throw std::out_of_range("Maximum terrain scale");
    return {floor_divide(key.x, 2), floor_divide(key.y, 2),
            floor_divide(key.z, 2), key.scale + 1};
  }
  static std::array<RegionKey, 8> children(RegionKey key)
  {
    validate(key);
    if (!key.scale)
      throw std::out_of_range("Base chunk has no child chunks");
    std::array<RegionKey, 8> result;
    for (uint32_t i = 0; i < 8; ++i)
      result[i] = {key.x * 2 + (i & 1), key.y * 2 + ((i >> 1) & 1),
                   key.z * 2 + ((i >> 2) & 1), key.scale - 1};
    return result;
  }

  // Authoritative queries never depend on which LODs are loaded.
  TerrainVoxel sample(WorldPosition position) const
  {
    return sample_voxel(voxel_position(position));
  }
  TerrainVoxel sample_voxel(VoxelPosition position) const
  {
    const auto key = key_at_voxel(position);
    const auto it = chunks_.find(key);
    if (it == chunks_.end())
      return {};
    const auto base = origin(key);
    TerrainRegion::Tree::Point local;
    for (uint32_t axis = 0; axis < 3; ++axis)
      local[axis] = uint32_t(position[axis] - base[axis]);
    const auto leaf = it->second->find(local);
    return leaf ? it->second->cell(leaf) : TerrainVoxel{};
  }
  void set_voxel(VoxelPosition position, TerrainVoxel value)
  {
    const auto key = key_at_voxel(position);
    const auto base = origin(key);
    TerrainEdit edit{key, {}, chunk_depth, value};
    for (uint32_t axis = 0; axis < 3; ++axis)
      edit.origin[axis] = uint32_t(position[axis] - base[axis]);
    apply(edit);
  }
  void apply(const TerrainEdit& edit)
  {
    validate(edit.chunk);
    if (edit.chunk.scale || edit.depth > chunk_depth)
      throw std::out_of_range("Terrain edit must address a base chunk");
    const auto width = uint32_t{1} << (chunk_depth - edit.depth);
    for (auto axis : edit.origin)
      if (axis >= chunk_voxels || axis % width)
        throw std::out_of_range("Unaligned terrain edit");
    if (!std::isfinite(edit.value.density) || edit.value.density < 0 ||
        edit.value.density > 1)
      throw std::invalid_argument("Terrain density must be in [0,1]");
    auto it = chunks_.find(edit.chunk);
    if (it == chunks_.end())
    {
      if (edit.value.density == 0)
        return;
      auto data = std::make_unique<TerrainRegion::Tree>(chunk_depth);
      data->paint({edit.origin, edit.depth}, edit.value);
      chunks_.emplace(edit.chunk, std::move(data));
    }
    else
    {
      if (edit.value.density == 0)
        it->second->erase({edit.origin, edit.depth});
      else
        it->second->paint({edit.origin, edit.depth}, edit.value);
      if (it->second->empty())
        chunks_.erase(it);
    }
    ++revision_;
    // Every loaded ancestor is a derived representation of the edited chunk.
    // Invalidating removes it immediately; there are no stale sample results.
    for (auto cache = loaded_.begin(); cache != loaded_.end();)
    {
      if (encloses(cache->first, edit.chunk))
        cache = loaded_.erase(cache);
      else
        ++cache;
    }
  }

  // A region's depth cap is independent of its spatial scale. These const
  // views remain valid until unload, replacement, clear, or an edit inside it.
  const TerrainRegion* find(RegionKey key) const
  {
    validate(key);
    const auto it = loaded_.find(key);
    return it == loaded_.end() ? nullptr : it->second.get();
  }
  const TerrainRegion& ensure(RegionKey key, uint32_t detailDepth = chunk_depth)
  {
    validate(key);
    if (detailDepth > chunk_depth)
      throw std::out_of_range("Terrain detail depth");
    auto it = loaded_.find(key);
    if (it != loaded_.end() && it->second->detail_depth() == detailDepth)
      return *it->second;
    auto region = build(key, detailDepth);
    auto* result = region.get();
    loaded_.insert_or_assign(key, std::move(region));
    return *result;
  }
  bool unload(RegionKey key)
  {
    validate(key);
    return loaded_.erase(key) != 0;
  }
  // Replace a loaded parent representation by its eight children. Source
  // terrain remains intact. A base chunk refines internally through ensure().
  std::array<RegionKey, 8> subdivide(RegionKey key,
                                     uint32_t detailDepth = chunk_depth)
  {
    const auto keys = children(key);
    for (auto child : keys)
      ensure(child, detailDepth);
    unload(key);
    return keys;
  }
  // Build a parent representation before unloading its cached descendants.
  // Authoritative fine terrain is retained, so later refinement is lossless.
  const TerrainRegion& collapse(RegionKey key, uint32_t detailDepth = 0)
  {
    ensure(key, detailDepth);
    for (auto it = loaded_.begin(); it != loaded_.end();)
    {
      if (it->first != key && encloses(key, it->first))
        it = loaded_.erase(it);
      else
        ++it;
    }
    return *find(key);
  }
  std::vector<RegionKey> source_keys() const
  {
    std::vector<RegionKey> keys;
    for (const auto& [key, data] : chunks_) keys.push_back(key);
    return keys;
  }
  uint64_t revision() const noexcept { return revision_; }
  const TerrainRegion::Tree* source(RegionKey key) const
  {
    validate(key);
    const auto it = chunks_.find(key);
    return it == chunks_.end() ? nullptr : it->second.get();
  }
  size_t source_chunk_count() const noexcept
  {
    return chunks_.size();
  }
  size_t loaded_region_count() const noexcept
  {
    return loaded_.size();
  }
  void clear() noexcept
  {
    ++revision_;
    loaded_.clear();
    chunks_.clear();
  }

private:
  uint64_t revision_ = 0;
  using SourceMap =
      std::unordered_map<RegionKey, std::unique_ptr<TerrainRegion::Tree>,
                         RegionKeyHash>;
  SourceMap chunks_;
  std::unordered_map<RegionKey, std::unique_ptr<TerrainRegion>, RegionKeyHash>
      loaded_;

  static bool encloses(RegionKey outer, RegionKey inner)
  {
    if (outer.scale < inner.scale)
      return false;
    const auto divisor = int64_t{1} << (outer.scale - inner.scale);
    return floor_divide(inner.x, divisor) == outer.x &&
           floor_divide(inner.y, divisor) == outer.y &&
           floor_divide(inner.z, divisor) == outer.z;
  }
  struct Accumulator
  {
    long double density = 0;
    std::unordered_map<uint32_t, long double> materials;
  };
  std::unique_ptr<TerrainRegion> build(RegionKey key,
                                       uint32_t detailDepth) const
  {
    auto region =
        std::unique_ptr<TerrainRegion>(new TerrainRegion(key, detailDepth));
    const auto regionOrigin = origin(key);
    const auto targetPower = chunk_depth + key.scale - detailDepth;
    const auto localStep = uint32_t{1} << (chunk_depth - detailDepth);
    std::unordered_map<uint32_t, Accumulator> accumulated;
    for (const auto& [chunk, data] : chunks_)
    {
      if (!encloses(key, chunk))
        continue;
      const auto chunkOrigin = origin(chunk);
      data->visit(
          [&](const TerrainRegion::Tree::Entry& entry)
          {
            if (!entry.terminal)
              return;
            const auto value = data->cell(entry.handle);
            if (value.density == 0)
              return;
            std::array<uint64_t, 3> relative;
            for (uint32_t axis = 0; axis < 3; ++axis)
              relative[axis] =
                  uint64_t(chunkOrigin[axis] - regionOrigin[axis]) +
                  entry.region.origin[axis];
            const auto sourcePower = chunk_depth - entry.region.depth;
            if (sourcePower >= targetPower)
            {
              // This whole source leaf fits the requested cap. Preserve its
              // adaptive extent instead of expanding it into thousands of
              // uniform cells.
              TerrainRegion::Tree::Point local;
              for (uint32_t axis = 0; axis < 3; ++axis)
                local[axis] = uint32_t(relative[axis] >> key.scale);
              region->data_.paint({local, key.scale + entry.region.depth},
                                  value);
            }
            else
            {
              const auto x = uint32_t(relative[0] >> targetPower);
              const auto y = uint32_t(relative[1] >> targetPower);
              const auto z = uint32_t(relative[2] >> targetPower);
              auto& sum = accumulated[x | (y << 8) | (z << 16)];
              const auto weight =
                  std::ldexp(1.0L, -3 * int(targetPower - sourcePower));
              const auto mass = weight * value.density;
              sum.density += mass;
              sum.materials[value.material] += mass;
            }
          });
    }
    for (const auto& [packed, sum] : accumulated)
    {
      uint32_t material = 0;
      long double greatest = -1;
      for (const auto& [id, mass] : sum.materials)
        if (mass > greatest || (mass == greatest && id < material))
        {
          material = id;
          greatest = mass;
        }
      TerrainRegion::Tree::Point local{(packed & 255u) * localStep,
                                       ((packed >> 8) & 255u) * localStep,
                                       ((packed >> 16) & 255u) * localStep};
      region->data_.paint({local, detailDepth}, {material, float(sum.density)});
    }
    return region;
  }
};
} // namespace EESG
