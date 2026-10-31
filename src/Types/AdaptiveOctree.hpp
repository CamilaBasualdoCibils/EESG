#pragma once
#include "AABB.hpp"
#include "MortonOrdering.hpp"

#include "Types.hpp"
#include "pch.hpp"
#include <algorithm>
#include <bitset>
#include <optional>
#include <stack>
#include <variant>
namespace Lighthouse {
template <uint32_t TreeDepth, typename LeafData = void,
          typename BranchData = void>
class Octree {
public:
  using MortonIndexType =
      typename std::conditional<TreeDepth <= 10, uint32_t, uint64_t>::type;
  using VoxelPosition = uint32_t;
  using LeafDataType = LeafData;

private:
  constexpr static inline uint32_t MAX_DEPTH =
      32; // given 2^32 is the max a uint32 coord can store
  struct Node {
    std::bitset<8> ChildrenSet = 0;
    // std::array<MortonIndexType, 8> children = {0};
    LeafData data;
    bool HasChildAtIndex(uint32_t index) const {
      return ChildrenSet.test(index);
    }
    uint8_t GetChildCount() const { return ChildrenSet.count(); }
  };
  struct custom_hash_simple {
    auto operator()(MortonIndexType const &x) const noexcept -> uint64_t {
      return x;
    }
  };
  // using NodesContainer = ankerl::unordered_dense::map<MortonIndexType, Node>;
  //   using NodesContainer =
  //   ankerl::unordered_dense::segmented_map<MortonIndexType,Node,custom_hash_simple>;
  using NodesContainer = std::unordered_map<MortonIndexType, Node>;
  // using NodesContainer = std::vector<Node>;
  std::array<NodesContainer, TreeDepth + 1> Levels;

  uint32_t SignToChildIndex(IVec3 sign) {
    return ((sign.x >= 0) << 2) | ((sign.y >= 0) << 1) | (sign.z >= 0);
  }
  Vec<3, int8_t> ChildIndexToSign(uint32_t index) {
    IVec3 sign;
    for (int i = 0; i < 3; i++)
      sign[i] = ((index >> (2 - i)) & 0b1) ? 1 : -1;
    return sign;
  }

  MortonIndexType IndexFromChildPos(uvec3 ChildPos, uint32_t ChildDepth) {
    return Morton::Regular::MortonEncodeBounded<3>(
        ChildPos / SizeOfNodeAtDepth(ChildDepth),
        uvec3(OctreeResolution(ChildDepth)));
  }

public:
  Octree();

  /// @brief Gets the deepest node that occupies that position
  /// @param Position
  /// @param Level Specify a specific level to look for
  /// @return
  Node &AtPositionNode(uvec3 Position, std::optional<uint32_t> Level);
  /// @brief Get the Leaf node at a given global position of last depth
  /// @param Position
  /// @return
  Node &AtPositionLeaf(uvec3 Position);
  /// @brief Gets the node that exists at a specified level and Morton Index
  /// @param level
  /// @param index
  /// @return
  Node &AtMortonIndex(uint32_t level, MortonIndexType index);

  Node &InsertLeaf(uvec3 GlobalPosition);
  void InsertLeafs(const std::vector<uvec3> &positions);

