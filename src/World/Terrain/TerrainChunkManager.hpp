#pragma once
#include "World/Terrain/ITerrainGenerator.hpp"
#include "World/Terrain/ITerrainStorage.hpp"
#include "World/Terrain/TerrainMemoryBudget.hpp"
#include "World/Terrain/TerrainThreadInfo.hpp"
#include <algorithm>
#include <climits>
#include <future>
#include <mutex>
#include <tuple>

namespace EESG
{
struct TerrainRequest
{
  uint32_t lod = 8;
  int priority = 0;
  bool operator==(const TerrainRequest&) const = default;
};
using TerrainRequests =
    std::unordered_map<RegionKey, TerrainRequest, RegionKeyHash>;
using TerrainResidency = std::unordered_map<RegionKey, uint32_t, RegionKeyHash>;
struct TerrainGenerationStats
{
  std::string threadId, lastAction = "No terrain work yet", error;
  size_t generated = 0, loaded = 0, budgetBlocked = 0, pending = 0;
  double updateMilliseconds = 0, lastMilliseconds = 0;
  std::optional<RegionKey> lastChunk;
  uint32_t lastLOD = 0;
  bool budgetPending = false;
  TerrainBackgroundStatus worker;
  std::vector<TerrainBackgroundStatus> workers;
  size_t threadCount = 3, activeWorkers = 0, retiringWorkers = 0;
  bool memoryPressure = false;
};
// Bounded generation workers share a CPU budget. Storage access stays serial.
// Update only polls, plans work, and installs already-prepared pointers. Old
// trees remain visible until replacement; reclamation also runs off-thread.
class TerrainChunkManager
{
public:
  static constexpr size_t default_generation_threads = 3;
  static constexpr size_t maximum_generation_threads = 64;
  TerrainChunkManager(WorldTerrain& terrain, ITerrainGenerator& generator,
                      ITerrainStorage& storage)
      : terrain_(terrain), generator_(generator), storage_(storage),
        slots_(default_generation_threads)
  {
  }
  ~TerrainChunkManager()
  {
    for (auto& slot : slots_)
      if (slot.job)
        slot.job->stop.request_stop();
    // Only shutdown joins; runtime control changes and updates never wait.
    for (auto& slot : slots_)
      if (slot.future.valid())
        slot.future.wait();
  }
  size_t GenerationThreadCount() const noexcept
  {
    return threadCount_;
  }
  void SetGenerationThreadCount(size_t count)
  {
    if (!count || count > maximum_generation_threads)
      throw std::invalid_argument(
          "Generation thread count must be between 1 and 64");
    if (count == threadCount_)
      return;
    threadCount_ = count;
    if (slots_.size() < count)
      slots_.resize(count);
    for (size_t i = count; i < slots_.size(); ++i)
      if (slots_[i].job)
        slots_[i].job->stop.request_stop();
    TrimIdleSlots();
  }
  const TerrainResidency& Resident() const noexcept
  {
    return resident_;
  }
  TerrainGenerationStats GenerationStats() const
  {
    auto stats = stats_;
    stats.threadCount = threadCount_;
    stats.pending = 0;
    stats.activeWorkers = 0;
    stats.retiringWorkers = 0;
    stats.memoryPressure = memoryPressure_;
    for (size_t i = 0; i < slots_.size(); ++i)
    {
      stats.workers.push_back(slots_[i].activity->Snapshot());
      if (stats.workers.back().active)
        ++stats.activeWorkers;
      if (slots_[i].job)
      {
        ++stats.pending;
        if (i >= threadCount_)
          ++stats.retiringWorkers;
      }
    }
    if (!stats.workers.empty())
      stats.worker = stats.workers.front();
    if (stats.threadId.empty())
      stats.threadId = stats.worker.threadId;
    stats.budgetPending = memory_->Limit() != requestedLimit_;
    return stats;
  }
  size_t MemoryUsed() const noexcept
  {
    return memory_->Used();
  }
  size_t MemoryPeak() const noexcept
  {
    return memory_->Peak();
  }
  size_t MemoryLimit() const noexcept
  {
    return requestedLimit_;
  }
  bool Busy() const noexcept
  {
    return ActiveJobs() != 0 || !retired_.empty();
  }
  void SetMemoryLimit(size_t bytes)
  {
    if (bytes == requestedLimit_)
      return;
    memoryPressure_ = bytes < requestedLimit_;
    requestedLimit_ = bytes;
    fallback_.clear();
    ++epoch_;
    for (auto& slot : slots_)
      if (slot.job)
        slot.job->stop.request_stop();
  }
  // Returns readiness, not completion of synchronous work. Call Update each
  // frame; clients may also repeatedly Request until the chunk is resident.
  bool Request(RegionKey key, uint32_t lod)
  {
    auto requested = desired_;
    requested.insert_or_assign(key, TerrainRequest{lod, 0});
    Update(requested);
    const auto it = resident_.find(key);
    return it != resident_.end() && it->second <= lod && terrain_.source(key);
  }
  void Update(const TerrainRequests& requests)
  {
    stats_.generated = stats_.loaded = stats_.budgetBlocked = 0;
    stats_.updateMilliseconds = 0;
    for (const auto& [key, request] : requests)
    {
      WorldTerrain::validate(key);
      if (key.scale || request.lod > WorldTerrain::chunk_depth)
        throw std::out_of_range("Invalid terrain request");
    }
    if (requests != desired_ || terrain_.clear_revision() != clearRevision_)
    {
      desired_ = requests;
      clearRevision_ = terrain_.clear_revision();
      ++epoch_;
      fallback_.clear();
      ordered_.assign(desired_.begin(), desired_.end());
      std::sort(ordered_.begin(), ordered_.end(),
                [](const auto& a, const auto& b)
                {
                  if (a.second.priority != b.second.priority)
                    return a.second.priority > b.second.priority;
                  if (a.second.lod != b.second.lod)
                    return a.second.lod < b.second.lod;
                  return std::tie(a.first.x, a.first.y, a.first.z) <
                         std::tie(b.first.x, b.first.y, b.first.z);
                });
      for (auto& slot : slots_)
        if (slot.job && slot.job->kind != Kind::Cleanup &&
            (!Relevant(*slot.job) ||
             slot.job->clearRevision != terrain_.clear_revision()))
          slot.job->stop.request_stop();
    }
    std::erase_if(resident_, [&](const auto& entry)
                  { return !terrain_.source(entry.first); });
    PollCompleted();
    TrimIdleSlots();
    // Retry quality only after a stable drop in live memory, not when another
    // worker's temporary allocation grows/shrinks while a batch is running.
    if (!ActiveJobs())
    {
      if (memory_->Used() < stableUsed_)
        fallback_.clear();
      stableUsed_ = memory_->Used();
      if (retired_.empty() && !reclaimFor_ &&
          std::all_of(desired_.begin(), desired_.end(),
                      [&](const auto& entry)
                      {
                        const auto it = resident_.find(entry.first);
                        return it != resident_.end() &&
                               it->second <= entry.second.lod;
                      }))
        memoryPressure_ = false;
    }
    while (FreeSlot())
      if (!PlanOne())
        break;
  }

private:
  bool PlanOne()
  {
    // Lowering a live budget is asynchronous: stop allocation work, retire
    // old terrain, and apply the new hard limit once existing buffers fit.
    if (memory_->Limit() != requestedLimit_)
    {
      // Lowering the hard cap must not race active allocations. Stop sources
      // were signaled by SetMemoryLimit; drain them through zero-timeout polls.
      if (requestedLimit_ < memory_->Limit() && ActiveJobs())
        return false;
      if (memory_->Used() <= requestedLimit_)
      {
        memory_->SetLimit(requestedLimit_);
        fallback_.clear();
      }
      else
      {
        if (!retired_.empty())
        {
          Launch(Kind::Cleanup, {}, {});
          return true;
        }
        if (auto victim = Victim({}))
        {
          Launch(Kind::Evict, *victim, {});
          return true;
        }
        return false; // snapshots may still be releasing on meshing workers
      }
    }
    // Evictions precede new generation; edited terrain is reconstructed on
    // the worker, so streaming never replays an edit log on the render thread.
    for (const auto& [key, lod] : resident_)
      if (!desired_.contains(key) && !InFlight(key))
      {
        Launch(Kind::Evict, key, {});
        return true;
      }
    if (reclaimFor_)
    {
      const auto requester = desired_.find(*reclaimFor_);
      if (requester != desired_.end())
      {
        bool drainingLower = false;
        for (auto& slot : slots_)
          if (slot.job && slot.job->kind == Kind::Generate &&
              slot.job->priority < requester->second.priority)
          {
            slot.job->stop.request_stop();
            drainingLower = true;
          }
        if (auto victim = Victim(requester->second.priority))
        {
          Launch(Kind::Evict, *victim, {});
          reclaimFor_.reset();
          return true;
        }
        if (drainingLower)
          return false;
      }
      reclaimFor_.reset();
    }
    if (!requestedLimit_)
    {
      if (!retired_.empty())
      {
        Launch(Kind::Cleanup, {}, {});
        return true;
      }
      return false;
    }
    // Under budget pressure, drain the batch and allocate one generation at
    // a time in priority order. Lower-priority workers must not repeatedly win
    // allocation races against an important observer.
    if (memoryPressure_ && ActiveJobs())
    {
      if (!retired_.empty())
      {
        Launch(Kind::Cleanup, {}, {});
        return true;
      }
      return false;
    }
    for (const auto& [key, request] : ordered_)
    {
      if (InFlight(key))
        continue;
      const auto target = fallback_.contains(key)
                              ? std::max(request.lod, fallback_.at(key))
                              : request.lod;
      if (target > WorldTerrain::chunk_depth)
        continue;
      const auto resident = resident_.find(key);
      if (resident != resident_.end() && resident->second == target)
        continue;
      // A failed upgrade must not replace a finer resident chunk by a worse
      // fallback. Explicit observer downgrades still replace it normally.
      if (resident != resident_.end() && request.lod < resident->second &&
          target >= resident->second)
        continue;
      auto actual = request;
      actual.lod = target;
      Launch(Kind::Generate, key, actual);
      return true;
    }
    if (!retired_.empty())
    {
      Launch(Kind::Cleanup, {}, {});
      return true;
    }
    return false;
  }

private:
  enum class Kind
  {
    Generate,
    Evict,
    Cleanup
  };
  struct Job
  {
    Kind kind;
    RegionKey key;
    uint32_t lod = 8, targetLOD = 8;
    int priority = 0;
    size_t editRevision = 0;
    uint64_t clearRevision = 0, epoch = 0;
    std::vector<TerrainEdit> edits;
    std::stop_source stop;
  };
  struct Result
  {
    std::shared_ptr<TerrainRegion::Tree> data;
    bool generated = false, budgetBlocked = false, released = false;
    double milliseconds = 0;
    std::string message, error;
  };
  struct WorkerSlot
  {
    std::shared_ptr<Job> job;
    std::future<Result> future;
    std::shared_ptr<TerrainBackgroundActivity> activity =
        std::make_shared<TerrainBackgroundActivity>();
  };
  size_t ActiveJobs() const
  {
    return std::count_if(slots_.begin(), slots_.end(),
                         [](const auto& slot) { return bool(slot.job); });
  }
  std::optional<size_t> FreeSlot() const
  {
    for (size_t i = 0; i < threadCount_; ++i)
      if (!slots_[i].job)
        return i;
    return {};
  }
  bool InFlight(RegionKey key) const
  {
    return std::any_of(slots_.begin(), slots_.end(),
                       [&](const auto& slot)
                       {
                         return slot.job && slot.job->kind != Kind::Cleanup &&
                                slot.job->key == key;
                       });
  }
  void TrimIdleSlots()
  {
    while (slots_.size() > threadCount_ && !slots_.back().job)
      slots_.pop_back();
    pollCursor_ %= slots_.size();
  }
  void PollCompleted()
  {
    // Round-robin collection prevents fast low-numbered slots from starving
    // other completed or retiring workers. Limit main-thread publications.
    const size_t count = slots_.size(), start = pollCursor_;
    size_t completedCount = 0;
    for (size_t step = 0; step < count && completedCount < 2; ++step)
    {
      const auto index = (start + step) % count;
      auto& slot = slots_[index];
      if (!slot.job || slot.future.wait_for(std::chrono::seconds(0)) !=
                           std::future_status::ready)
        continue;
      auto result = slot.future.get();
      auto completed = std::move(slot.job);
      stats_.threadId = slot.activity->Snapshot().threadId;
      HandleResult(std::move(completed), std::move(result));
      pollCursor_ = (index + 1) % count;
      ++completedCount;
    }
  }
  void HandleResult(std::shared_ptr<Job> completed, Result result)
  {
    stats_.lastChunk = completed->kind == Kind::Cleanup
                           ? std::nullopt
                           : std::optional(completed->key);
    stats_.lastLOD = completed->lod;
    stats_.lastMilliseconds = result.milliseconds;
    stats_.updateMilliseconds += result.milliseconds;
    stats_.lastAction = result.message;
    if (result.released)
      fallback_.clear();
    if (!result.error.empty())
      stats_.error = result.error;
    const bool current =
        !completed->stop.stop_requested() && Relevant(*completed) &&
        completed->clearRevision == terrain_.clear_revision() &&
        completed->editRevision == terrain_.edit_revision(completed->key);
    if (result.data)
    {
      if (current)
      {
        terrain_.CommitPreparedSource(
            completed->key, completed->lod, std::move(result.data),
            completed->kind == Kind::Generate, retired_);
        if (completed->kind == Kind::Evict)
        {
          resident_.erase(completed->key);
          fallback_.clear();
        }
        else
        {
          resident_.insert_or_assign(completed->key, completed->lod);
          if (result.generated)
            ++stats_.generated;
          else
            ++stats_.loaded;
          fallback_[completed->key] = completed->lod;
        }
      }
      else
        retired_.trees.push_back(std::move(result.data));
    }
    if (result.budgetBlocked && current)
    {
      ++stats_.budgetBlocked;
      fallback_[completed->key] = completed->lod + 1;
      memoryPressure_ = true;
      const auto currentRequester =
          reclaimFor_ ? desired_.find(*reclaimFor_) : desired_.end();
      const auto candidate = desired_.find(completed->key);
      if (candidate != desired_.end() &&
          (currentRequester == desired_.end() ||
           candidate->second.priority > currentRequester->second.priority))
        reclaimFor_ = completed->key;
    }
    if (!result.error.empty() && current)
      fallback_[completed->key] = WorldTerrain::chunk_depth + 1;
  }
  bool Relevant(const Job& job) const
  {
    if (job.kind == Kind::Cleanup)
      return true;
    const auto it = desired_.find(job.key);
    if (job.kind == Kind::Evict)
      return it == desired_.end() || job.epoch == epoch_;
    return it != desired_.end() && it->second.lod == job.targetLOD;
  }
  std::optional<RegionKey> Victim(std::optional<int> requesterPriority) const
  {
    std::optional<RegionKey> result;
    int lowest = INT_MAX;
    for (const auto& [key, lod] : resident_)
    {
      if (InFlight(key))
        continue;
      const auto wanted = desired_.find(key);
      const int priority =
          wanted == desired_.end() ? INT_MIN : wanted->second.priority;
      if (requesterPriority && priority >= *requesterPriority)
        continue;
      if (!result || priority < lowest)
      {
        result = key;
        lowest = priority;
      }
    }
    return result;
  }
  void Launch(Kind kind, RegionKey key, TerrainRequest request)
  {
    auto job = std::make_shared<Job>();
    job->kind = kind;
    job->key = key;
    job->lod = request.lod;
    job->priority = request.priority;
    job->epoch = epoch_;
    job->clearRevision = terrain_.clear_revision();
    job->editRevision = terrain_.edit_revision(key);
    if (kind != Kind::Cleanup)
      job->edits = terrain_.copy_edits(key);
    if (auto desired = desired_.find(key); desired != desired_.end())
      job->targetLOD = desired->second.lod;
    auto retired = std::move(retired_);
    retired_ = {};
    const auto index = *FreeSlot();
    auto& slot = slots_[index];
    const auto activity = slot.activity;
    const auto storageMutex = storageMutex_;
    const auto workerName = "TerrainGen" + std::to_string(index + 1);
    const auto memory = memory_;
    auto* generator = &generator_;
    auto* storage = &storage_;
    activity->Begin(kind == Kind::Cleanup ? std::nullopt : std::optional(key),
                    request.lod);
    slot.future = std::async(
        std::launch::async,
        [job, retired = std::move(retired), activity, memory, generator,
         storage, storageMutex, workerName]() mutable
        {
          const auto start = std::chrono::steady_clock::now();
          Result result;
          result.released = !retired.empty();
          activity->Mark("Releasing old terrain", workerName.c_str());
          retired = {};
          const auto stop = job->stop.get_token();
          auto check = [&]
          {
            if (stop.stop_requested())
              throw TerrainGenerationCancelled{};
          };
          try
          {
            check();
            if (job->kind == Kind::Cleanup)
              result.message = "Released retired terrain";
            else
            {
              std::unique_ptr<TerrainRegion::Tree> data;
              if (job->kind == Kind::Generate)
              {
                activity->Mark("Waiting for storage", workerName.c_str());
                {
                  std::lock_guard lock(*storageMutex);
                  check();
                  activity->Mark("Loading chunk", workerName.c_str());
                  const auto stored = storage->Query(job->key);
                  check();
                  if (stored && *stored <= job->lod)
                    data = storage->Load(job->key, job->lod, memory);
                }
                check();
                if (!data)
                {
                  activity->Mark("Generating voxels", workerName.c_str());
                  result.generated = true;
                  data = generator->Generate(job->key, job->lod, memory, stop);
                  check();
                  if (!data)
                    throw std::runtime_error(
                        "Terrain generator returned no data");
                  activity->Mark("Waiting to save generated data",
                                 workerName.c_str());
                  {
                    std::lock_guard lock(*storageMutex);
                    check();
                    activity->Mark("Saving generated data", workerName.c_str());
                    storage->Save(job->key, job->lod, *data);
                  }
                  check();
                }
              }
              else
                data = std::make_unique<TerrainRegion::Tree>(
                    WorldTerrain::chunk_depth);
              if (data->depth() != WorldTerrain::chunk_depth)
                throw std::invalid_argument("Invalid stored chunk depth");
              activity->Mark("Preparing chunk and edits", workerName.c_str());
              data->visit(
                  [&](const TerrainRegion::Tree::Entry& entry)
                  {
                    check();
                    if (!entry.terminal)
                      return;
                    const auto density = data->cell(entry.handle).density;
                    if (!std::isfinite(density) || density < 0 || density > 1)
                      throw std::invalid_argument("Invalid terrain occupancy");
                  });
              for (const auto& edit : job->edits)
              {
                check();
                if (edit.value.density == 0)
                  data->erase({edit.origin, edit.depth});
                else
                  data->paint({edit.origin, edit.depth}, edit.value);
              }
              check();
              result.data = std::move(data);
              result.message = job->kind == Kind::Evict
                                   ? "Evicted procedural chunk; retained edits"
                               : result.generated ? "Generated chunk"
                                                  : "Loaded chunk";
            }
          }
          catch (const TerrainGenerationCancelled&)
          {
            result.message = "Cancelled stale terrain request";
          }
          catch (const std::bad_alloc&)
          {
            result.budgetBlocked = true;
            result.message = "CPU budget reached; trying coarser voxels";
          }
          catch (const std::exception& error)
          {
            result.error = error.what();
            result.message = "Terrain request failed";
          }
          job->edits.clear();
          job->edits.shrink_to_fit();
          result.milliseconds = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
          activity->Finish(result.message);
          return result;
        });
    slot.job = std::move(job);
  }
  WorldTerrain& terrain_;
  ITerrainGenerator& generator_;
  ITerrainStorage& storage_;
  TerrainResidency resident_;
  TerrainRequests desired_;
  std::vector<std::pair<RegionKey, TerrainRequest>> ordered_;
  std::unordered_map<RegionKey, uint32_t, RegionKeyHash> fallback_;
  std::optional<RegionKey> reclaimFor_;
  uint64_t epoch_ = 0, clearRevision_ = 0;
  size_t requestedLimit_ = 256u * 1024 * 1024, stableUsed_ = 0;
  std::shared_ptr<TerrainMemoryBudget> memory_ =
      std::make_shared<TerrainMemoryBudget>();
  std::shared_ptr<std::mutex> storageMutex_ = std::make_shared<std::mutex>();
  size_t threadCount_ = default_generation_threads, pollCursor_ = 0;
  bool memoryPressure_ = false;
  TerrainGenerationStats stats_;
  WorldTerrain::RetiredSources retired_;
  std::vector<WorkerSlot> slots_;
};
} // namespace EESG
