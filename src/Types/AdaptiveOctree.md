# Adaptive terrain octree

This is a new container and API: `EESG::AdaptiveOctree<Cell, Summary>` in
`src/Types/AdaptiveOctree.hpp`. It has no engine or math-library dependencies.
The former `Lighthouse::Octree` API is gone. No current EESG source calls it.

## Terrain model

A region is absent, terminal, or subdivided. A terminal can appear at any depth
and covers a cube uniformly represented by its `Cell` payload. A cell can instead
reference an external terrain brick if you need dense voxel samples at leaves.
Branches have independent `Summary` payloads, for example min/max density bounds.
Omit `Summary` to allocate no branch payload buffer. The application maintains
summaries after terrain edits; the container cannot infer their meaning.

Depth is a runtime constructor argument in `[0, 31]`. Integer coordinates lie in
`[0, 2^depth)`, and finest cells have extent one. A `Region` is an aligned origin
plus a depth. Misaligned or out-of-domain edit regions throw. Out-of-domain queries
return an empty handle.

```cpp
struct Cell { uint32_t material = 0; float density = 0; };
struct Summary { float minimum = 0; float maximum = 0; };
EESG::AdaptiveOctree<Cell, Summary> terrain(12);

// Store a 256-cubed uniform terrain region, then refine one voxel inside it.
auto region = terrain.paint({{256, 0, 0}, 4}, {2, 1.0f});
terrain.paint({{258, 1, 1}, 12}, {3, 0.5f});
auto voxel = terrain.find({258, 1, 1});
const auto value = terrain.cell(voxel);
terrain.erase({{258, 1, 1}, 12});
terrain.coarsen(region, {2, 1.0f});
```

`paint` replaces exactly a region. Refining existing terrain copies its value to
all eight children, preserving surrounding terrain. Creating a sparse path never
fills missing neighbors. `refine(handle)` explicitly subdivides a terminal;
`coarsen(handle, value)` replaces its whole subtree. `erase` prunes empty ancestors.
`clear` empties the tree while retaining reusable storage. There is no implicit
merge based on payload equality.

`find` returns the covering terminal, regardless of depth. `visit` walks all live
nodes in depth-first order with their origin, extent, depth and handle. The callback
must not edit the tree. Morton codes are unnecessary for traversal and are neither
stored nor required by this API.

## Storage and lifetimes

Topology uses one flat buffer. Branches reserve eight consecutive child slots;
a child mask distinguishes present terrain from missing octants. Deletion recycles
blocks without shifting live nodes. CPU-only metadata records parents, depth, and
versions, so edits need no hierarchy scan. Payloads have independent recycled pools.

Handles contain a node index and version. They remain valid through unrelated
edits and buffer growth; deleting and reusing their slot does not resurrect them.
Refining/coarsening retains the edited parent's identity and invalidates any
removed descendants. `clear` invalidates all live handles. A handle belongs to the
instance that issued it. The container is deliberately noncopyable and nonmovable.

Node and payload buffers retain their high-water allocation, including holes.
Rebuilding a comparable tree reuses that storage. Mutations can invalidate borrowed
spans and payload references; obtain them again afterward. The container is not
thread-safe. Allocation failure can leave already-completed refinements in place;
operations do not promise transactional rollback of a whole path edit.

Payload types must be trivially copyable, with nonthrowing default construction,
copy construction, and copy assignment. Shader layout compatibility remains the
application's responsibility: in particular, avoid assuming arbitrary C++ structs,
`bool`, or vector-library types match storage-buffer layout automatically.

## GPU contract

`upload()` returns borrowed spans for topology, cells, and summaries, plus runtime
depth and root index (`0xffffffff` when empty). Upload all spans as one consistent
revision. GPU traversal requires only these buffers and constants; no CPU pointers,
parent metadata, handle versions, or per-level structures are needed.

Each `GpuNode` has a verified 16-byte stride and four uint32 fields in order:

| Byte | Field | Meaning |
| --- | --- | --- |
| 0 | `childBase` | First of eight reserved child slots, invalid for terminals |
| 4 | `payload` | Index into the terminal cell or branch summary buffer |
| 8 | `childBits` | Present octants, in the low eight bits |
| 12 | `tags` | Bit 0: terminal; bit 1: occupied |

Octant bits use **x=bit 0, y=bit 1, z=bit 2**. Child `k` is at `childBase+k`,
provided `childBits & (1u << k)` is nonzero. Do not compact holes, or apply popcount
offsets, when uploading these buffers. An optional future packing pass would need
to remap both topology and payload indices.

For example, shader-side point lookup can use this HLSL/Slang-style function:

```hlsl
struct TerrainNode {
    uint childBase;
    uint payload;
    uint childBits;
    uint tags;
};

uint findTerrain(StructuredBuffer<TerrainNode> nodes, uint root,
                 uint treeDepth, uint3 point)
{
    const uint invalid = 0xffffffffu;
    if (root == invalid || any(point >= (1u << treeDepth))) return invalid;
    uint index = root;
    for (uint level = 0; level <= treeDepth; ++level) {
        TerrainNode node = nodes[index];
        if ((node.tags & 2u) == 0) return invalid;
        if ((node.tags & 1u) != 0) return index;
        if (level == treeDepth) return invalid;
        uint3 bits = (point >> (treeDepth - level - 1)) & 1u;
        uint octant = bits.x | (bits.y << 1) | (bits.z << 2);
        if ((node.childBits & (1u << octant)) == 0) return invalid;
        index = node.childBase + octant;
    }
    return invalid;
}
```

A returned node's `payload` indexes the cell buffer. Hierarchical ray or volume
traversal can propagate cube bounds from the root with the same octant convention.
Graphics API allocation, upload synchronization, and renderer integration are
application concerns and are not performed by this container.

## Validation

`EESGOctreeTests` is registered when `EESG_BUILD_TESTING=ON`. It is also standalone:
compile `tests/AdaptiveOctreeTests.cpp` as C++23 with `src/Types` on the include path.
It tests sparse and mixed-depth terrain, preservation during refinement, collapse,
pruning, stale handles, storage reuse, alignment and bounds, depths 0 and 31, and
3,000 randomized edits compared against a dense grid. Every grid query also checks
shader-style traversal using only the exported GPU buffers.
