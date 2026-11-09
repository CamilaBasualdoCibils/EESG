#pragma once
#include "TerrainTypes.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace EESG
{
using namespace Lighthouse;
// Access providers publish only completed, CPU-visible bytes. A mapped provider
// must retain its allocation and prevent GPU writes for the view's lifetime.
class ITerrainOctreeMemory
{
public:
  virtual ~ITerrainOctreeMemory() = default;
  virtual std::span<const std::byte> Bytes() const = 0;
  virtual uint64_t Revision() const = 0;
};
class TerrainSnapshotMemory final : public ITerrainOctreeMemory
{
public:
  TerrainSnapshotMemory(std::span<const std::byte> bytes, uint64_t revision)
      : bytes_(bytes.begin(), bytes.end()), revision_(revision)
  {
  }
  std::span<const std::byte> Bytes() const override
  {
    return bytes_;
  }
  uint64_t Revision() const override
  {
    return revision_;
  }

private:
  std::vector<std::byte> bytes_;
  uint64_t revision_;
};
class TerrainMappedMemory final : public ITerrainOctreeMemory
{
public:
  TerrainMappedMemory(std::span<const std::byte> bytes, uint64_t revision,
                      std::shared_ptr<const void> lease)
      : bytes_(bytes), revision_(revision), lease_(std::move(lease))
  {
    if (!lease_)
      throw std::invalid_argument(
          "Mapped terrain requires an allocation lease");
  }
  std::span<const std::byte> Bytes() const override
  {
    return bytes_;
  }
  uint64_t Revision() const override
  {
    return revision_;
  }

private:
  std::span<const std::byte> bytes_;
  uint64_t revision_;
  std::shared_ptr<const void> lease_;
};
// Interprets residency topology only. No voxel samples or geometry exist yet.
class TerrainOctree
{
public:
  explicit TerrainOctree(std::shared_ptr<const ITerrainOctreeMemory> memory)
      : memory_(std::move(memory))
  {
    if (!memory_)
      throw std::invalid_argument("Terrain memory is required");
    const auto h = Header();
    if (h.counts.x > TerrainNodeCapacity || h.counts.y > h.counts.x ||
        h.counts.z > h.counts.x ||
        memory_->Bytes().size() <
            sizeof(TerrainHeader) + size_t(h.counts.x) * sizeof(TerrainNode))
      throw std::invalid_argument("Invalid terrain snapshot counts");
  }
  TerrainHeader Header() const
  {
    return Read<TerrainHeader>(0);
  }
  TerrainNode Node(uint32_t index) const
  {
    if (index >= Header().counts.x)
      throw std::out_of_range("Terrain node index");
    return Read<TerrainNode>(sizeof(TerrainHeader) +
                             size_t(index) * sizeof(TerrainNode));
  }
  uint64_t Revision() const
  {
    return memory_->Revision();
  }

private:
  template <class T> T Read(size_t offset) const
  {
    auto bytes = memory_->Bytes();
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
      throw std::out_of_range("Truncated terrain memory");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
  }
  std::shared_ptr<const ITerrainOctreeMemory> memory_;
};
} // namespace EESG
