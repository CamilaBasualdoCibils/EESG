#include "World/CameraView.hpp"
#include "World/Entity/Player.hpp"
#include "World/Entity/Transform.hpp"
#include "World/Terrain/PerlinTerrainGenerator.hpp"
#include "World/Terrain/TerrainObserverManager.hpp"
#include "World/World.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <iostream>
#include <set>
#include <thread>
#include <unordered_set>
using namespace EESG;
template <class F> void Eventually(F action)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!action())
  {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
struct CountingGenerator : ITerrainGenerator
{
  mutable std::atomic<int> calls = 0;
  double Density(WorldPosition p) const override
  {
    return 16 - p.y;
  }
  std::unique_ptr<TerrainRegion::Tree>
  Generate(RegionKey key, uint32_t lod,
           std::shared_ptr<std::pmr::memory_resource> memory = {},
           std::stop_token stop = {}) const override
  {
    ++calls;
    return ITerrainGenerator::Generate(key, lod, std::move(memory), stop);
  }
};
struct TestStorage : ITerrainStorage
{
  int loads = 0, saves = 0;
  std::optional<uint32_t> stored;
  bool miss = false;
  std::optional<uint32_t> Query(RegionKey) const override
  {
    return stored;
  }
  std::unique_ptr<TerrainRegion::Tree>
  Load(RegionKey, uint32_t,
       std::shared_ptr<std::pmr::memory_resource> memory) override
  {
    ++loads;
    if (miss)
      return {};
    auto tree = std::make_unique<TerrainRegion::Tree>(8, std::move(memory));
    tree->paint({{}, 0}, {7, 1});
    return tree;
  }
  void Save(RegionKey, uint32_t, const TerrainRegion::Tree&) override
  {
    ++saves;
  }
};
struct GatedGenerator : ITerrainGenerator
{
  mutable std::atomic<int> calls = 0;
  mutable std::promise<std::string> entered;
  std::shared_future<void> release;
  explicit GatedGenerator(std::shared_future<void> gate)
      : release(std::move(gate))
  {
  }
  double Density(WorldPosition p) const override
  {
    return 16 - p.y;
  }
  std::unique_ptr<TerrainRegion::Tree>
  Generate(RegionKey key, uint32_t lod,
           std::shared_ptr<std::pmr::memory_resource> memory = {},
           std::stop_token stop = {}) const override
  {
    if (calls.fetch_add(1) == 0)
    {
      entered.set_value(TerrainThreadId());
      release.wait();
    }
    return ITerrainGenerator::Generate(key, lod, std::move(memory), stop);
  }
};
struct ConcurrentGenerator : ITerrainGenerator
{
  std::shared_future<void> release;
  mutable std::atomic<int> entered = 0, active = 0, peak = 0;
  mutable std::atomic<bool> duplicate = false;
  mutable std::mutex mutex;
  mutable std::unordered_set<RegionKey, RegionKeyHash> activeKeys;
  mutable std::set<std::string> threadIds;
  explicit ConcurrentGenerator(std::shared_future<void> gate)
      : release(std::move(gate))
  {
  }
  double Density(WorldPosition p) const override
  {
    return 16 - p.y;
  }
  std::unique_ptr<TerrainRegion::Tree>
  Generate(RegionKey key, uint32_t lod,
           std::shared_ptr<std::pmr::memory_resource> memory = {},
           std::stop_token stop = {}) const override
  {
    {
      std::lock_guard lock(mutex);
      if (!activeKeys.insert(key).second)
        duplicate = true;
      threadIds.insert(TerrainThreadId());
    }
    struct Guard
    {
      const ConcurrentGenerator& owner;
      RegionKey key;
      ~Guard()
      {
        std::lock_guard lock(owner.mutex);
        owner.activeKeys.erase(key);
        --owner.active;
      }
    } guard{*this, key};
    const auto current = ++active;
    auto previous = peak.load();
    while (previous < current && !peak.compare_exchange_weak(previous, current))
    {
    }
    ++entered;
    release.wait();
    return ITerrainGenerator::Generate(key, lod, std::move(memory), stop);
  }
};
struct SerializedStorage : ITerrainStorage
{
  mutable std::atomic<int> active = 0, peak = 0, queries = 0, saves = 0;
  struct Call
  {
    const SerializedStorage& owner;
    explicit Call(const SerializedStorage& storage) : owner(storage)
    {
      const auto count = ++owner.active;
      auto peak = owner.peak.load();
      while (peak < count && !owner.peak.compare_exchange_weak(peak, count))
      {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ~Call()
    {
      --owner.active;
    }
  };
  std::optional<uint32_t> Query(RegionKey) const override
  {
    Call call(*this);
    ++queries;
    return {};
  }
  std::unique_ptr<TerrainRegion::Tree>
  Load(RegionKey, uint32_t, std::shared_ptr<std::pmr::memory_resource>) override
  {
    Call call(*this);
    return {};
  }
  void Save(RegionKey, uint32_t, const TerrainRegion::Tree&) override
  {
    Call call(*this);
    ++saves;
  }
};
static void TestGenerationThreadControl()
{
  std::promise<void> release;
  ConcurrentGenerator generator(release.get_future().share());
  SerializedStorage storage;
  WorldTerrain world;
  TerrainChunkManager manager(world, generator, storage);
  assert(manager.GenerationThreadCount() == 3);
  TerrainRequests requested;
  for (int i = 0; i < 12; ++i)
    requested.emplace(RegionKey{i, 0, 0, 0}, TerrainRequest{7, i < 6 ? 10 : 0});
  manager.Update(requested);
  Eventually([&] { manager.Update(requested);return generator.entered==3; });
  assert(generator.active==3 && manager.GenerationStats().activeWorkers==3);
  manager.SetGenerationThreadCount(6);
  Eventually(
      [&]
      {
        manager.Update(requested);
        return generator.entered == 6;
      });
  assert(generator.active == 6 && generator.peak == 6 &&
         manager.GenerationStats().activeWorkers == 6);
  {
    std::lock_guard lock(generator.mutex);
    assert(generator.activeKeys.size() == 6 && generator.threadIds.size() == 6);
    for (const auto& key : generator.activeKeys)
      assert(key.x < 6); // priority order
  }
  for (int i = 0; i < 10; ++i)
    manager.Update(requested);
  assert(generator.entered == 6 && !generator.duplicate);
  // The worker gate is still closed: resizing must return without joining.
  manager.SetGenerationThreadCount(2);
  manager.Update(requested);
  assert(manager.GenerationThreadCount() == 2 &&
         manager.GenerationStats().retiringWorkers == 4);
  assert(generator.entered == 6);
  manager.SetGenerationThreadCount(8);
  manager.Update(requested);
  Eventually(
      [&]
      {
        manager.Update(requested);
        return generator.entered == 8;
      });
  assert(generator.active == 8 && generator.peak == 8 && !generator.duplicate);
  manager.SetGenerationThreadCount(3);
  assert(manager.GenerationStats().threadCount == 3 &&
         manager.GenerationStats().retiringWorkers == 5);
  release.set_value();
  Eventually(
      [&]
      {
        manager.Update(requested);
        return manager.Resident().size() == requested.size() && !manager.Busy();
      });
  assert(manager.GenerationStats().workers.size() == 3 &&
         manager.GenerationStats().pending == 0);
  assert(!generator.duplicate && storage.peak == 1 && storage.queries >= 12 &&
         storage.saves >= 12);
  assert(manager.MemoryPeak() <= manager.MemoryLimit());
  for (auto count :
       {size_t(0), TerrainChunkManager::maximum_generation_threads + 1})
  {
    bool rejected = false;
    try
    {
      manager.SetGenerationThreadCount(count);
    }
    catch (const std::invalid_argument&)
    {
      rejected = true;
    }
    assert(rejected && manager.GenerationThreadCount() == 3);
  }
  requested.clear();
  Eventually(
      [&]
      {
        manager.Update(requested);
        return manager.Resident().empty() && !manager.Busy();
      });
}
static void TestConcurrentBudget()
{
  auto budget = std::make_shared<TerrainMemoryBudget>(1024);
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic<int> entered = 0, admitted = 0;
  std::vector<std::future<void>> workers;
  for (int i = 0; i < 6; ++i)
    workers.push_back(std::async(
        std::launch::async,
        [&]
        {
          std::pmr::polymorphic_allocator<std::byte> allocator(budget.get());
          std::byte* memory = nullptr;
          try
          {
            memory = allocator.allocate(256);
            ++admitted;
          }
          catch (const std::bad_alloc&)
          {
          }
          ++entered;
          gate.wait();
          if (memory)
            allocator.deallocate(memory, 256);
        }));
  Eventually([&] { return entered == 6; });
  assert(admitted == 4 && budget->Used() == 1024 && budget->Peak() <= 1024);
  release.set_value();
  for (auto& worker : workers)
    worker.get();
  assert(budget->Used() == 0);
}

int main()
{
  TestGenerationThreadControl();
  TestConcurrentBudget();
  PerlinTerrainGenerator a(42), b(42), c(43);
  const WorldPosition position{-17.3, 2.7, 9.4};
  assert(a.Density(position) == b.Density(position) &&
         a.Density(position) != c.Density(position));
  assert(a.Sample({0, -1000, 0}).density == 1 &&
         a.Sample({0, 1000, 0}).density == 0);
  NullTerrainStorage null;
  assert(!null.Query({}) && !null.Load({}, 8, {}));
  WorldTerrain terrain;
  CountingGenerator generator;
  TestStorage storage;
  TerrainChunkManager chunks(terrain, generator, storage);
  auto request = [&](RegionKey key, uint32_t lod)
  { Eventually([&] { return chunks.Request(key, lod) && !chunks.Busy(); }); };
  auto evict = [&]
  {
    Eventually(
        [&]
        {
          chunks.Update({});
          return chunks.Resident().empty() && !chunks.Busy();
        });
  };
  request({}, 7);
  assert(generator.calls == 1 && storage.saves == 1);
  assert(chunks.GenerationStats().threadId != TerrainThreadId());
  request({}, 7);
  assert(generator.calls == 1);
  request({}, 6);
  assert(generator.calls == 2);
  terrain.set_voxel({0, 0, 0}, {});
  terrain.set_voxel({1, 1, 1}, {9, 1});
  request({}, 5);
  assert(terrain.sample_voxel({0, 0, 0}).density == 0 &&
         terrain.sample_voxel({1, 1, 1}).material == 9);
  evict();
  assert(terrain.sample_voxel({1, 1, 1}).material == 9);
  storage.stored = 5;
  request({1, 0, 0, 0}, 6);
  assert(storage.loads == 1 && terrain.sample({33, 1, 1}).material == 7);
  storage.stored = 8;
  request({2, 0, 0, 0}, 7);
  assert(storage.loads == 1);
  storage.stored = 0;
  storage.miss = true;
  request({3, 0, 0, 0}, 7);
  assert(storage.loads == 2);
  evict();

  // A blocked generator cannot block Update, Request, cancellation, or a
  // memory-budget change on the rendering thread. No sleep-based assumption.
  {
    std::promise<void> release;
    GatedGenerator slow(release.get_future().share());
    auto entered = slow.entered.get_future();
    WorldTerrain live;
    TerrainChunkManager manager(live, slow, null);
    const TerrainRequests desired{{{0, 0, 0, 0}, {7, 0}}};
    manager.Update(desired);
    Eventually(
        [&]
        {
          return entered.wait_for(std::chrono::seconds(0)) ==
                 std::future_status::ready;
        });
    assert(entered.get() != TerrainThreadId());
    for (int i = 0; i < 20; ++i)
    {
      manager.Update(desired);
      assert(manager.Resident().empty());
    }
    manager.SetMemoryLimit(4096);
    manager.Update({});
    release.set_value();
    Eventually(
        [&]
        {
          manager.Update({});
          return !manager.Busy();
        });
    assert(manager.Resident().empty() && !live.source({}));
  }
  // An edit arriving while generation is in progress invalidates the prepared
  // result; replay occurs on the worker before the next pointer installation.
  {
    std::promise<void> release;
    GatedGenerator slow(release.get_future().share());
    auto entered = slow.entered.get_future();
    WorldTerrain live;
    TerrainChunkManager manager(live, slow, null);
    TerrainRequests desired{{{0, 0, 0, 0}, {7, 0}}};
    manager.Update(desired);
    Eventually(
        [&]
        {
          return entered.wait_for(std::chrono::seconds(0)) ==
                 std::future_status::ready;
        });
    live.set_voxel({1, 1, 1}, {12, 1});
    release.set_value();
    Eventually(
        [&]
        {
          manager.Update(desired);
          return manager.Resident().contains({}) && !manager.Busy();
        });
    assert(live.sample_voxel({1, 1, 1}).material == 12);
  }
  TerrainObserverManager observers;
  assert(TerrainObserver{}.lodLevels.front().lod == 0);
  auto first = observers.Register(
      {.position = {-0.1, 1, 1}, .lodLevels = {{0, 7}}, .priority = 2});
  auto second = observers.Register(
      {.position = {-0.1, 1, 1}, .lodLevels = {{0, 5}}, .priority = 8});
  const RegionKey negative{-1, 0, 0, 0};
  auto wanted = observers.Requests();
  assert(wanted.size() == 1 && wanted.at(negative).lod == 5 &&
         wanted.at(negative).priority == 8);
  auto settings = *second;
  settings.priority = 23;
  settings.lodLevels = {{0, 0}};
  observers.Configure(second, settings);
  assert(second->priority == 23 && second->lodLevels.front().lod == 0);
  settings.lodLevels = {{-1, 0}};
  bool rejected = false;
  try
  {
    observers.Configure(second, settings);
  }
  catch (const std::invalid_argument&)
  {
    rejected = true;
  }
  assert(rejected && second->lodLevels.front().distance == 0);
  settings.lodLevels = {{1e9, 0}};
  rejected = false;
  try
  {
    observers.Configure(second, settings);
  }
  catch (const std::length_error&)
  {
    rejected = true;
  }
  assert(rejected);
  first.reset();
  second.reset();
  assert(observers.Requests().empty());
  CameraView camera;
  camera.terrainPriority = 11;
  camera.UpdateTerrainObserver(observers, {3, 4, 5});
  assert(camera.GetTerrainObserver()->position == WorldPosition(3, 4, 5) &&
         camera.GetTerrainObserver()->priority == 11);
  CameraView moved(std::move(camera));
  moved.UpdateTerrainObserver(observers, {-33, 4, 5});
  assert(observers.Observers().size() == 1);
  moved.ReleaseTerrainObserver();
  assert(observers.Requests().empty());
  WorldTerrain limitedTerrain;
  TerrainChunkManager limited(limitedTerrain, generator, null);
  limited.SetMemoryLimit(1024);
  TerrainRequests desired{{{0, 0, 0, 0}, {4, 10}}, {{1, 0, 0, 0}, {4, 0}}};
  auto settle = [&]
  {
    Eventually(
        [&]
        {
          limited.Update(desired);
          return !limited.Busy() && limited.Resident().size() == desired.size();
        });
  };
  settle();
  assert(limited.MemoryUsed() <= 1024 && limited.MemoryPeak() <= 1024);
  assert(limited.Resident().at({0, 0, 0, 0}) <=
         limited.Resident().at({1, 0, 0, 0}));
  std::swap(desired.at({0, 0, 0, 0}).priority,
            desired.at({1, 0, 0, 0}).priority);
  settle();
  assert(limited.Resident().at({1, 0, 0, 0}) <=
         limited.Resident().at({0, 0, 0, 0}));
  limited.SetMemoryLimit(1024 * 1024);
  settle();
  assert(limited.Resident().at({0, 0, 0, 0}) == 4 &&
         limited.Resident().at({1, 0, 0, 0}) == 4);
  limited.SetMemoryLimit(0);
  Eventually(
      [&]
      {
        limited.Update(desired);
        return !limited.Busy() && limited.Resident().empty() &&
               limited.MemoryUsed() == 0;
      });
  assert(!limited.GenerationStats().budgetPending);
  World world("Streaming test");
  auto player = world.CreateEntity("Player");
  player.Add<Transform>().SetPos({1, 1, 1});
  auto& view = player.Add<Player>().cameraView;
  view.UpdateTerrainObserver(world.GetTerrainObservers(), {1, 1, 1});
  view.GetTerrainObserver()->lodLevels = {{0, 8}};
  Eventually(
      [&]
      {
        world.UpdateTerrain();
        return world.GetTerrainChunks().Resident().contains({0, 0, 0, 0});
      });
  player.Get<Transform>().SetPos({-33, 1, 1});
  Eventually(
      [&]
      {
        world.UpdateTerrain();
        return world.GetTerrainChunks().Resident().contains({-2, 0, 0, 0}) &&
             !world.GetTerrainChunks().Resident().contains({0, 0, 0, 0});
      });
  assert(view.GetTerrainObserver()->position == WorldPosition(-33, 1, 1));
  assert(!world.GetTerrainChunks().Resident().contains({0, 0, 0, 0}));
  world.Registry().destroy(player.ID());
  Eventually(
      [&]
      {
        world.UpdateTerrain();
        return world.GetTerrainChunks().Resident().empty() &&
               !world.GetTerrainChunks().Busy();
      });
  auto security = world.CreateEntity("Security camera");
  security.Add<Transform>().SetPos({1, 1, 1});
  auto& securityView = security.Add<CameraView>();
  securityView.terrainPriority = 12;
  securityView.UpdateTerrainObserver(world.GetTerrainObservers(), {1, 1, 1});
  securityView.GetTerrainObserver()->lodLevels = {{0, 8}};
  Eventually(
      [&]
      {
        world.UpdateTerrain();
        return world.GetTerrainChunks().Resident().contains({0, 0, 0, 0});
      });
  assert(securityView.GetTerrainObserver()->priority == 12);
  std::cout << "Asynchronous terrain streaming tests passed\n";
}
