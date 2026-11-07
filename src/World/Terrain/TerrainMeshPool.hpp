#pragma once

#include "World/Terrain/TerrainMesher.hpp"
#include <Lighthouse/Engine/Rendering/IRenderDevice.hpp>
#include <Lighthouse/Engine/Rendering/IRenderCommandList.hpp>

namespace EESG
{
// Owns shared terrain buffers. Rebuild after edits before compiling the graph;
// graph imports stay valid until the next rebuild or destruction.
class TerrainMeshPool
{
public:
  explicit TerrainMeshPool(Lighthouse::IRenderDevice& device) : device_(device) {}
  TerrainMeshPool(const TerrainMeshPool&) = delete;
  TerrainMeshPool& operator=(const TerrainMeshPool&) = delete;
  ~TerrainMeshPool() { Clear(); }
  void Rebuild(const WorldTerrain& terrain)
  {
    if (revision_ == terrain.revision()) return;
    std::vector<Lighthouse::Mesh3DVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Lighthouse::DrawIndexedIndirectCommand> draws;
    for (auto key : terrain.source_keys())
    {
      auto mesh = TerrainMesher::Build(terrain, key);
      if (mesh.indices.empty()) continue;
      if (vertices.size() + mesh.vertices.size() > size_t(INT32_MAX) ||
          indices.size() + mesh.indices.size() > size_t(UINT32_MAX))
        throw std::overflow_error("Terrain mesh pool exceeds draw address range");
      draws.push_back({uint32_t(mesh.indices.size()), 1, uint32_t(indices.size()),
                       int32_t(vertices.size()), 0});
      vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
      indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }
    Clear();
    using namespace Lighthouse;
    try
    {
      if (!draws.empty())
      {
        vertices_ = device_.CreateBuffer({.size = vertices.size() * sizeof(vertices[0]),
            .usage = BufferUsage::Vertex, .initialData = std::as_bytes(std::span(vertices))});
        indices_ = device_.CreateBuffer({.size = indices.size() * sizeof(indices[0]),
            .usage = BufferUsage::Index, .initialData = std::as_bytes(std::span(indices))});
        indirect_ = device_.CreateBuffer({.size = draws.size() * sizeof(draws[0]),
            .usage = BufferUsage::Indirect, .initialData = std::as_bytes(std::span(draws))});
      }
      count_ = uint32_t(draws.size());
      revision_ = terrain.revision();
    }
    catch (...) { Clear(); throw; }
  }
  void Draw(Lighthouse::IRenderCommandList& commands) const
  {
    if (!count_) return;
    commands.BindVertexBuffer(vertices_, sizeof(Lighthouse::Mesh3DVertex));
    commands.BindIndexBuffer(indices_);
    commands.MultiDrawIndexedIndirect(indirect_, count_);
  }
  Lighthouse::BufferHandle Vertices() const { return vertices_; }
  Lighthouse::BufferHandle Indices() const { return indices_; }
  Lighthouse::BufferHandle Indirect() const { return indirect_; }
  uint64_t Revision() const { return revision_; }
private:
  void Clear()
  {
    if (vertices_ || indices_ || indirect_) device_.WaitIdle();
    for (auto buffer : {vertices_, indices_, indirect_})
      if (buffer) device_.DestroyBuffer(buffer);
    vertices_ = {}; indices_ = {}; indirect_ = {}; count_ = 0;
    revision_ = UINT64_MAX;
  }
  Lighthouse::IRenderDevice& device_;
  Lighthouse::BufferHandle vertices_{}, indices_{}, indirect_{};
  uint32_t count_ = 0;
  uint64_t revision_ = UINT64_MAX;
};
} // namespace EESG