  void Clear() {
    for (auto &level : Levels) {
      level.clear();
    }
  }
  bool empty() const { return Levels[TreeDepth].empty(); }
  size_t LeafCount() const { return Levels[TreeDepth].size(); }
  bool ContainLeaf(ivec3 Position) const {
    if (!IsPosInside(Position))
      return false;
    uvec3 NodePosition = uvec3(0);
    MortonIndexType morton = Morton::Regular::MortonEncodeBounded<3>(
        uvec3(Position), uvec3(OctreeResolution()));
    return Levels[TreeDepth].find(morton) != Levels[TreeDepth].end();
  }
  constexpr bool IsPosInside(ivec3 Position) const {
    return glm::all(glm::greaterThanEqual(Position, ivec3(0))) &&
           glm::all(glm::lessThan(
               Position, ivec3(OctreeResolution()))); // check if position is
                                                      // within octree bounds
  }
  constexpr uint32 SizeOfNodeAtDepth(uint32 depth) const {
    return uint32(1) << (TreeDepth - depth);
  };
  constexpr uint32 OctreeResolution(uint32 AtDepth = TreeDepth) const {
    return uint32(1) << AtDepth;
  }
  constexpr size_t AbsMaxMemoryUse() const {
    size_t size = 0;
    for (int i = 0; i < TreeDepth; i++) {
      const uint32 res = OctreeResolution(i);
      size += res * res * res * sizeof(Node);
    }
    return size;
  }
  size_t CurrentMemoryUsage() const {
    size_t size = 0;
    for (int i = 0; i < TreeDepth; i++) {
      size += sizeof(Node) * Levels[i].size();
    }
    return size;
  }
  constexpr uint32 GetDepth() const { return TreeDepth; }

  struct NodeData {
    MortonIndexType mortonCode;
    uint32 Depth;
    uint32 NodeSizeXYZ;
    uvec3 NodePos;
    bool isLeaf;
    std::variant<std::monostate, LeafData *, BranchData *> data;

    template <typename J> J *GetData() const {
      static_assert(!std::is_same_v<J, J>,
                    "Use GetLeafData() or GetBranchData(); GetData<T>() is "
                    "ambiguous here.");
      return nullptr;
    }

    LeafData *GetLeafData() const {
      return data.index() == 1 ? std::get<1>(data) : nullptr;
    }

    BranchData *GetBranchData() const {
      return data.index() == 2 ? std::get<2>(data) : nullptr;
    }
  };

  void ForEach(std::function<void(const NodeData &)> Func);
};

template <uint32 TreeDepth, typename LeafData, typename BranchData>
inline Octree<TreeDepth, LeafData, BranchData>::Octree() {
  static_assert(TreeDepth <= MAX_DEPTH, "Exceeds maximum depth");
  // Levels[0].resize(1);
}

template <uint32 TreeDepth, typename LeafData, typename BranchData>
inline typename Octree<TreeDepth, LeafData, BranchData>::Node &
Octree<TreeDepth, LeafData, BranchData>::AtPositionLeaf(uvec3 Position) {
  uvec3 NodePosition = uvec3(0);
  MortonIndexType morton = Morton::Regular::MortonEncodeBounded<3>(
      Position, uvec3(OctreeResolution()));
  return AtMortonIndex(TreeDepth, morton);
}

template <uint32 TreeDepth, typename LeafData, typename BranchData>
inline typename Octree<TreeDepth, LeafData, BranchData>::Node &
Octree<TreeDepth, LeafData, BranchData>::AtMortonIndex(uint32 level,
                                                       MortonIndexType index) {
  return Levels.at(level).at(index);
}

template <uint32 TreeDepth, typename LeafData, typename BranchData>
inline typename Octree<TreeDepth, LeafData, BranchData>::Node &
Octree<TreeDepth, LeafData, BranchData>::InsertLeaf(uvec3 GlobalPosition) {
  assert(glm::all(glm::greaterThanEqual(GlobalPosition, uvec3(0))) &&
         glm::all(glm::lessThan(GlobalPosition, uvec3(OctreeResolution()))) &&
         "Invalid Position");

  Node *currentNode = &Levels[0][0];
  uvec3 Position = {0, 0, 0};

  for (int idepth = 0; idepth < TreeDepth; idepth++) {
    const uint32 NodeSizeXYZ = SizeOfNodeAtDepth(idepth);
    const AABB3i nodeAABB(Position, Position + uvec3(NodeSizeXYZ));
    const ivec3 childSign = nodeAABB.SignFromCenter(GlobalPosition);
    const uint32 ChildIndex = SignToChildIndex(childSign);
    uvec3 PositionOfChild =
        Position + uvec3(glm::min(childSign + ivec3(1), ivec3(1))) *
                       uint32(NodeSizeXYZ / 2);
    const MortonIndexType EncodedChildPosition =
        IndexFromChildPos(PositionOfChild, idepth + 1);
    auto &ChildBuffer = Levels[idepth + 1];

    currentNode->ChildrenSet.set(ChildIndex);
    if (ChildBuffer.find(EncodedChildPosition) == ChildBuffer.end()) {
      ChildBuffer.insert(std::make_pair(EncodedChildPosition, Node()));
    }

    Node *Child = &ChildBuffer.at(EncodedChildPosition);
    currentNode = Child;
    Position +=
        glm::min(childSign + ivec3(1), ivec3(1)) * int32(NodeSizeXYZ / 2);
  }

  return *currentNode;
}

template <uint32 TreeDepth, typename LeafData, typename BranchData>
inline void Octree<TreeDepth, LeafData, BranchData>::InsertLeafs(
    const std::vector<uvec3> &positions) {
  static std::vector<std::pair<uvec3, MortonIndexType>> posMorton;
  // std::pair<uvec3, uint32> posMorton[positions.size()];
  posMorton.clear();
  posMorton.reserve(positions.size());
  int i = 0;
  for (auto &pos : positions) {
    MortonIndexType idx =
        Morton::Regular::MortonEncode<3>(pos); // implement MortonEncode(x,y,z)
    // posMorton[i] = {pos, idx};
    posMorton.push_back({pos, idx});
    i++;
  }
  std::sort(posMorton.begin(), posMorton.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });

