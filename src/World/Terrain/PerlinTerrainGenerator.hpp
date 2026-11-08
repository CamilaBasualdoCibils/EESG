#pragma once
#include "World/Terrain/ITerrainGenerator.hpp"
#include <algorithm>

namespace EESG
{
class PerlinTerrainGenerator final : public ITerrainGenerator
{
public:
  explicit PerlinTerrainGenerator(uint32_t seed = 1) : seed_(seed) {}
  double Density(WorldPosition position) const override
  {
    WorldTerrain::voxel_position(
        position); // reject nonfinite/out-of-range input
    // Vertical bias supplies a plane; the 3D field permits overhangs.
    return -position.y + 24.0 * Noise(position * 0.025);
  }

protected:
  std::optional<TerrainVoxel> UniformSample(WorldPosition center,
                                            double halfWidth) const override
  {
    // Each Perlin gradient has two +/-1 components, so |noise| <= 2.
    // Its partial derivatives are bounded by 1 + 4*max(fade') = 8.5.
    // These conservative bounds skip uniform interiors without losing caves
    // or thin surface features, unlike guessing from corner signs.
    if (-center.y - halfWidth - 48.0 > 0)
      return TerrainVoxel{1, 1};
    if (-center.y + halfWidth + 48.0 <= 0)
      return TerrainVoxel{};
    const auto density = Density(center);
    const auto variation = halfWidth * (1.0 + 3.0 * 24.0 * 0.025 * 8.5);
    if (density > variation)
      return TerrainVoxel{1, 1};
    if (density + variation <= 0)
      return TerrainVoxel{};
    return {};
  }

private:
  uint32_t seed_;
  static double Fade(double t)
  {
    return t * t * t * (t * (t * 6 - 15) + 10);
  }
  uint32_t Hash(int64_t x, int64_t y, int64_t z) const
  {
    uint64_t h = seed_;
    for (auto v : {x, y, z})
    {
      h ^= uint64_t(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
      h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
      h ^= h >> 31;
    }
    return uint32_t(h);
  }
  static double Gradient(uint32_t h, double x, double y, double z)
  {
    h &= 15;
    const auto u = h < 8 ? x : y;
    const auto v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
  }
  double Noise(WorldPosition p) const
  {
    const auto x = int64_t(std::floor(p.x)), y = int64_t(std::floor(p.y)),
               z = int64_t(std::floor(p.z));
    p -= WorldPosition(double(x), double(y), double(z));
    double values[2][2];
    for (int dz = 0; dz < 2; ++dz)
      for (int dy = 0; dy < 2; ++dy)
        values[dz][dy] = std::lerp(
            Gradient(Hash(x, y + dy, z + dz), p.x, p.y - dy, p.z - dz),
            Gradient(Hash(x + 1, y + dy, z + dz), p.x - 1, p.y - dy, p.z - dz),
            Fade(p.x));
    return std::lerp(std::lerp(values[0][0], values[0][1], Fade(p.y)),
                     std::lerp(values[1][0], values[1][1], Fade(p.y)),
                     Fade(p.z));
  }
};
} // namespace EESG
