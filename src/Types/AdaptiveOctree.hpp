#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <memory>
#include <memory_resource>

namespace EESG {

// No payload buffer is allocated for this type.
struct NoOctreePayload {};

// A terrain region is either absent, a uniform terminal region, or a branch.
// The topology is independent of the terrain representation: a terminal payload
// may hold density/material values or an index into an application brick
// buffer.
template <class Cell, class Summary = NoOctreePayload> class AdaptiveOctree {
  template <class T>
  static constexpr bool validPayload =
      std::is_trivially_copyable_v<T> &&
      std::is_nothrow_default_constructible_v<T> &&
      std::is_nothrow_copy_constructible_v<T> &&
      std::is_nothrow_copy_assignable_v<T>;
  static_assert(
      validPayload<Cell> && validPayload<Summary>,
      "Terrain payloads must be trivial, with nonthrowing value operations");

public:
  using Point = std::array<uint32_t, 3>;
  static constexpr uint32_t null = std::numeric_limits<uint32_t>::max();
  static constexpr uint32_t terminalBit = 1;
  static constexpr uint32_t occupiedBit = 2;

  // GPU ABI: four uints, a 16-byte structured-buffer element. Child octant k
  // occupies childBase+k when childBits has bit k set. x/y/z use bits 0/1/2.
  // payload addresses cells for terminals, summaries for branches. Slots
  // lacking occupiedBit are allocator holes and must never be traversed.
  struct GpuNode {
    uint32_t childBase = null;
    uint32_t payload = null;
    uint32_t childBits = 0;
    uint32_t tags = 0;
  };
  static_assert(sizeof(GpuNode) == 16 && alignof(GpuNode) == 4 &&
                std::is_trivially_copyable_v<GpuNode> &&
                std::is_standard_layout_v<GpuNode>);
  static_assert(offsetof(GpuNode, payload) == 4 &&
                offsetof(GpuNode, childBits) == 8 &&
                offsetof(GpuNode, tags) == 12);

  // CPU handles detect deletion/reuse. GPU buffers contain only indices.
  struct Handle {
    uint32_t index = null;
    uint64_t version = 0;
    bool operator==(const Handle &) const = default;
    explicit operator bool() const { return index != null; }
  };
  struct Region {
    Point origin{};
    uint32_t depth = 0;
  };
  struct Entry {
    Handle handle;
    Region region;
    uint32_t extent;
    bool terminal;
  };
  struct Buffers {
    std::span<const GpuNode> topology;
    std::span<const Cell> cells;
    std::span<const Summary> summaries;
    uint32_t depth;
    uint32_t root; // null when empty
  };

  // depth is a runtime terrain/chunk choice, not a C++ type parameter.
  // Finest cells have extent 1. A depth-0 tree holds one cell.
  explicit AdaptiveOctree(uint32_t depth,
      std::shared_ptr<std::pmr::memory_resource> resource = {})
      : resource_(std::move(resource)), depth_(depth),
        topology_(Resource()), slots_(Resource()), cells_(Resource()), summaries_(Resource()) {
    if (depth > 31)
      throw std::invalid_argument("Octree depth must be <= 31");
    slots_.resize(1);
    topology_.resize(1);
  }
  // Handles belong to one tree instance. Disallow implicit copying/moving them
  // into another owner; obtain a copied GPU snapshot through upload().
  AdaptiveOctree(const AdaptiveOctree &) = delete;
  AdaptiveOctree &operator=(const AdaptiveOctree &) = delete;
  AdaptiveOctree(AdaptiveOctree &&) = delete;
  AdaptiveOctree &operator=(AdaptiveOctree &&) = delete;

  std::shared_ptr<std::pmr::memory_resource> memory_resource() const noexcept
  {
    return resource_;
  }
  uint32_t depth() const noexcept { return depth_; }
  uint32_t extent() const noexcept { return uint32_t{1} << depth_; }
  bool empty() const noexcept { return !live(0); }
  size_t terminal_count() const noexcept { return terminalCount_; }
  Handle root() const noexcept { return empty() ? Handle{} : handle(0); }
  bool contains(Handle h) const noexcept {
    return h.index < slots_.size() && live(h.index) &&
           h.version == slots_[h.index].version;
  }
  bool is_terminal(Handle h) const { return terminal(require(h)); }

  // Borrowed spans: any edit may invalidate them. Upload all spans without
  // packing out holes; all three must describe the same edit revision. The
  // caller owns shader layout for Cell and Summary and upload synchronization.
  Buffers upload() const noexcept {
    return {topology_, cells_.buffer(), summaries_.buffer(), depth_,
            empty() ? null : 0};
  }

  // Spatial query returns the covering terminal, at whatever depth it exists.
  // Out-of-domain points and absent terrain both return an empty handle.
  Handle find(Point p) const noexcept {
    for (auto coordinate : p)
      if (coordinate >= extent())
        return {};
    if (empty())
      return {};
    uint32_t index = 0;
    for (uint32_t level = 0; !terminal(index); ++level) {
      const auto octant = octant_at(p, level);
      if (!(topology_[index].childBits & (1u << octant)))
        return {};
      index = topology_[index].childBase + octant;
    }
    return handle(index);
  }

  const Cell &cell(Handle h) const {
    const auto index = require(h);
    if (!terminal(index))
      throw std::invalid_argument("Expected terminal terrain");
    return cells_.at(topology_[index].payload);
  }
  void set_cell(Handle h, const Cell &value) {
    const auto index = require(h);
    if (!terminal(index))
      throw std::invalid_argument("Expected terminal terrain");
    cells_.at(topology_[index].payload) = value;
  }
  const Summary &summary(Handle h) const
    requires(!std::is_same_v<Summary, NoOctreePayload>)
  {
    const auto index = require(h);
    if (terminal(index))
      throw std::invalid_argument("Expected terrain branch");
    return summaries_.at(topology_[index].payload);
  }
  void set_summary(Handle h, const Summary &value)
    requires(!std::is_same_v<Summary, NoOctreePayload>)
  {
    const auto index = require(h);
    if (terminal(index))
      throw std::invalid_argument("Expected terrain branch");
    summaries_.at(topology_[index].payload) = value;
  }

  // Replace an aligned region, preserving surrounding terrain. Refinement
  // inherits a terminal's value into all eight children. Missing regions stay
  // absent. Ancestor summaries are application-owned, not inferred from Cell.
  Handle paint(Region region, const Cell &value) {
    validate(region);
    const Cell copy = value;
    const auto index = descend(region, true);
    replace(index, copy);
    return handle(index);
  }

  // Explicit LOD editing. Refine preserves this handle and all existing
  // unrelated handles; coarsen preserves the parent handle and invalidates
  // descendants.
  void refine(Handle h) {
    const auto index = require(h);
    if (!terminal(index))
      throw std::invalid_argument("Expected terminal terrain");
    split(index);
  }
  void coarsen(Handle h, const Cell &value) {
    const auto index = require(h);
    const Cell copy = value;
    replace(index, copy);
  }

  // Removing only part of a coarse terminal first refines it. Empty ancestors
  // are pruned. Absent regions report false; stale handles throw on access.
  bool erase(Region region) {
    validate(region);
    const auto index = descend(region, false);
    if (index == null)
      return false;
    const auto parent = slots_[index].parent;
    const auto octant = slots_[index].octant;
    release(index);
    if (parent != null) {
      topology_[parent].childBits &= ~(1u << octant);
      prune(parent);
    }
    return true;
  }
  void clear() noexcept {
    if (!empty())
      release(0);
  }

  // Depth-first traversal with spatial information computed on demand. Callback
  // must not mutate the tree. No Morton codes or positions are stored per node.
  template <class Visitor> void visit(Visitor &&visitor) const {
    if (!empty())
      traverse(0, {}, visitor);
  }

private:
  struct Slot {
    uint64_t version = 0;
    uint32_t parent = null;
    uint32_t depth = 0;
    uint32_t octant = 0;
  };
  template <class T> struct PayloadPool {
    explicit PayloadPool(std::pmr::memory_resource* resource) : values(resource), links(resource) {}
    std::pmr::vector<T> values;
    std::pmr::vector<uint32_t> links;
    uint32_t free = null;
    uint32_t acquire(const T &value) {
      if constexpr (std::is_same_v<T, NoOctreePayload>)
        return null;
      if (free != null) {
        const auto index = free;
        free = links[index];
        values[index] = value;
        return index;
      }
      if (values.size() >= null)
        throw std::length_error("Terrain payload capacity");
      // Reserve before modifying either size, so allocation failure keeps the
      // two arrays synchronized. Geometric growth avoids quadratic appends.
      if (links.size() == links.capacity())
        links.reserve(links.capacity() ? links.capacity() * 2 : 8);
      values.push_back(value);
      links.push_back(null);
      return static_cast<uint32_t>(values.size() - 1);
    }
    void recycle(uint32_t index) noexcept {
      if constexpr (!std::is_same_v<T, NoOctreePayload>) {
        links[index] = free;
        free = index;
      }
    }
    T &at(uint32_t index) { return values.at(index); }
    const T &at(uint32_t index) const { return values.at(index); }
    std::span<const T> buffer() const noexcept {
      if constexpr (std::is_same_v<T, NoOctreePayload>)
        return {};
      return values;
    }
  };
  static_assert(!std::is_same_v<Cell, NoOctreePayload>,
                "A terrain terminal needs a payload");
  std::shared_ptr<std::pmr::memory_resource> resource_;
  std::pmr::memory_resource* Resource() const noexcept {
    return resource_ ? resource_.get() : std::pmr::get_default_resource();
  }
  uint32_t depth_;
  size_t terminalCount_ = 0;
  uint64_t version_ = 0;
  uint32_t freeBlock_ = null;
  std::pmr::vector<GpuNode> topology_;
  std::pmr::vector<Slot> slots_; // CPU-only edit metadata, never uploaded
  PayloadPool<Cell> cells_;
  PayloadPool<Summary> summaries_;

  bool live(uint32_t index) const noexcept {
    return topology_[index].tags & occupiedBit;
  }
  bool terminal(uint32_t index) const noexcept {
    return topology_[index].tags & terminalBit;
  }
  Handle handle(uint32_t index) const noexcept {
    return {index, slots_[index].version};
  }
  uint32_t require(Handle h) const {
    if (!contains(h))
      throw std::invalid_argument("Expired terrain handle");
    return h.index;
  }
  uint32_t octant_at(Point p, uint32_t level) const noexcept {
    const auto shift = depth_ - level - 1;
    return ((p[0] >> shift) & 1u) | (((p[1] >> shift) & 1u) << 1) |
           (((p[2] >> shift) & 1u) << 2);
  }
  void validate(Region region) const {
    if (region.depth > depth_)
      throw std::out_of_range("Terrain region depth");
    const auto size = uint32_t{1} << (depth_ - region.depth);
    for (auto coordinate : region.origin)
      if (coordinate >= extent() || coordinate % size)
        throw std::out_of_range(
            "Terrain region must be aligned and inside the tree");
  }
  uint64_t new_version() {
    if (version_ == std::numeric_limits<uint64_t>::max())
      throw std::length_error("Terrain handle version capacity");
    return ++version_;
  }
  uint32_t block() {
    if (freeBlock_ != null) {
      const auto result = freeBlock_;
      freeBlock_ = topology_[result].childBase;
      for (uint32_t k = 0; k < 8; ++k)
        topology_[result + k] = {};
      return result;
    }
    const auto size = topology_.size();
    if (size > static_cast<size_t>(null) - 8)
      throw std::length_error("Terrain node capacity");
    // Reserve both before changing sizes. No stored references survive this
    // call.
    if (topology_.capacity() < size + 8)
      topology_.reserve((size + 8) * 2);
    if (slots_.capacity() < size + 8)
      slots_.reserve((size + 8) * 2);
    topology_.resize(size + 8);
    slots_.resize(size + 8);
    return static_cast<uint32_t>(size);
  }
  void recycle_block(uint32_t first) noexcept {
    topology_[first].childBase = freeBlock_;
    freeBlock_ = first;
  }
  void activate(uint32_t index, uint32_t parent, uint32_t octant,
                bool asTerminal, const Cell &value) {
    const auto version = new_version();
    if (asTerminal) {
      const auto payload = cells_.acquire(value);
      topology_[index] = {null, payload, 0, occupiedBit | terminalBit};
      ++terminalCount_;
    } else {
      const auto first = block();
      uint32_t payload = null;
      try {
        payload = summaries_.acquire(Summary{});
      } catch (...) {
        recycle_block(first);
        throw;
      }
      topology_[index] = {first, payload, 0, occupiedBit};
    }
    slots_[index] = {version, parent,
                     parent == null ? 0 : slots_[parent].depth + 1, octant};
  }
  void split(uint32_t index) {
    if (slots_[index].depth == depth_)
      throw std::out_of_range("Finest terrain cannot refine");
    const Cell value = cells_.at(topology_[index].payload);
    const auto first = block();
    uint32_t summaryIndex = null;
    uint32_t initialized = 0;
    try {
      summaryIndex = summaries_.acquire(Summary{});
      for (; initialized < 8; ++initialized)
        activate(first + initialized, index, initialized, true, value);
    } catch (...) {
      for (uint32_t k = 0; k < initialized; ++k)
        release(first + k);
      if (summaryIndex != null)
        summaries_.recycle(summaryIndex);
      recycle_block(first);
      throw;
    }
    cells_.recycle(topology_[index].payload);
    topology_[index] = {first, summaryIndex, 255, occupiedBit};
    --terminalCount_;
  }
  void release_children(uint32_t index) noexcept {
    const auto node = topology_[index];
    for (uint32_t k = 0; k < 8; ++k)
      if (node.childBits & (1u << k))
        release(node.childBase + k);
    recycle_block(node.childBase);
    summaries_.recycle(node.payload);
  }
  void release(uint32_t index) noexcept {
    if (terminal(index)) {
      cells_.recycle(topology_[index].payload);
      --terminalCount_;
    } else
      release_children(index);
    topology_[index] = {};
  }
  void replace(uint32_t index, const Cell &value) {
    if (terminal(index)) {
      cells_.at(topology_[index].payload) = value;
      return;
    }
    // Acquire before freeing the subtree for allocation-failure safety.
    const auto payload = cells_.acquire(value);
    release_children(index);
    topology_[index] = {null, payload, 0, occupiedBit | terminalBit};
    ++terminalCount_;
  }
  uint32_t descend(Region region, bool create) {
    if (empty()) {
      if (!create)
        return null;
      activate(0, null, 0, region.depth == 0, Cell{});
    }
    uint32_t index = 0;
    for (uint32_t level = 0; level < region.depth; ++level) {
      if (terminal(index))
        split(index);
      const auto octant = octant_at(region.origin, level);
      const auto child = topology_[index].childBase + octant;
      if (!(topology_[index].childBits & (1u << octant))) {
        if (!create)
          return null;
        activate(child, index, octant, level + 1 == region.depth, Cell{});
        topology_[index].childBits |= 1u << octant;
      }
      index = child;
    }
    return index;
  }
  void prune(uint32_t index) noexcept {
    while (!topology_[index].childBits) {
      const auto slot = slots_[index];
      release(index);
      if (slot.parent == null)
        break;
      index = slot.parent;
      topology_[index].childBits &= ~(1u << slot.octant);
    }
  }
  template <class Visitor>
  void traverse(uint32_t index, Point origin, Visitor &visitor) const {
    const auto level = slots_[index].depth;
    const auto size = uint32_t{1} << (depth_ - level);
    visitor(Entry{handle(index), {origin, level}, size, terminal(index)});
    if (terminal(index))
      return;
    const auto node = topology_[index];
    for (uint32_t k = 0; k < 8; ++k) {
      if (!(node.childBits & (1u << k)))
        continue;
      auto childOrigin = origin;
      for (uint32_t axis = 0; axis < 3; ++axis)
        childOrigin[axis] += ((k >> axis) & 1u) * (size / 2);
      traverse(node.childBase + k, childOrigin, visitor);
    }
  }
};
} // namespace EESG
