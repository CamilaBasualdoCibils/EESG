#include "World/CameraView.hpp"
#include "World/Terrain/TerrainSystem.hpp"
#include <Lighthouse/Backends/Render/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Backends/Render/Vulkan/VulkanBackend.hpp>
#include <Lighthouse/Engine/Rendering/Renderer.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <set>
#include <source_location>
#include <thread>
#include <tuple>
using namespace EESG;
using namespace Lighthouse;
namespace
{
void Check(bool value,
           std::source_location location = std::source_location::current())
{
  if (!value)
    throw std::runtime_error("Terrain check failed at line " +
                             std::to_string(location.line()));
}
template <class F> void Rejects(F f)
{
  bool rejected = false;
  try
  {
    f();
  }
  catch (const std::exception&)
  {
    rejected = true;
  }
  Check(rejected);
}
void Memory()
{
  TerrainHeader header;
  header.counts = {1, 1, 1, 0};
  TerrainNode node;
  node.state.w = TerrainLeaf;
  std::vector<std::byte> bytes(sizeof(header) + sizeof(node));
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), &node, sizeof(node));
  TerrainOctree snapshot(std::make_shared<TerrainSnapshotMemory>(bytes, 7));
  Check(snapshot.Revision() == 7 && snapshot.Node(0).state.w == TerrainLeaf);
  Rejects([&] { snapshot.Node(1); });
  auto owner = std::make_shared<std::vector<std::byte>>(bytes);
  TerrainOctree mapped(std::make_shared<TerrainMappedMemory>(*owner, 8, owner));
  owner.reset();
  Check(mapped.Header().counts.x == 1 && mapped.Revision() == 8);
  Rejects(
      [&]
      {
        TerrainOctree truncated(std::make_shared<TerrainSnapshotMemory>(
            std::span(bytes).first(8), 1));
      });
  header.counts.x = TerrainNodeCapacity + 1;
  std::memcpy(bytes.data(), &header, sizeof(header));
  Rejects(
      [&]
      {
        TerrainOctree corrupt(
            std::make_shared<TerrainSnapshotMemory>(bytes, 1));
      });
  TerrainObserverManager observers;
  auto observer = observers.Register();
  Check(observer->maxCellSizePixels == 4.0f &&
        observer->viewportSize == UVec2(1280, 720));
  for (float value : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()})
  {
    auto bad = *observer;
    bad.maxCellSizePixels = value;
    Rejects([&] { observers.Configure(observer, bad); });
  }
  auto bad = *observer;
  bad.viewportSize.y = 0;
  Rejects([&] { observers.Configure(observer, bad); });
  bad = *observer;
  bad.verticalFovDegrees = 180;
  Rejects([&] { observers.Configure(observer, bad); });
  CameraView camera;
  camera.SetResolution({1920, 1080});
  camera.verticalFov = 60;
  camera.UpdateTerrainObserver(observers, {3, 4, 5});
  auto cameraObserver = camera.GetTerrainObserver();
  Check(cameraObserver->viewportSize == UVec2(1920, 1080) &&
        cameraObserver->verticalFovDegrees == 60);
  cameraObserver->maxCellSizePixels = 7;
  camera.UpdateTerrainObserver(observers, {6, 7, 8});
  Check(cameraObserver->position == WorldPosition(6, 7, 8) &&
        cameraObserver->maxCellSizePixels == 7);
  camera.ReleaseTerrainObserver();
  cameraObserver.reset();
  TerrainSystem terrain;
  Rejects([&] { terrain.SetRootCoverage({.count = {0, 1, 1}}); });
  Rejects([&]
          { terrain.SetRootCoverage({.count = {TerrainNodeCapacity, 2, 1}}); });
  Rejects(
      [&]
      {
        terrain.SetRootCoverage(
            {.minimum = {std::numeric_limits<int32_t>::max(), 0, 0},
             .count = {2, 1, 1}});
      });
  observer.reset();
  Check(observers.Observers().empty());
}
using LeafKey = std::tuple<int, int, int, int, uint32_t>;
using LeafSet = std::set<LeafKey>;
LeafSet Leaves(const TerrainOctree& tree)
{
  LeafSet result;
  const auto header = tree.Header();
  uint32_t visited = 0, split = 0;
  for (auto level : header.levels)
  {
    visited += level.x;
    split += level.z;
    if (!header.counts.w)
      Check(level.x == level.y + level.z + level.w);
  }
  Check(visited == header.counts.x &&
        header.counts.x == header.counts.y + 8 * split);
  for (uint32_t i = 0; i < header.counts.x; ++i)
  {
    const auto node = tree.Node(i);
    Check(node.region.w >= 0 &&
          uint32_t(node.region.w) <= TerrainMetaChunkLevels);
    if (i < header.counts.y)
      Check(node.state.y == InvalidTerrainNode);
    else
    {
      const auto parent = tree.Node(node.state.y);
      Check(parent.state.w == TerrainBranch && i >= parent.state.x &&
            i < parent.state.x + 8);
      Check(parent.region.w == node.region.w + 1);
      for (int axis = 0; axis < 3; ++axis)
      {
        Check(node.region[axis] >= parent.region[axis]);
        Check(int64_t(node.region[axis]) + (int64_t(1) << node.region.w) <=
              int64_t(parent.region[axis]) + (int64_t(1) << parent.region.w));
      }
    }
    if (node.state.w == TerrainBranch)
      Check(node.state.x + 8 <= header.counts.x);
    if (node.state.w != TerrainLeaf)
      continue;
    Check(node.state.x == InvalidTerrainNode);
    Check(node.state.z >= uint32_t(node.region.w) &&
          node.state.z <= uint32_t(node.region.w) + TerrainChunkDepth);
    result.emplace(node.region.x, node.region.y, node.region.z, node.region.w,
                   node.state.z);
  }
  Check(result.size() == header.counts.z);
  return result;
}
void PointDetail(const TerrainOctree& tree, const TerrainObserver& observer)
{
  const auto header = tree.Header();
  const double scale =
      double(observer.viewportSize.y) /
      (2 * std::tan(glm::radians(double(observer.verticalFovDegrees)) / 2));
  uint64_t volume = 0;
  for (uint32_t i = 0; i < header.counts.x; ++i)
  {
    const auto node = tree.Node(i);
    const auto minimum = WorldPosition(node.region) * TerrainChunkWidth;
    const double width = TerrainChunkWidth * (uint64_t(1) << node.region.w);
    const auto delta =
        observer.position -
        glm::clamp(observer.position, minimum, minimum + WorldPosition(width));
    const double allowed =
        observer.maxCellSizePixels * glm::length(delta) / scale;
    const double finest = TerrainVoxelWidth * (uint64_t(1) << node.region.w);
    Check(node.state.w == TerrainLeaf ||
          node.state.w == TerrainBranch); // No radial cutoff.
    if (node.state.w == TerrainBranch)
    {
      Check(finest > allowed * (1 - 1e-5));
      continue;
    }
    const double cell = TerrainVoxelWidth * (uint64_t(1) << node.state.z);
    Check(cell <= allowed * (1 + 1e-5) ||
          (node.region.w == 0 && node.state.z == 0));
    if (node.state.z < uint32_t(node.region.w) + TerrainChunkDepth)
      Check(cell * 2 > allowed * (1 - 1e-5));
    volume += uint64_t(1) << (3 * node.region.w);
  }
  // Selected leaves exactly partition the finite roots, including distant
  // space.
  Check(volume == uint64_t(header.counts.y) *
                      (uint64_t(1) << (3 * TerrainMetaChunkLevels)));
}
TerrainNode FindLeaf(const TerrainOctree& tree, IVec3 position)
{
  for (uint32_t root = 0; root < tree.Header().counts.y; ++root)
  {
    auto node = tree.Node(root);
    const int64_t width = int64_t(1) << node.region.w;
    bool contains = true;
    for (int axis = 0; axis < 3; ++axis)
      contains &= position[axis] >= node.region[axis] &&
                  int64_t(position[axis]) < int64_t(node.region[axis]) + width;
    if (!contains)
      continue;
    while (node.state.w == TerrainBranch)
    {
      const auto middle =
          IVec3(node.region) + (IVec3(1) << (node.region.w - 1));
      const auto child = uint32_t(position.x >= middle.x) +
                         2 * uint32_t(position.y >= middle.y) +
                         4 * uint32_t(position.z >= middle.z);
      node = tree.Node(node.state.x + child);
    }
    Check(node.state.w == TerrainLeaf);
    return node;
  }
  throw std::runtime_error("Missing terrain coverage");
}
void GPU(bool vulkan)
{
  std::unique_ptr<IGraphicsBackend> backend =
      vulkan
          ? std::unique_ptr<IGraphicsBackend>(std::make_unique<VulkanBackend>())
          : std::unique_ptr<IGraphicsBackend>(
                std::make_unique<OpenGLBackend>());
  std::string diagnostic;
  Check(backend->Initialize(diagnostic));
  {
    Renderer renderer(backend->CreateDevice(
        {.applicationName = "Terrain pixel residency tests"}));
    auto& device = *renderer.GetRenderDevice();
    SimpleRenderGraphAllocator allocator(device);
    TerrainSystem terrain;
    TerrainObserverManager observers;
    TerrainObserver initial;
    initial.position = {7, 11, 13};
    auto observer = observers.Register(initial);
    RenderGraph graph;
    auto build = [&]
    {
      graph.Clear();
      terrain.BuildGraph(graph, device, observers);
      graph.Compile();
    };
    build();
    auto inspection = graph.GetInspectionData();
    Check(inspection.passes.size() == TerrainMetaChunkLevels + 2);
    for (auto& pass : inspection.passes)
      Check(pass.name != "Terrain inspector readback");
    graph.Execute(&renderer, &allocator);
    graph.Drain();
    Check(!terrain.Snapshot());
    terrain.EnableInspector(true);
    build();
    inspection = graph.GetInspectionData();
    Check(inspection.passes.back().name == "Terrain inspector readback");
    Check(inspection.passes.back().reads[0] ==
          inspection.passes[inspection.passes.size() - 2].writes[0]);
    auto capture = [&](bool overflow = false)
    {
      graph.Drain();
      terrain.PollReadbacks();
      const auto revision =
          terrain.Snapshot() ? terrain.Snapshot()->Revision() : 0;
      graph.Execute(&renderer, &allocator);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
      do
      {
        terrain.PollReadbacks();
        Check(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
      } while (!terrain.Snapshot() ||
               terrain.Snapshot()->Revision() == revision);
      Check(terrain.Diagnostic().empty());
      if (!overflow)
        Check(terrain.Snapshot()->Header().counts.w == 0);
      return terrain.Snapshot();
    };
    const auto fine = capture();
    const auto fineLeaves = Leaves(*fine);
    Check(!fineLeaves.empty() && fine->Header().counts.y == 8);
    PointDetail(*fine, *observer);
    if (TerrainMetaChunkLevels > 0)
    {
      Check(std::any_of(fineLeaves.begin(), fineLeaves.end(),
                        [](auto leaf) { return std::get<3>(leaf) > 0; }));
      Check(std::any_of(fineLeaves.begin(), fineLeaves.end(),
                        [](auto leaf) { return std::get<3>(leaf) == 0; }));
    }
    observer->maxCellSizePixels = 16;
    const auto coarse = capture();
    const auto coarseLeaves = Leaves(*coarse);
    PointDetail(*coarse, *observer);
    Check(coarseLeaves.size() <= fineLeaves.size());
    for (auto [x, y, z, level, lod] : fineLeaves)
      Check(FindLeaf(*coarse, {x, y, z}).region.w >= level);
    // Doubling viewport pixels and the threshold must preserve identical
    // detail.
    observer->maxCellSizePixels = 8;
    observer->viewportSize *= 2u;
    Check(Leaves(*capture()) == fineLeaves);
    observer->maxCellSizePixels = 4;
    observer->viewportSize = {1280, 720};
    observer->verticalFovDegrees = 90;
    const auto wide = capture();
    PointDetail(*wide, *observer);
    Check(Leaves(*wide).size() <= fineLeaves.size());
    observer->verticalFovDegrees = 45;
    auto stricter = observers.Register(*observer);
    observer->maxCellSizePixels = 16;
    Check(Leaves(*capture()) == fineLeaves);
    stricter.reset();
    observer->maxCellSizePixels = 4;
    observer->position = {1e9, 1e9, 1e9};
    const auto distant = capture();
    PointDetail(*distant, *observer);
    Check(distant->Header().counts.x == 8 && distant->Header().counts.z == 8);
    for (auto [x, y, z, level, lod] : Leaves(*distant))
      Check(uint32_t(level) == TerrainMetaChunkLevels &&
            lod == TerrainMetaChunkLevels + TerrainChunkDepth);
    observer->position = {-100, -50, -25};
    PointDetail(*capture(), *observer);
    // Coverage is world-defined, remains finite, and updates without a graph
    // rebuild.
    terrain.SetRootCoverage({.minimum = {2, -1, -1}, .count = {1, 2, 2}});
    const auto shifted = capture();
    Check(shifted->Header().counts.y == 4);
    PointDetail(*shifted, *observer);
    const int span = 1 << TerrainMetaChunkLevels;
    for (auto [x, y, z, level, lod] : Leaves(*shifted))
      Check(x >= 2 * span && x < 3 * span);
    terrain.SetRootCoverage({});
    observer->type = TerrainObserverType::Pyramid;
    observer->position = {0, 0, 0};
    observer->viewProjection =
        glm::perspectiveRH_NO(glm::radians(60.f), 1280.f / 720.f, .1f, 100.f);
    const auto frustum = Leaves(*capture());
    Check(!frustum.empty());
    for (auto [x, y, z, level, lod] : frustum)
    {
      Check(z < 0);
      // Analytic -Z perspective: closest camera depth, not Euclidean range.
      const double nearestDepth = std::max(
          -TerrainChunkWidth * (double(z) + double(uint64_t(1) << level)), 0.0);
      const double pixelScale =
          720.0 / (2.0 * std::tan(glm::radians(60.0) * 0.5));
      const double allowed =
          observer->maxCellSizePixels * nearestDepth / pixelScale;
      const double cell = TerrainVoxelWidth * (uint64_t(1) << lod);
      Check(cell <= allowed * (1 + 1e-5) || (level == 0 && lod == 0));
      if (lod < uint32_t(level) + TerrainChunkDepth)
        Check(cell * 2 > allowed * (1 - 1e-5));
    }
    // Orthographic projection has constant pixel scale: 5 px/m, hence LOD 2 at
    // 4 px.
    observer->viewProjection =
        glm::orthoRH_NO(-128.f, 128.f, -128.f, 128.f, .1f, 100.f);
    const auto ortho = Leaves(*capture());
    Check(!ortho.empty());
    for (auto [x, y, z, level, lod] : ortho)
      Check(lod == 2);
    observer->viewProjection *= 1e-4f;
    Check(Leaves(*capture()) == ortho);
    observer->enabled = false;
    Check(Leaves(*capture()).empty());
    observer->enabled = true;
    observer->type = TerrainObserverType::Point;
    observer->position = initial.position;
    if (TerrainMetaChunkLevels >= 5)
    {
      observer->maxCellSizePixels = 1e-8f;
      const auto overflow = capture(true);
      Check(overflow->Header().counts.w == 1);
      Check(overflow->Header().counts.x <= TerrainNodeCapacity);
      Leaves(*overflow);
      observer->maxCellSizePixels = 4;
    }
    graph.Execute(&renderer, &allocator);
    RenderGraph replacement;
    terrain.BuildGraph(replacement, device, observers);
    replacement.Compile();
    graph = std::move(replacement);
    Check(Leaves(*capture()) == fineLeaves);
    terrain.EnableInspector(false);
    build();
    terrain.PollReadbacks();
    const auto previous = terrain.Snapshot()->Revision();
    graph.Execute(&renderer, &allocator);
    graph.Drain();
    terrain.PollReadbacks();
    Check(terrain.Snapshot()->Revision() == previous);
    graph.Clear();
    terrain.Release();
  }
  backend->Shutdown();
}
} // namespace
int main()
{
  try
  {
    Memory();
    GPU(false);
    GPU(true);
    std::cout << "Terrain pixel residency checks passed\n";
  }
  catch (const std::exception& e)
  {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
