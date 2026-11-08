#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace EESG
{
// Byte ranges inside a single persistent vertex/index buffer. Aligning range
// starts to vertex stride lets baseVertex address vertices without rebinding
// an offset; firstIndex addresses the index section of the same buffer.
class TerrainMeshArena
{
public:
  struct Range
  {
    size_t offset = 0, size = 0;
  };
  explicit TerrainMeshArena(size_t capacity)
      : capacity_(capacity), free_{{0, capacity}}
  {
  }
  std::optional<Range> Allocate(size_t bytes, size_t alignment)
  {
    if (!bytes || !alignment || bytes > capacity_)
      return {};
    for (auto it = free_.begin(); it != free_.end(); ++it)
    {
      const size_t padding = (alignment - it->offset % alignment) % alignment;
      if (padding > it->size || bytes > it->size - padding)
        continue;
      const Range range{it->offset + padding, bytes};
      const Range tail{range.offset + bytes, it->size - padding - bytes};
      if (padding)
      {
        it->size = padding;
        if (tail.size)
          free_.insert(it + 1, tail);
      }
      else if (tail.size)
        *it = tail;
      else
        free_.erase(it);
      used_ += bytes;
      return range;
    }
    return {};
  }
  void Free(Range range)
  {
    if (!range.size)
      return;
    used_ -= range.size;
    auto it =
        free_.insert(std::lower_bound(free_.begin(), free_.end(), range.offset,
                                      [](auto entry, size_t offset)
                                      { return entry.offset < offset; }),
                     range);
    if (it != free_.begin() && (it - 1)->offset + (it - 1)->size == it->offset)
    {
      (it - 1)->size += it->size;
      it = free_.erase(it) - 1;
    }
    if (it + 1 != free_.end() && it->offset + it->size == (it + 1)->offset)
    {
      it->size += (it + 1)->size;
      free_.erase(it + 1);
    }
  }
  size_t Used() const
  {
    return used_;
  }
  size_t Capacity() const
  {
    return capacity_;
  }

private:
  size_t capacity_, used_ = 0;
  std::vector<Range> free_;
};
} // namespace EESG
