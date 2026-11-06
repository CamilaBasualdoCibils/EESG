#include "AdaptiveOctree.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <random>

using Tree = EESG::AdaptiveOctree<uint32_t, uint32_t>;

// Simulate shader lookup using only the public upload ABI, not the CPU tree.
static uint32_t shader_find(const Tree::Buffers &gpu, Tree::Point p) {
  if (gpu.root == Tree::null)
    return Tree::null;
  for (const auto coordinate : p)
    if (coordinate >= (uint32_t{1} << gpu.depth))
      return Tree::null;
  uint32_t index = gpu.root;
  for (uint32_t depth = 0; depth <= gpu.depth; ++depth) {
    assert(index < gpu.topology.size());
    const auto node = gpu.topology[index];
    assert(node.tags & Tree::occupiedBit);
    if (node.tags & Tree::terminalBit)
      return index;
    assert(depth < gpu.depth);
    const auto shift = gpu.depth - depth - 1;
    const auto octant = ((p[0] >> shift) & 1u) | (((p[1] >> shift) & 1u) << 1) |
                        (((p[2] >> shift) & 1u) << 2);
    if (!(node.childBits & (1u << octant)))
      return Tree::null;
    index = node.childBase + octant;
  }
  std::abort();
}

int main() {
  Tree tree(3);
  assert(tree.empty());
  assert(!tree.root());
  assert(!tree.find({0, 0, 0}));
  const auto coarse = tree.paint({{0, 0, 0}, 2}, 7);
  const auto other = tree.paint({{7, 7, 7}, 3}, 9);
  assert(tree.terminal_count() == 2);
  assert(tree.cell(tree.find({1, 1, 1})) == 7);
  assert(!tree.find({2, 2, 2}));
  assert(tree.erase({{0, 0, 0}, 2}));
  assert(!tree.contains(coarse));
  assert(tree.contains(other));
  assert(tree.cell(other) == 9);
  tree.paint({{0, 0, 0}, 2}, 4);
  assert(!tree.contains(coarse)); // recycled index cannot resurrect handle
  bool threw = false;
  try {
    (void)tree.cell(coarse);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  tree.clear();
  assert(!tree.contains(other));
  const auto root = tree.paint({{0, 0, 0}, 0}, 3);
  tree.paint({{0, 0, 0}, 3}, 8);
  assert(tree.contains(root));
  assert(tree.cell(tree.find({1, 0, 0})) == 3);
  assert(tree.cell(tree.find({0, 0, 0})) == 8);
  const auto descendant = tree.find({1, 0, 0});
  assert(tree.erase({{0, 0, 0}, 3}));
  assert(!tree.find({0, 0, 0}));
  tree.coarsen(root, 5);
  assert(tree.terminal_count() == 1);
  assert(!tree.contains(descendant));
  tree.refine(root);
  assert(tree.terminal_count() == 8);
  tree.set_summary(root, 42);
  assert(tree.summary(root) == 42);
  tree.visit([&](const Tree::Entry &entry) {
    if (entry.region.depth == 1) {
      assert(entry.extent == 4);
      assert(tree.find(entry.region.origin) == entry.handle);
      assert(tree.cell(entry.handle) == 5);
    }
  });
  threw = false;
  try {
    tree.paint({{1, 0, 0}, 1}, 0);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw); // region origins must align
  threw = false;
  try {
    tree.paint({{8, 0, 0}, 1}, 0);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw);
  assert(!tree.find({8, 0, 0}));
  tree.clear();
  std::array<int, 512> dense;
  dense.fill(-1);
  std::mt19937 rng(1234);
  for (int step = 0; step < 3000; ++step) {
    const uint32_t depth = rng() % 4;
    const uint32_t size = 1u << (3 - depth);
    Tree::Point origin{uint32_t(rng() % 8) / size * size,
                       uint32_t(rng() % 8) / size * size,
                       uint32_t(rng() % 8) / size * size};
    const bool erase = rng() % 3 == 0;
    if (erase)
      tree.erase({origin, depth});
    else
      tree.paint({origin, depth}, step);
    const auto gpu = tree.upload();
    for (uint32_t x = 0; x < 8; ++x)
      for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t z = 0; z < 8; ++z) {
          auto &expected = dense[x * 64 + y * 8 + z];
          if (x / size == origin[0] / size && y / size == origin[1] / size &&
              z / size == origin[2] / size)
            expected = erase ? -1 : step;
          const auto h = tree.find({x, y, z});
          const auto shaderIndex = shader_find(gpu, {x, y, z});
          assert(h.index == shaderIndex);
          assert((h ? int(tree.cell(h)) : -1) == expected);
          if (shaderIndex != Tree::null)
            assert(int(gpu.cells[gpu.topology[shaderIndex].payload]) ==
                   expected);
        }
    size_t count = 0;
    tree.visit([&](const Tree::Entry &entry) {
      const auto &node = gpu.topology[entry.handle.index];
      assert(tree.contains(entry.handle));
      if (entry.terminal)
        ++count;
      else {
        assert(node.childBase + 7 < gpu.topology.size());
        assert(node.childBits != 0);
        for (uint32_t k = 0; k < 8; ++k)
          assert(
              bool(node.childBits & (1u << k)) ==
              bool(gpu.topology[node.childBase + k].tags & Tree::occupiedBit));
      }
    });
    assert(count == tree.terminal_count());
  }
  // Storage reaches a high-water mark, then reuses both nodes and payloads.
  tree.clear();
  tree.paint({{0, 0, 0}, 0}, 1);
  for (uint32_t x = 0; x < 8; ++x)
    for (uint32_t y = 0; y < 8; ++y)
      for (uint32_t z = 0; z < 8; ++z)
        tree.paint({{x, y, z}, 3}, 2);
  const auto full = tree.upload();
  const auto nodes = full.topology.size(), cells = full.cells.size(),
             summaries = full.summaries.size();
  for (int repetition = 0; repetition < 10; ++repetition) {
    tree.clear();
    for (uint32_t x = 0; x < 8; ++x)
      for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t z = 0; z < 8; ++z)
          tree.paint({{x, y, z}, 3}, 3);
    assert(tree.upload().topology.size() == nodes);
    assert(tree.upload().cells.size() == cells);
    assert(tree.upload().summaries.size() == summaries);
  }
  EESG::AdaptiveOctree<uint32_t> zero(0);
  auto h = zero.paint({{0, 0, 0}, 0}, 1);
  assert(zero.upload().summaries.empty());
  threw = false;
  try {
    zero.refine(h);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw);
  zero.clear();
  zero.paint({{0, 0, 0}, 0}, 1);
  assert(!zero.contains(h));
  EESG::AdaptiveOctree<uint32_t> large(31);
  const uint32_t edge = (uint32_t{1} << 31) - 1;
  h = large.paint({{edge, edge, edge}, 31}, 77);
  assert(large.cell(large.find({edge, edge, edge})) == 77);
  assert(large.erase({{edge, edge, edge}, 31}));
  assert(large.empty());
  threw = false;
  try {
    EESG::AdaptiveOctree<uint32_t> invalid(32);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  std::cout << "AdaptiveOctree tests passed\n";
}
