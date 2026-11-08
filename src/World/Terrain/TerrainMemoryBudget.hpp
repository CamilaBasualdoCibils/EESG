#pragma once
#include <atomic>
#include <memory>
#include <memory_resource>
#include <stdexcept>
namespace EESG
{
// Reservations use atomics: the render thread never waits for a worker's
// allocation/deallocation or allocator mutex. Reservations include realloc
// peaks.
class TerrainMemoryBudget final : public std::pmr::memory_resource
{
public:
  explicit TerrainMemoryBudget(size_t limit = 256u * 1024 * 1024)
      : limit_(limit)
  {
  }
  size_t Used() const noexcept
  {
    return used_.load();
  }
  size_t Peak() const noexcept
  {
    return peak_.load();
  }
  size_t Limit() const noexcept
  {
    return limit_.load();
  }
  // Lower only while allocation jobs are drained; the manager handles that
  // asynchronously and keeps the old hard cap until live allocations fit.
  void SetLimit(size_t limit)
  {
    if (limit < Used())
      throw std::invalid_argument("Terrain budget change pending retirement");
    limit_ = limit;
  }
private:
  std::atomic<size_t> used_{0}, peak_{0}, limit_;
  void* do_allocate(size_t bytes, size_t alignment) override
  {
    auto used = used_.load();
    for (;;)
    {
      const auto limit = limit_.load();
      if (used > limit || bytes > limit - used)
        throw std::bad_alloc();
      if (used_.compare_exchange_weak(used, used + bytes))
        break;
    }
    auto peak = peak_.load();
    while (peak < used + bytes &&
           !peak_.compare_exchange_weak(peak, used + bytes))
    {
    }
    try
    {
      return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    catch (...)
    {
      used_.fetch_sub(bytes);
      throw;
    }
  }
  void do_deallocate(void* p, size_t bytes, size_t alignment) override
  {
    std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    used_.fetch_sub(bytes);
  }
  bool
  do_is_equal(const std::pmr::memory_resource& other) const noexcept override
  {
    return this == &other;
  }
};
} // namespace EESG
