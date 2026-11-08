#include "World/Terrain/TerrainMeshPool.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_set>
using namespace EESG;
using namespace Lighthouse;

// Records uploads and draw addresses without requiring a graphics driver.
class BufferDevice final : public IRenderDevice
{
public:
  std::vector<std::byte> bytes;
  BufferMetadata metadata{};
  size_t created = 0, destroyed = 0, uploaded = 0, waits = 0;
  bool gpuComplete = true;
  uint64_t markerCounter = 0;
  std::span<std::byte> MapStreamingBuffer(BufferHandle handle) override
  {
    assert(handle == buffer);
    return bytes;
  }
  uint64_t StreamingBufferAlignment() const override
  {
    return 256;
  }
  void PublishStreamingWrites(BufferHandle handle, uint64_t offset,
                              uint64_t count) override
  {
    assert(handle == buffer && offset + count <= bytes.size());
    uploaded += count;
  }
  uint64_t InsertCompletionMarker() override
  {
    return ++markerCounter;
  }
  bool PollCompletionMarker(uint64_t) override
  {
    return gpuComplete;
  }
  void ReleaseCompletionMarker(uint64_t) override
  {
    assert(gpuComplete);
  }
  BufferHandle buffer{1, 1, DeviceId()};
  BufferHandle CreateBuffer(const BufferDesc& desc) override
  {
    ++created;
    bytes.resize(desc.size);
    metadata = {desc.size, desc.usage, desc.memory};
    return buffer;
  }
  void UpdateBuffer(BufferHandle handle, uint64_t offset,
                    std::span<const std::byte> data) override
  {
    (void)handle;
    (void)offset;
    (void)data;
    throw std::runtime_error("Synchronous terrain upload used");
  }
  void DestroyBuffer(BufferHandle handle) override
  {
    assert(handle == buffer);
    ++destroyed;
  }
  std::optional<BufferMetadata>
  GetBufferDesc(BufferHandle) const noexcept override
  {
    return metadata;
  }
  SwapchainHandle CreateSwapchain(Window&, const SwapchainConfig&) override
  {
    return {};
  }
  AcquiredSwapchainImage AcquireSwapchainImage(SwapchainHandle) override
  {
    return {};
  }
  bool DestroySwapchain(SwapchainHandle) override
  {
    return false;
  }
  ImageHandle CreateImage(const ImageDesc&) override
  {
    return {};
  }
  ImageViewHandle CreateImageView(const ImageViewDesc&) override
  {
    return {};
  }
#define VALID(Type)                                                            \
  bool IsValid(Type) const noexcept override                                   \
  {                                                                            \
    return true;                                                               \
  }
  VALID(SwapchainHandle)
  VALID(BufferHandle)
  VALID(DescriptorSetLayoutHandle)
  VALID(DescriptorSetHandle)
  VALID(PipelineLayoutHandle) VALID(SamplerHandle)
#undef VALID
#define CREATE(Name, Handle, Desc)                                             \
  Handle Name(const Desc&) override                                            \
  {                                                                            \
    return {};                                                                 \
  }
      CREATE(CreateShaderModule, ShaderModuleHandle,
             ShaderModuleDesc) CREATE(CreateDescriptorSetLayout,
                                      DescriptorSetLayoutHandle,
                                      DescriptorSetLayoutDesc)
          CREATE(CreateDescriptorSet, DescriptorSetHandle,
                 DescriptorSetDesc) CREATE(CreatePipelineLayout,
                                           PipelineLayoutHandle,
                                           PipelineLayoutDesc)
              CREATE(CreateSampler, SamplerHandle,
                     SamplerDesc) CREATE(CreatePipeline, GraphicsPipelineHandle,
                                         GraphicsPipelineDesc)
#undef CREATE
#define DESTROY(Name, Type)                                                    \
  void Name(Type) override {}
                  DESTROY(DestroyImage, ImageHandle) DESTROY(DestroyImageView,
                                                             ImageViewHandle)
                      DESTROY(DestroyPipeline, GraphicsPipelineHandle)
                          DESTROY(DestroyModule, ShaderModuleHandle) DESTROY(
                              DestroyDescriptorSetLayout,
                              DescriptorSetLayoutHandle)
                              DESTROY(DestroyDescriptorSet, DescriptorSetHandle)
                                  DESTROY(DestroyPipelineLayout,
                                          PipelineLayoutHandle)
                                      DESTROY(DestroySampler, SamplerHandle)
#undef DESTROY
                                          void UpdateDescriptorSet(
                                              DescriptorSetHandle,
                                              std::span<const DescriptorWrite>)
                                              override
  {
  }
  std::unique_ptr<IRenderCommandList> CreateCommandList(QueueType) override
  {
    return {};
  }
  RenderCapabilities GetCapabilities() const override
  {
    return {};
  }
  void Submit(const IRenderCommandList&) override {}
  void Present(SwapchainHandle) override {}
  void WaitIdle() override
  {
    ++waits;
    gpuComplete = true;
  }
};
class DrawCommands final : public IRenderCommandList
{
public:
  BufferHandle vertices{}, indices{};
  struct Draw
  {
    uint32_t count, first;
    int32_t base;
  };
  std::vector<Draw> draws;
  void Begin() override {}
  void End() override {}
  void BeginRendering(const RenderPassDesc&) override {}
  void EndRendering() override {}
  void BindPipeline(GraphicsPipelineHandle) override {}
  void BindDescriptorSet(uint32_t, DescriptorSetHandle) override {}
  void BindVertexBuffer(BufferHandle h, uint32_t stride) override
  {
    assert(stride == sizeof(Mesh3DVertex));
    vertices = h;
  }
  void BindIndexBuffer(BufferHandle h) override
  {
    indices = h;
  }
  void BindTexture(uint32_t, ImageHandle) override {}
  void Draw(PrimitiveTopology, uint32_t, uint32_t, uint32_t, uint32_t) override
  {
  }
  void DrawIndexed(uint32_t) override
  {
    assert(false);
  }
  void DrawIndexedRange(uint32_t count, uint32_t first, int32_t base) override
  {
    draws.push_back({count, first, base});
  }
  void Dispatch(uint32_t, uint32_t, uint32_t) override {}
  void BlitImage(ImageHandle, ImageHandle, const BlitRegion&,
                 ImageFilter) override
  {
  }
  void Clear() override {}
};
template <class F> void Eventually(F action)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!action())
  {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
static void TestMeshingThreadControl()
{
  WorldTerrain world;
  for (int i = 0; i < 12; ++i)
    world.set_voxel({int64_t(i) * 256, 0, 0}, {1, 1});
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic<int> entered = 0, active = 0, peak = 0;
  std::atomic<bool> duplicate = false;
  std::mutex mutex;
  std::unordered_set<RegionKey, RegionKeyHash> activeKeys;
  TerrainMeshScheduler scheduler(
      [&](const TerrainMeshInput& input)
      {
        {
          std::lock_guard lock(mutex);
          if (!activeKeys.insert(input.key).second)
            duplicate = true;
        }
        struct Guard
        {
          std::mutex& mutex;
          std::unordered_set<RegionKey, RegionKeyHash>& keys;
          RegionKey key;
          std::atomic<int>& active;
          ~Guard()
          {
            std::lock_guard lock(mutex);
            keys.erase(key);
            --active;
          }
        } guard{mutex, activeKeys, input.key, active};
        const auto count = ++active;
        auto maximum = peak.load();
        while (maximum < count && !peak.compare_exchange_weak(maximum, count))
        {
        }
        ++entered;
        gate.wait();
        return TerrainMesher::Build(input);
      });
  assert(scheduler.MeshingThreadCount() == 3 &&
         scheduler.Workers().size() == 3);
  scheduler.Update(world);
  Eventually(
      [&]
      {
        scheduler.Update(world);
        return entered == 3;
      });
  assert(active == 3 && peak == 3 && scheduler.Pending() == 3);
  // The builder gate stays closed through both resize operations: controls
  // must not join/destroy running futures or dispatch duplicate chunks.
  scheduler.SetMeshingThreadCount(1);
  scheduler.Update(world);
  assert(scheduler.MeshingThreadCount() == 1 &&
         scheduler.RetiringWorkers() == 2 && entered == 3);
  scheduler.SetMeshingThreadCount(5);
  scheduler.Update(world);
  Eventually(
      [&]
      {
        scheduler.Update(world);
        return entered == 5;
      });
  assert(active == 5 && peak == 5 && !duplicate);
  scheduler.SetMeshingThreadCount(2);
  assert(scheduler.RetiringWorkers() == 3 && scheduler.Pending() == 5);
  release.set_value();
  std::unordered_set<RegionKey, RegionKeyHash> completed;
  Eventually(
      [&]
      {
        scheduler.Update(world);
        while (auto result = scheduler.TakeReady())
        {
          assert(result->error.empty() && !result->mesh.indices.empty());
          assert(completed.insert(result->key).second);
        }
        return completed.size() == 12 && scheduler.Pending() == 0;
      });
  assert(scheduler.Workers().size() == 2 && scheduler.Scheduled() == 12 &&
         !duplicate);
  for (const auto& worker : scheduler.Workers())
    assert(!worker.retiring && worker.phase == TerrainWorkerPhase::Idle);
  for (auto count :
       {size_t(0), TerrainMeshScheduler::maximum_meshing_threads + 1})
  {
    bool rejected = false;
    try
    {
      scheduler.SetMeshingThreadCount(count);
    }
    catch (const std::invalid_argument&)
    {
      rejected = true;
    }
    assert(rejected && scheduler.MeshingThreadCount() == 2);
  }
}

int main()
{
  TestMeshingThreadControl();
  TerrainMeshArena arena(1024);
  const auto a = arena.Allocate(111, 32), b = arena.Allocate(222, 32);
  assert(a && b && a->offset % 32 == 0 && b->offset % 32 == 0);
  arena.Free(*a);
  arena.Free(*b);
  assert(arena.Used() == 0 && arena.Allocate(1024, 32));
  // Capture is an immutable shared snapshot, not a render-thread deep copy.
  WorldTerrain edited;
  edited.set_voxel({0, 0, 0}, {1, 1});
  auto captured = TerrainMeshInput::Capture(edited, {}, 0);
  edited.set_voxel({1, 0, 0}, {1, 1});
  assert(TerrainMesher::Build(captured).indices.size() == 36);
  assert(TerrainMesher::Build(edited, {}).indices.size() == 60);
  WorldTerrain world;
  world.set_voxel({0, 0, 0}, {1, 1});
  world.set_voxel({256, 0, 0}, {1, 1});
  world.set_voxel({1024, 0, 0}, {1, 1});
  TerrainMeshScheduler scheduler;
  auto initialWorkers = scheduler.Workers();
  assert(initialWorkers.size() == TerrainMeshScheduler::worker_limit);
  for (const auto& worker : initialWorkers)
    assert(worker.phase == TerrainWorkerPhase::Idle && !worker.chunk);
  size_t completed = 0;
  auto drain = [&]
  {
    scheduler.Update(world);
    while (auto result = scheduler.TakeReady())
    {
      assert(result->error.empty());
      ++completed;
    }
    return completed;
  };
  Eventually([&] { return drain() == 3; });
  const auto jobs = scheduler.Scheduled();
  bool completedWorker = false;
  for (const auto& worker : scheduler.Workers())
  {
    assert(worker.phase == TerrainWorkerPhase::Idle);
    if (worker.chunk)
    {
      completedWorker = true;
      assert(!worker.threadId.empty() && worker.milliseconds >= 0 &&
             !worker.lastResult.empty());
    }
  }
  assert(completedWorker);
  for (int i = 0; i < 5; ++i)
    scheduler.Update(world);
  assert(scheduler.Scheduled() == jobs);
  // An interior edit changes only its own mesh, not the adjacent chunk.
  world.set_voxel({128, 128, 128}, {1, 1});
  Eventually([&] { return drain() == 4; });
  assert(scheduler.Scheduled() == jobs + 1);
  const auto farStamp = TerrainMeshStamp::Read(world, {4, 0, 0, 0});
  world.set_voxel({255, 0, 0}, {1, 1});
  assert(TerrainMeshStamp::Read(world, {4, 0, 0, 0}) == farStamp);
  Eventually([&] { return drain() == 6; }); // center plus face neighbor only
  assert(scheduler.Scheduled() == jobs + 3);
  auto snapshot = TerrainMeshInput::Capture(world, {}, 0);
  world.clear();
  assert(
      !TerrainMesher::Build(snapshot).indices.empty()); // independent ownership
  scheduler.Update(world);
  assert(!scheduler.Contains({}));
  world.set_voxel({0, 0, 0}, {1, 1});
  scheduler.Update(world);
  world.clear();
  scheduler.Update(world);
  Eventually(
      [&]
      {
        scheduler.Update(world);
        return scheduler.Pending() == 0;
      });
  assert(!scheduler.TakeReady());
  assert(scheduler.Discarded() > 0);

  BufferDevice device;
  {
    TerrainMeshPool pool(device, 1024 * 1024);
    const auto stable = pool.Buffer();
    world.set_voxel({0, 0, 0}, {1, 1});
    world.set_voxel({1024, 0, 0}, {1, 1});
    auto update = [&]
    {
      const auto before = device.uploaded;
      pool.Update(world, 64, 1);
      assert(device.uploaded - before <= 64 &&
             pool.Stats().uploadedChunks <= 1);
      assert(pool.Buffer() == stable && device.created == 1 &&
             device.destroyed == 0 && device.waits == 0);
    };
    Eventually(
        [&]
        {
          update();
          return pool.Stats().chunks == 2 && pool.Stats().pending == 0;
        });
    DrawCommands commands;
    pool.Draw(commands);
    assert(commands.vertices == stable && commands.indices == stable &&
           commands.draws.size() == 2);
    for (const auto& draw : commands.draws)
    {
      assert(draw.count == 36 && draw.base >= 0);
      for (size_t i = 0; i < draw.count; ++i)
      {
        uint32_t index;
        std::memcpy(&index,
                    device.bytes.data() + (draw.first + i) * sizeof(uint32_t),
                    sizeof(index));
        Mesh3DVertex vertex;
        const auto address = (size_t(draw.base) + index) * sizeof(vertex);
        assert(address + sizeof(vertex) <= device.bytes.size());
        std::memcpy(&vertex, device.bytes.data() + address, sizeof(vertex));
        assert(vertex.position.x >= 0 && vertex.position.x <= 128.125f);
      }
    }
    const auto scheduled = pool.Stats().scheduled;
    world.set_voxel({1, 0, 0}, {1, 1});
    bool sawOld = false;
    Eventually(
        [&]
        {
          update();
          DrawCommands during;
          pool.Draw(during);
          size_t count = 0;
          for (auto draw : during.draws)
            count += draw.count;
          assert(count == 72 ||
                 count == 96); // previous mesh survives partial upload
          sawOld |= count == 72;
          return pool.Stats().pending == 0 &&
                 pool.Stats().scheduled == scheduled + 1;
        });
    assert(sawOld);
    assert(pool.Stats().scheduled == scheduled + 1);
    const auto uploaded = pool.Stats().uploadWorker;
    assert(!uploaded.threadId.empty() &&
           uploaded.threadId != TerrainThreadId());
    device.gpuComplete = false;
    world.clear();
    update();
    DrawCommands empty;
    pool.Draw(empty);
    assert(empty.draws.empty());
    const auto retiredBytes = pool.Stats().bufferUsed;
    assert(retiredBytes > 0);
    update();
    assert(pool.Stats().bufferUsed == retiredBytes && device.waits == 0);
    device.gpuComplete = true;
    Eventually(
        [&]
        {
          update();
          return pool.Stats().bufferUsed == 0;
        });
  }
  assert(device.created == 1 && device.destroyed == 1);
  std::cout << "Incremental terrain mesh tests passed\n";
}