  for (const auto &[pos, morton] : posMorton) {
    InsertLeaf(pos);
  }
}

template <uint32 TreeDepth, typename LeafData, typename BranchData>
inline void Octree<TreeDepth, LeafData, BranchData>::ForEach(
    std::function<void(const NodeData &)> func) {
  struct NodeInfo {
    uvec3 NodePos;
    MortonIndexType MortonIndex;
    uint32 Depth;
    Node *node;
    bool isLeaf = false;
  };
  std::stack<NodeInfo> stack;
  uint64 NodesProcessed = 0;
  stack.push(NodeInfo{.NodePos = {0, 0, 0},
                      .MortonIndex = 0,
                      .Depth = 0,
                      .node = &Levels.at(0).at(0)});
  while (!stack.empty()) {
    NodeInfo Current = stack.top();
    Node *node = Current.node;
    stack.pop();
    NodesProcessed++;
    NodeData iterData;
    iterData.Depth = Current.Depth;
    iterData.NodeSizeXYZ = SizeOfNodeAtDepth(Current.Depth);
    iterData.isLeaf = Current.isLeaf;
    iterData.mortonCode = Current.MortonIndex;
    iterData.NodePos = Current.NodePos;

    if (Current.isLeaf) {
      iterData.data.template emplace<1>(&node->data);
    } else {
      iterData.data.template emplace<0>();
    }

    func(iterData);
    if (Current.isLeaf) {
      continue;
    }
    for (uint8 i = 0; i < 8; i++) {
      if (node->HasChildAtIndex(i)) {
        NodeInfo ChildInfo;
        ChildInfo.Depth = Current.Depth + 1;
        vec<3, int8> ChildSignVector =
            glm::max(ChildIndexToSign(i), vec<3, int8>(0));

        ChildInfo.NodePos =
            Current.NodePos +
            (SizeOfNodeAtDepth(Current.Depth) / 2) * (uvec3)ChildSignVector;
        ChildInfo.MortonIndex =
            IndexFromChildPos(ChildInfo.NodePos, ChildInfo.Depth);
        ChildInfo.node = &Levels.at(ChildInfo.Depth).at(ChildInfo.MortonIndex);
        ChildInfo.isLeaf = ChildInfo.Depth == TreeDepth;
        stack.push(ChildInfo);
      }
    }
  }
}
} // namespace Lighthouse