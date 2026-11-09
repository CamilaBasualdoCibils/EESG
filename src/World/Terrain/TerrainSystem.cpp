#include "TerrainSystem.hpp"
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <cstring>
#include <fstream>
#include <iterator>

namespace EESG
{
using namespace Lighthouse;
using namespace Lighthouse;
namespace
{
constexpr uint64_t QueueBytes = 4 + uint64_t(TerrainNodeCapacity) * 4;
constexpr uint64_t InputBytes = 4 * 1024 * 1024;
std::string ShaderSource(const char* name)
{
  std::ifstream stream(std::string(EESG_TERRAIN_RESOURCE_ROOT) + "/" + name);
  if (!stream)
    throw std::runtime_error("Cannot open terrain compute shader");
  std::string source((std::istreambuf_iterator<char>(stream)), {});
  source.insert(
      source.find('\n') + 1,
      "#define META_LEVELS " + std::to_string(TerrainMetaChunkLevels) +
          "\n#define CHUNK_DEPTH " + std::to_string(TerrainChunkDepth) + "u" +
          "\n#define NODE_CAPACITY " + std::to_string(TerrainNodeCapacity) +
          "u\n");
  return source;
}
BufferHandle Buffer(FrameGraphPassResources& resources,
                    RenderGraphResource resource, bool imported = false)
{
  return imported ? resources.get<ImportedRenderGraphBuffer>(resource).handle
                  : resources.get<RenderGraphBuffer>(resource).handle;
}
} // namespace
TerrainSystem::~TerrainSystem()
{
  Release();
}
void TerrainSystem::Initialize(IRenderDevice& device)
{
  if (device_)
  {
    if (device_ != &device)
      throw std::logic_error("Terrain system belongs to another device");
    return;
  }
  device_ = &device;
  try
  {
    terrain_ = device.CreateBuffer(
        {.size = TerrainBufferBytes,
         .usage = BufferUsage::Storage | BufferUsage::TransferSrc});
    DescriptorSetLayoutDesc set;
    for (uint32_t binding = 0; binding < 5; ++binding)
      set.bindings.push_back({binding,
                              binding == 4 ? DescriptorType::UniformBuffer
                                           : DescriptorType::StorageBuffer,
                              1, ShaderStageFlags::Compute});
    setLayout_ = device.CreateDescriptorSetLayout(set);
    layout_ = device.CreatePipelineLayout({.sets = {setLayout_}});
    residencyShader_ = device.CreateShaderModule(ShaderCompiler::Compile(
        ShaderSource("Residency.comp.glsl"), {.stage = ShaderStage::Compute}));
    residency_ = device.CreatePipeline(
        {.layout = layout_, .computeShader = residencyShader_});
  }
  catch (...)
  {
    Release();
    throw;
  }
}
void TerrainSystem::SetRootCoverage(TerrainRootCoverage coverage)
{
  uint64_t roots = 1;
  constexpr int64_t span = int64_t(1) << TerrainMetaChunkLevels;
  for (int axis = 0; axis < 3; ++axis)
  {
    if (!coverage.count[axis] ||
        coverage.count[axis] > TerrainNodeCapacity / roots)
      throw std::invalid_argument(
          "Root coverage must contain 1 to TerrainNodeCapacity roots");
    roots *= coverage.count[axis];
    const int64_t low = int64_t(coverage.minimum[axis]) * span;
    const int64_t high =
        (int64_t(coverage.minimum[axis]) + coverage.count[axis]) * span;
    if (low < std::numeric_limits<int32_t>::min() ||
        high > int64_t(std::numeric_limits<int32_t>::max()) + 1)
      throw std::out_of_range("Root coverage exceeds terrain coordinate range");
  }
  coverage_ = coverage;
}
std::vector<std::byte>
TerrainSystem::ObserverBytes(const TerrainObserverManager& manager)
{
  struct Observer
  {
    Vec4 position;   // xyz metres, w maximum cell size in pixels
    Vec4 projection; // x pixel scale, y observer type
    Vec4 clipW;      // World -> clip-W row, for conservative perspective depth
    std::array<Vec4, 6> planes;
  };
  static_assert(sizeof(Observer) == 144);
  std::vector<Observer> observers;
  for (const auto& handle : manager.Observers())
  {
    const auto& observer = *handle;
    if (!observer.enabled)
      continue;
    TerrainObserverManager::Validate(observer);
    Observer gpu{};
    gpu.position = Vec4(Vec3(observer.position), observer.maxCellSizePixels);
    gpu.projection.x = float(
        double(observer.viewportSize.y) /
        (2.0 *
         std::tan(glm::radians(double(observer.verticalFovDegrees)) * 0.5)));
    gpu.projection.y = float(observer.type);
    if (observer.type == TerrainObserverType::Pyramid)
    {
      auto row = [&](int i)
      {
        return Vec4(
            observer.viewProjection[0][i], observer.viewProjection[1][i],
            observer.viewProjection[2][i], observer.viewProjection[3][i]);
      };
      gpu.clipW = row(3);
      gpu.projection.x = std::max(
          float(observer.viewportSize.x) * 0.5f * glm::length(Vec3(row(0))),
          float(observer.viewportSize.y) * 0.5f * glm::length(Vec3(row(1))));
      for (int axis = 0; axis < 3; ++axis)
      {
        gpu.planes[axis * 2] = row(3) + row(axis);
        gpu.planes[axis * 2 + 1] = row(3) - row(axis);
      }
    }
    if (!std::isfinite(gpu.projection.x) || gpu.projection.x <= 0)
      throw std::invalid_argument(
          "Observer projection has an invalid pixel scale");
    observers.push_back(gpu);
  }
  // Every enabled observer evaluates the same finite domain. Coverage is
  // independent of the observer's position and requested detail.
  std::vector<IVec4> roots;
  if (!observers.empty())
  {
    constexpr int64_t span = int64_t(1) << TerrainMetaChunkLevels;
    for (uint32_t z = 0; z < coverage_.count.z; ++z)
      for (uint32_t y = 0; y < coverage_.count.y; ++y)
        for (uint32_t x = 0; x < coverage_.count.x; ++x)
          roots.emplace_back(int32_t((int64_t(coverage_.minimum.x) + x) * span),
                             int32_t((int64_t(coverage_.minimum.y) + y) * span),
                             int32_t((int64_t(coverage_.minimum.z) + z) * span),
                             0);
  }
  const size_t observerOffset = 16 + roots.size() * sizeof(IVec4);
  const size_t bytes = observerOffset + observers.size() * sizeof(Observer);
  if (bytes > InputBytes)
    throw std::length_error("Terrain observer upload capacity exceeded");
  std::vector<std::byte> result(bytes);
  const UVec4 header{uint32_t(roots.size()), uint32_t(observers.size()),
                     uint32_t(observerOffset / 4), 0};
  std::memcpy(result.data(), &header, sizeof(header));
  if (!roots.empty())
    std::memcpy(result.data() + 16, roots.data(), roots.size() * sizeof(IVec4));
  if (!observers.empty())
    std::memcpy(result.data() + observerOffset, observers.data(),
                observers.size() * sizeof(Observer));
  return result;
}
void TerrainSystem::Compute(RenderGraphExecutePassContext& execute,
                            BufferHandle inputQueue, BufferHandle outputQueue,
                            BufferHandle terrain, BufferHandle input,
                            uint32_t mode, uint32_t level,
                            GraphicsPipelineHandle pipeline)
{
  const UVec4 stage{mode, level, 0, 0};
  const auto uniform = device_->CreateBuffer(
      {.size = sizeof(stage),
       .usage = BufferUsage::Uniform | BufferUsage::TransferSrc,
       .initialData = std::as_bytes(std::span(&stage, 1))});
  execute.Defer([device = device_, uniform]
                { device->DestroyBuffer(uniform); });
  const auto set = device_->CreateDescriptorSet({.layout = setLayout_});
  execute.Defer([device = device_, set] { device->DestroyDescriptorSet(set); });
  const std::array<DescriptorWrite, 5> writes{
      {{0, 0, DescriptorBufferInfo{inputQueue}},
       {1, 0, DescriptorBufferInfo{outputQueue}},
       {2, 0, DescriptorBufferInfo{terrain}},
       {3, 0, DescriptorBufferInfo{input}},
       {4, 0, DescriptorBufferInfo{uniform}}}};
  device_->UpdateDescriptorSet(set, writes);
  auto& commands = *execute.commandList;
  commands.Begin();
  if (mode == 1)
    commands.CopyBuffer(uniform, outputQueue, 8, 0,
                        4); // reset next queue before dispatch
  commands.BindPipeline(pipeline);
  commands.BindDescriptorSet(0, set);
  commands.Dispatch(mode == 0 ? 1 : (TerrainNodeCapacity + 63) / 64, 1, 1);
  commands.End();
}
void TerrainSystem::BuildGraph(RenderGraph& graph, IRenderDevice& device,
                               TerrainObserverManager& observers)
{
  Initialize(device);
  auto tree = graph.Import(
      "Terrain residency",
      ImportedRenderGraphBuffer::Desc{.size = TerrainBufferBytes,
                                      .usage = BufferUsage::Storage |
                                               BufferUsage::TransferSrc},
      ImportedRenderGraphBuffer{terrain_});
  struct Seed
  {
    RenderGraphResource tree, input, queue;
  };
  const auto seed = graph.AddPass<Seed>(
      "Terrain residency roots",
      [&](RenderGraph::Builder& b, Seed& d)
      {
        d.tree = b.Write(tree);
        d.input = b.Write(b.Create<RenderGraphBuffer>(
            "Terrain observers",
            {InputBytes, BufferUsage::Storage | BufferUsage::TransferDst}));
        d.queue = b.Write(b.Create<RenderGraphBuffer>(
            "Terrain root queue",
            {QueueBytes, BufferUsage::Storage | BufferUsage::TransferDst}));
      },
      [this, &observers](const Seed& d, FrameGraphPassResources& r,
                         RenderGraphExecutePassContext& e)
      {
        PollReadbacks();
        ++revision_;
        std::vector<std::byte> bytes;
        try
        {
          bytes = ObserverBytes(observers);
          diagnostic_.clear();
        }
        catch (const std::exception& error)
        {
          diagnostic_ = error.what();
          bytes.resize(16);
        }
        const auto upload =
            device_->CreateBuffer({.size = bytes.size(),
                                   .usage = BufferUsage::TransferSrc,
                                   .initialData = bytes});
        e.Defer([device = device_, upload] { device->DestroyBuffer(upload); });
        // Upload and seed are recorded in the same graph pass.
        const UVec4 stage{0};
        const auto uniform = device_->CreateBuffer(
            {.size = 16,
             .usage = BufferUsage::Uniform,
             .initialData = std::as_bytes(std::span(&stage, 1))});
        e.Defer([device = device_, uniform]
                { device->DestroyBuffer(uniform); });
        const auto set = device_->CreateDescriptorSet({setLayout_});
        e.Defer([device = device_, set] { device->DestroyDescriptorSet(set); });
        const auto queue = Buffer(r, d.queue), input = Buffer(r, d.input),
                   tree = Buffer(r, d.tree, true);
        const std::array<DescriptorWrite, 5> writes{
            {{0, 0, DescriptorBufferInfo{queue}},
             {1, 0, DescriptorBufferInfo{queue}},
             {2, 0, DescriptorBufferInfo{tree}},
             {3, 0, DescriptorBufferInfo{input}},
             {4, 0, DescriptorBufferInfo{uniform}}}};
        device_->UpdateDescriptorSet(set, writes);
        auto& c = *e.commandList;
        c.Begin();
        c.CopyBuffer(upload, input, 0, 0, bytes.size());
        c.BindPipeline(residency_);
        c.BindDescriptorSet(0, set);
        c.Dispatch(1, 1, 1);
        c.End();
      });
  tree = seed.tree;
  auto queue = seed.queue;
  const auto input = seed.input;
  struct Traverse
  {
    RenderGraphResource tree, input, in, out;
  };
  for (int level = int(TerrainMetaChunkLevels); level >= 0; --level)
  {
    const auto pass = graph.AddPass<Traverse>(
        "Terrain residency level " + std::to_string(level),
        [&](RenderGraph::Builder& b, Traverse& d)
        {
          d.tree = b.Write(b.Read(tree));
          d.input = b.Read(input);
          d.in = b.Read(queue);
          d.out = b.Write(b.Create<RenderGraphBuffer>(
              "Terrain next queue",
              {QueueBytes, BufferUsage::Storage | BufferUsage::TransferDst}));
          if (level == 0)
            b.SetSideEffect();
        },
        [this, level](const Traverse& d, FrameGraphPassResources& r,
                      RenderGraphExecutePassContext& e)
        {
          Compute(e, Buffer(r, d.in), Buffer(r, d.out), Buffer(r, d.tree, true),
                  Buffer(r, d.input), 1, level, residency_);
        });
    tree = pass.tree;
    queue = pass.out;
  }
  if (!inspectorEnabled_)
    return;
  for (auto& slot : readbacks_)
    if (!slot.buffer)
      slot.buffer = device.CreateBuffer(
          {.size = TerrainBufferBytes,
           .usage = BufferUsage::TransferDst,
           .memory = {MemoryLocation::HostCached,
                      MemoryAccess::CPURead | MemoryAccess::GPUWrite}});
  struct Download
  {
    RenderGraphResource tree;
    std::array<RenderGraphResource, 3> slots;
  };
  std::array<RenderGraphResource, 3> slots;
  for (size_t i = 0; i < slots.size(); ++i)
    slots[i] = graph.Import("Terrain readback " + std::to_string(i),
                            ImportedRenderGraphBuffer::Desc{
                                TerrainBufferBytes, BufferUsage::TransferDst},
                            ImportedRenderGraphBuffer{readbacks_[i].buffer});
  graph.AddPass<Download>(
      "Terrain inspector readback",
      [&](RenderGraph::Builder& b, Download& d)
      {
        d.tree = b.Read(tree);
        for (size_t i = 0; i < slots.size(); ++i)
          d.slots[i] = b.Write(slots[i]);
        b.SetSideEffect();
      },
      [this](const Download& d, FrameGraphPassResources& r,
             RenderGraphExecutePassContext& e)
      {
        auto& c = *e.commandList;
        c.Begin();
        if (inspectorEnabled_)
        {
          size_t index = 0;
          while (index < readbacks_.size() && readbacks_[index].pending)
            ++index;
          if (index == readbacks_.size())
            ++skippedReadbacks_;
          else
          {
            c.CopyBuffer(Buffer(r, d.tree, true),
                         Buffer(r, d.slots[index], true), 0, 0,
                         TerrainBufferBytes);
            auto& slot = readbacks_[index];
            if (!slot.semaphore)
              slot.semaphore =
                  device_->CreateSemaphore(SemaphoreType::Timeline);
            ++slot.value;
            e.signals.push_back({slot.semaphore, slot.value});
            e.afterSubmit = [this, index]
            {
              readbacks_[index].pending = true;
              readbacks_[index].revision = revision_;
            };
          }
        }
        c.End();
      });
}
void TerrainSystem::PollReadbacks()
{
  if (!device_)
    return;
  for (auto& slot : readbacks_)
  {
    if (!slot.pending || !device_->WaitSemaphore(slot.semaphore, slot.value, 0))
      continue;
    if (!snapshot_ || slot.revision > snapshot_->Revision())
    {
      device_->InvalidateMappedMemoryRange(slot.buffer, 0, TerrainBufferBytes);
      auto mapped = device_->MapMemory(slot.buffer);
      auto memory =
          std::make_shared<TerrainSnapshotMemory>(mapped, slot.revision);
      device_->UnmapMemory(slot.buffer);
      snapshot_ = std::make_shared<TerrainOctree>(std::move(memory));
    }
    slot.pending = false;
  }
}
void TerrainSystem::Release()
{
  if (!device_)
    return;
  device_->WaitIdle();
  for (auto& slot : readbacks_)
  {
    if (slot.semaphore)
      device_->DestroySemaphore(slot.semaphore);
    if (slot.buffer)
      device_->DestroyBuffer(slot.buffer);
    slot = {};
  }
  if (residency_)
    device_->DestroyPipeline(residency_);
  if (residencyShader_)
    device_->DestroyModule(residencyShader_);
  if (layout_)
    device_->DestroyPipelineLayout(layout_);
  if (setLayout_)
    device_->DestroyDescriptorSetLayout(setLayout_);
  if (terrain_)
    device_->DestroyBuffer(terrain_);
  terrain_ = {};
  residency_ = {};
  residencyShader_ = {};
  layout_ = {};
  setLayout_ = {};
  device_ = nullptr;
}
} // namespace EESG
