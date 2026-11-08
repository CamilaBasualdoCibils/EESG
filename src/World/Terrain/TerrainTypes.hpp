#pragma once
#include "Math/Types.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace EESG
{
// Change this constant to change the number of hierarchy levels above chunks.
inline constexpr uint32_t TerrainMetaChunkLevels = 16;
static_assert(TerrainMetaChunkLevels <= 16);
inline constexpr uint32_t TerrainChunkDepth = 8;
inline constexpr double TerrainVoxelWidth = 0.125;
inline constexpr double TerrainChunkWidth = 32.0;
inline constexpr uint32_t TerrainNodeCapacity = 65536;
inline constexpr uint32_t InvalidTerrainNode =
    std::numeric_limits<uint32_t>::max();
using WorldPosition = Dvec3;

// std430 layout shared with the compute shaders. Coordinates are in chunks;
// a level-L meta-chunk spans 2^L regular chunks along each axis.
struct alignas(16) TerrainNode
{
  IVec4 region{}; // xyz minimum, w level (0 = regular chunk)
  UVec4 state{InvalidTerrainNode, InvalidTerrainNode, 0,
              0}; // first child, parent, effective cell LOD, flags
};
// Effective cell LOD includes the node's meta level. Cell edge is
// TerrainVoxelWidth * 2^LOD; each node has TerrainChunkDepth internal depth.
inline constexpr uint32_t TerrainLeaf = 1, TerrainBranch = 2;
static_assert(TerrainMetaChunkLevels + TerrainChunkDepth < 32);
struct TerrainRootCoverage
{
  // Maximum-level root-grid coordinates, independent of observers and detail.
  IVec3 minimum{-1};
  UVec3 count{
      2}; // Default finite domain: eight roots surrounding world origin.
};
struct alignas(16) TerrainHeader
{
  UVec4
      counts{}; // allocated nodes, roots, selected leaves (any level), overflow
  std::array<UVec4, TerrainMetaChunkLevels + 1>
      levels{}; // visited,rejected,split,selected
};
static_assert(sizeof(TerrainNode) == 32);
static_assert(sizeof(TerrainHeader) == 16 * (TerrainMetaChunkLevels + 2));
inline constexpr uint64_t TerrainBufferBytes =
    sizeof(TerrainHeader) + uint64_t(TerrainNodeCapacity) * sizeof(TerrainNode);
} // namespace EESG
