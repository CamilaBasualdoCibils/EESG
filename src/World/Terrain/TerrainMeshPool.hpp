#pragma once
#include "World/Terrain/TerrainMeshArena.hpp"
#include "World/Terrain/TerrainMeshScheduler.hpp"
#include <Lighthouse/Engine/Rendering/IRenderDevice.hpp>
#include <atomic>
#include <cstring>
#include <numeric>
namespace EESG
{
struct TerrainMeshStats
{
  size_t pending = 0, chunks = 0, bufferUsed = 0, bufferCapacity = 0,
         uploadedBytes = 0, uploadedChunks = 0, retiringRanges = 0;
  uint64_t scheduled = 0, discarded = 0;
  std::string error;
  std::vector<TerrainWorkerStatus> workers;
  size_t activeWorkers = 0,
         workerLimit = TerrainMeshScheduler::default_meshing_threads,
         retiringWorkers = 0;
  TerrainBackgroundStatus uploadWorker;
};
// One persistent mapped geometry buffer. Workers copy only to exclusive unused
// ranges; the rendering thread polls futures and zero-timeout GPU markers.
class TerrainMeshPool
{
public:
  explicit TerrainMeshPool(Lighthouse::IRenderDevice& device,
                           size_t capacity = 64u * 1024 * 1024)
      : device_(device), arena_(capacity)
  {
    if (!capacity || capacity > size_t(INT32_MAX))
      throw std::invalid_argument("Terrain buffer capacity");
    alignment_ = std::lcm(size_t(device_.StreamingBufferAlignment()),
                          sizeof(Lighthouse::Mesh3DVertex));
    buffer_ = device_.CreateBuffer(
        {.size = capacity,
         .usage = {Lighthouse::BufferUsage::Vertex,
                   Lighthouse::BufferUsage::Index},
         .memory = {.preferredLocation = Lighthouse::MemoryLocation::HostLocal,
                    .requiredAccess = Lighthouse::MemoryAccess::CPUWrite |
                                      Lighthouse::MemoryAccess::GPURead}});
    try
    {
      mapped_ = device_.MapStreamingBuffer(buffer_);
    }
    catch (...)
    {
      device_.DestroyBuffer(buffer_);
      throw;
    }
  }
  TerrainMeshPool(const TerrainMeshPool&) = delete;
  TerrainMeshPool& operator=(const TerrainMeshPool&) = delete;
  ~TerrainMeshPool()
  {
    if (upload_ && upload_->future.valid())
    {
      upload_->cancelled->store(true);
      upload_->future.wait();
    }
    device_.WaitIdle(); // shutdown only
    for (const auto& retired : retired_)
      device_.ReleaseCompletionMarker(retired.marker);
    device_.DestroyBuffer(buffer_);
  }
  void Update(const WorldTerrain& terrain, size_t byteBudget = 2u * 1024 * 1024,
              size_t chunkBudget = 2)
  {
    stats_.uploadedBytes = stats_.uploadedChunks = 0;
    scheduler_.Update(terrain);
    for (auto it = retired_.begin(); it != retired_.end();)
      if (device_.PollCompletionMarker(it->marker))
      {
        device_.ReleaseCompletionMarker(it->marker);
        arena_.Free(it->range);
        it = retired_.erase(it);
      }
      else
        ++it;
    size_t removals = 0;
    for (auto it = draws_.begin();
         it != draws_.end() && removals < chunkBudget;)
      if (!scheduler_.Contains(it->first))
      {
        Retire(it->second.range);
        it = draws_.erase(it);
        ++removals;
      }
      else
        ++it;
    if (upload_ && !scheduler_.Current(upload_->metadata))
      upload_->cancelled->store(true);
    if (upload_ && upload_->future.valid())
    {
      if (upload_->future.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
      {
        FinishStats();
        return;
      }
      const auto copied = upload_->future.get();
      stats_.uploadedBytes = copied;
      if (upload_->cancelled->load())
      {
        if (upload_->range)
          arena_.Free(*upload_->range); // never submitted to GPU
        upload_.reset();
      }
      else
      {
        if (copied)
          device_.PublishStreamingWrites(
              buffer_, upload_->range->offset + upload_->offset, copied);
        upload_->offset += copied;
        if (upload_->offset == upload_->total)
        {
          Replace(upload_->metadata.key,
                  {*upload_->range, upload_->count,
                   uint32_t((upload_->range->offset + upload_->vertexBytes) /
                            sizeof(uint32_t)),
                   int32_t(upload_->range->offset /
                           sizeof(Lighthouse::Mesh3DVertex))});
          upload_.reset();
          ++stats_.uploadedChunks;
          stats_.error.clear();
        }
      }
    }
    while (!upload_ && stats_.uploadedChunks < chunkBudget)
    {
      auto ready = scheduler_.TakeReady();
      if (!ready)
        break;
      if (!ready->error.empty())
      {
        stats_.error = ready->error;
        continue;
      }
      Upload upload;
      upload.vertexBytes =
          ready->mesh.vertices.size() * sizeof(Lighthouse::Mesh3DVertex);
      upload.total =
          upload.vertexBytes + ready->mesh.indices.size() * sizeof(uint32_t);
      upload.count = uint32_t(ready->mesh.indices.size());
      upload.payload = std::make_shared<Payload>();
      upload.payload->mesh = std::move(ready->mesh);
      upload.metadata = std::move(*ready);
      upload.cancelled = std::make_shared<std::atomic<bool>>(
          !scheduler_.Current(upload.metadata));
      upload_ = std::move(upload);
      if (!upload_->total && !upload_->cancelled->load())
      {
        Replace(upload_->metadata.key, {});
        upload_.reset();
        ++stats_.uploadedChunks;
        continue;
      }
    }
    if (upload_ && !upload_->future.valid())
    {
      if (upload_->total > arena_.Capacity())
      {
        stats_.error = "Chunk mesh exceeds shared terrain buffer capacity";
        upload_->cancelled->store(true);
      }
      if (!upload_->range && !upload_->cancelled->load())
      {
        const auto allocation =
            (upload_->total + alignment_ - 1) / alignment_ * alignment_;
        upload_->range = arena_.Allocate(allocation, alignment_);
        if (!upload_->range)
          stats_.error = "Shared buffer waiting for GPU-safe free space";
      }
      if (upload_->cancelled->load() || (upload_->range && byteBudget))
      {
        const auto payload = upload_->payload;
        const auto cancelled = upload_->cancelled;
        const auto activity = uploadActivity_;
        const auto offset = upload_->offset, total = upload_->total;
        const auto count = std::min(total - offset, byteBudget);
        auto* target = upload_->range
                           ? mapped_.data() + upload_->range->offset + offset
                           : nullptr;
        activity->Begin(upload_->metadata.key, upload_->metadata.stamp.lod);
        upload_->future = std::async(
            std::launch::async,
            [payload, cancelled, activity, offset, total, count, target]()
            {
              activity->Mark("Copying to unused GPU buffer range",
                             "TerrainUpload");
              size_t copied = 0;
              if (!cancelled->load())
              {
                const auto vertices =
                    std::as_bytes(std::span(payload->mesh.vertices));
                const auto indices =
                    std::as_bytes(std::span(payload->mesh.indices));
                while (copied < count)
                {
                  const auto position = offset + copied;
                  const auto bytes =
                      position < vertices.size()
                          ? vertices.subspan(position)
                          : indices.subspan(position - vertices.size());
                  const auto amount = std::min(count - copied, bytes.size());
                  std::memcpy(target + copied, bytes.data(), amount);
                  copied += amount;
                }
              }
              if (cancelled->load() || offset + copied == total)
                payload->mesh = {}; // free CPU data on worker
              activity->Finish(
                  cancelled->load()
                      ? "Discarded stale upload"
                      : "Mapped range copied; ready for publication");
              return copied;
            });
      }
    }
    FinishStats();
  }
  void Draw(Lighthouse::IRenderCommandList& commands) const
  {
    commands.BindVertexBuffer(buffer_, sizeof(Lighthouse::Mesh3DVertex));
    commands.BindIndexBuffer(buffer_);
    for (const auto& [key, draw] : draws_)
      if (scheduler_.Contains(key))
        commands.DrawIndexedRange(draw.count, draw.firstIndex, draw.baseVertex);
  }
  Lighthouse::BufferHandle Buffer() const
  {
    return buffer_;
  }
  void SetMeshingThreadCount(size_t count)
  {
    scheduler_.SetMeshingThreadCount(count);
  }
  TerrainMeshStats Stats() const
  {
    auto stats = stats_;
    stats.workerLimit = scheduler_.MeshingThreadCount();
    stats.retiringWorkers = scheduler_.RetiringWorkers();
    stats.workers = scheduler_.Workers();
    stats.uploadWorker = uploadActivity_->Snapshot();
    for (const auto& worker : stats.workers)
      if (worker.phase == TerrainWorkerPhase::Starting ||
          worker.phase == TerrainWorkerPhase::Meshing)
        ++stats.activeWorkers;
    return stats;
  }

private:
  struct DrawRange
  {
    TerrainMeshArena::Range range;
    uint32_t count = 0, firstIndex = 0;
    int32_t baseVertex = 0;
  };
  struct Payload
  {
    Lighthouse::Mesh3D mesh;
  };
  struct Upload
  {
    TerrainMeshResult metadata;
    std::shared_ptr<Payload> payload;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::optional<TerrainMeshArena::Range> range;
    size_t offset = 0, total = 0, vertexBytes = 0;
    uint32_t count = 0;
    std::future<size_t> future;
  };
  struct RetiredRange
  {
    TerrainMeshArena::Range range;
    uint64_t marker;
  };
  void Retire(TerrainMeshArena::Range range)
  {
    if (range.size)
      retired_.push_back({range, device_.InsertCompletionMarker()});
  }
  void Replace(RegionKey key, DrawRange next)
  {
    if (auto old = draws_.find(key); old != draws_.end())
    {
      Retire(old->second.range);
      draws_.erase(old);
    }
    if (next.count)
      draws_.emplace(key, next);
  }
  void FinishStats()
  {
    stats_.pending =
        scheduler_.Pending() + size_t(upload_.has_value()) + retired_.size();
    stats_.chunks = draws_.size();
    stats_.bufferUsed = arena_.Used();
    stats_.bufferCapacity = arena_.Capacity();
    stats_.retiringRanges = retired_.size();
    stats_.scheduled = scheduler_.Scheduled();
    stats_.discarded = scheduler_.Discarded();
  }
  Lighthouse::IRenderDevice& device_;
  TerrainMeshArena arena_;
  Lighthouse::BufferHandle buffer_{};
  std::span<std::byte> mapped_;
  size_t alignment_ = 256;
  TerrainMeshScheduler scheduler_;
  std::unordered_map<RegionKey, DrawRange, RegionKeyHash> draws_;
  std::vector<RetiredRange> retired_;
  std::optional<Upload> upload_;
  std::shared_ptr<TerrainBackgroundActivity> uploadActivity_ =
      std::make_shared<TerrainBackgroundActivity>();
  TerrainMeshStats stats_;
};
} // namespace EESG
