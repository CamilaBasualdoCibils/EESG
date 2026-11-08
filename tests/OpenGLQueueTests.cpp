#include "../extern/Lighthouse-Engine/src/Backends/Render/OpenGL/OpenGLContextExecutor.hpp"
#include <Lighthouse/Backends/Render/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <Lighthouse/Engine/Window/WindowManager.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>
using namespace Lighthouse;
namespace
{
class OpenGLQueueTests : public testing::Test
{
protected:
  OpenGLBackend backend;
  std::unique_ptr<IRenderDevice> device;
  void SetUp() override
  {
    std::string diagnostic;
    ASSERT_TRUE(backend.Initialize(diagnostic)) << diagnostic;
    ASSERT_NO_THROW(
        device = backend.CreateDevice(
            {.queues = {{.capabilities = RenderQueueCapability::Graphics,
                         .minCount = 2,
                         .preferredCount = 2}}}));
    ASSERT_NE(device->GetQueue(0), nullptr);
    ASSERT_NE(device->GetQueue(1), nullptr);
    EXPECT_EQ(device->GetQueue(2), nullptr);
    EXPECT_TRUE(device->GetQueue(0)->GetCapabilities().Contains(
        RenderQueueCapability::Compute));
  }
  void TearDown() override
  {
    device.reset();
    backend.Shutdown();
  }
  BufferHandle Upload(std::span<const std::byte> bytes)
  {
    return device->CreateBuffer(
        {.size = bytes.size(),
         .usage = BufferUsage::TransferSrc,
         .initialData = bytes,
         .memory = {.requiredAccess =
                        MemoryAccess::CPUWrite | MemoryAccess::GPURead}});
  }
  BufferHandle Readback(std::uint64_t bytes)
  {
    return device->CreateBuffer(
        {.size = bytes,
         .usage = BufferUsage::TransferDst,
         .memory = {.requiredAccess =
                        MemoryAccess::CPURead | MemoryAccess::GPUWrite}});
  }
  void ExpectBytes(BufferHandle handle, std::span<const std::byte> expected)
  {
    device->InvalidateMappedMemoryRange(handle, 0, expected.size());
    const auto bytes = device->MapMemory(handle);
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
    device->UnmapMemory(handle);
  }
  ShaderModuleHandle Shader(std::string_view text, ShaderStage stage)
  {
    return device->CreateShaderModule(
        ShaderCompiler::Compile(text, {.stage = stage}));
  }
};

TEST(OpenGLContextWorkers, OwnContextsAndDoNotSerializeCpuExecution)
{
  auto group = std::make_shared<Detail::OpenGLDeviceGroup>();
  ASSERT_NO_THROW(group->Initialize());
  std::promise<std::pair<std::thread::id, EGLContext>> firstIdentity,
      secondIdentity;
  auto firstId = firstIdentity.get_future();
  auto secondId = secondIdentity.get_future();
  Detail::OpenGLContextExecutor first(
      group,
      [&]
      {
        firstIdentity.set_value(
            {std::this_thread::get_id(), eglGetCurrentContext()});
      },
      [] {});
  Detail::OpenGLContextExecutor second(
      group,
      [&]
      {
        secondIdentity.set_value(
            {std::this_thread::get_id(), eglGetCurrentContext()});
      },
      [] {});
  const auto a = firstId.get(), b = secondId.get();
  const auto extensions = first.Invoke(
      []
      {
        std::array<bool, 3> available{};
        const std::array<std::string_view, 3> names{"GL_EXT_semaphore",
                                                    "GL_EXT_semaphore_fd",
                                                    "GL_NV_timeline_semaphore"};
        GLint count{};
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i)
        {
          const auto extension = std::string_view(
              reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)));
          for (size_t name = 0; name < names.size(); ++name)
            available[name] = available[name] || extension == names[name];
        }
        return available;
      });
  std::cout << "OpenGL external semaphore extensions: EXT=" << extensions[0]
            << " fd=" << extensions[1] << " NV_timeline=" << extensions[2]
            << "; external backing provider: unavailable\n";

  EXPECT_NE(a.first, b.first);
  EXPECT_NE(a.first, std::this_thread::get_id());
  EXPECT_NE(a.second, EGL_NO_CONTEXT);
  EXPECT_NE(a.second, b.second);
  std::promise<void> entered, release, independent;
  auto started = entered.get_future();
  auto unblock = release.get_future().share();
  auto done = independent.get_future();
  first.Post(
      [&]
      {
        entered.set_value();
        unblock.wait();
      });
  started.wait();
  second.Post([&] { independent.set_value(); });
  const auto status = done.wait_for(std::chrono::seconds(3));
  // Always release the first owner before reporting a failure or destroying it.
  release.set_value();
  EXPECT_EQ(status, std::future_status::ready);
  first.Invoke([] {});
  second.Invoke([] {});
}

TEST_F(OpenGLQueueTests, TwoCpuSubmittersAndRecordedListLifetime)
{
  const std::array<std::uint32_t, 4> values{3, 5, 7, 11};
  const auto bytes = std::as_bytes(std::span(values));
  const auto source = Upload(bytes);
  const std::array outputs{Readback(bytes.size()), Readback(bytes.size())};
  const std::array fences{device->CreateFence(), device->CreateFence()};
  std::array<std::exception_ptr, 2> failures;
  std::barrier ready(3);
  std::array<std::thread, 2> submitters;
  for (unsigned index = 0; index < 2; ++index)
    submitters[index] = std::thread(
        [&, index]
        {
          ready.arrive_and_wait();
          try
          {
            auto commands = device->CreateCommandList(index);
            commands->Begin();
            commands->CopyBuffer(source, outputs[index], 0, 0, bytes.size());
            commands->End();
            device->GetQueue(index)->Submit(*commands, fences[index]);
            commands->Clear(); // Submission owns a copy; recording storage is
                               // reusable.
          }
          catch (...)
          {
            failures[index] = std::current_exception();
          }
        });
  ready.arrive_and_wait();
  for (auto& thread : submitters)
    thread.join();
  for (unsigned index = 0; index < 2; ++index)
  {
    if (failures[index])
      std::rethrow_exception(failures[index]);
    ASSERT_TRUE(device->WaitForFence(fences[index], 5'000'000'000));
    ExpectBytes(outputs[index], bytes);
    device->DestroyFence(fences[index]);
    device->DestroyBuffer(outputs[index]);
  }
  device->DestroyBuffer(source);
}

TEST_F(OpenGLQueueTests, BinaryDependencyPinsSharedAllocations)
{
  const std::array<std::uint32_t, 4> values{13, 17, 19, 23};
  const auto bytes = std::as_bytes(std::span(values));
  const auto source = Upload(bytes);
  const auto intermediate = device->CreateBuffer(
      {.size = bytes.size(),
       .usage = {BufferUsage::TransferSrc, BufferUsage::TransferDst}});
  const auto output = Readback(bytes.size());
  auto producer = device->CreateCommandList(0),
       consumer = device->CreateCommandList(1);
  producer->Begin();
  producer->CopyBuffer(source, intermediate, 0, 0, bytes.size());
  producer->End();
  consumer->Begin();
  consumer->CopyBuffer(intermediate, output, 0, 0, bytes.size());
  consumer->End();
  const auto semaphore = device->CreateSemaphore();
  const auto fence = device->CreateFence();
  const std::array signals{SemaphoreSubmitInfo{semaphore}};
  const IRenderCommandList* first = producer.get();
  const IRenderCommandList* second = consumer.get();
  device->GetQueue(0)->Submit(
      QueueSubmitInfo{.commandLists = {&first, 1}, .signals = signals});
  device->GetQueue(1)->Submit(
      QueueSubmitInfo{.commandLists = {&second, 1}, .waits = signals}, fence);
  producer.reset();
  consumer.reset();
  device->DestroyBuffer(source);
  device->DestroyBuffer(intermediate);
  EXPECT_FALSE(device->IsValid(source));
  ASSERT_TRUE(device->WaitForFence(fence, 5'000'000'000));
  ExpectBytes(output, bytes);
  device->DestroySemaphore(semaphore);
  device->DestroyFence(fence);
  device->DestroyBuffer(output);
}

TEST_F(OpenGLQueueTests, TimelineDependenciesAndExplicitFutureWaitRejection)
{
  const auto semaphore = device->CreateSemaphore(SemaphoreType::Timeline, 0);
  const auto firstFence = device->CreateFence(),
             secondFence = device->CreateFence();
  const std::array firstSignal{SemaphoreSubmitInfo{semaphore, 1}};
  const std::array secondSignal{SemaphoreSubmitInfo{semaphore, 2}};
  device->GetQueue(0)->Submit(QueueSubmitInfo{.signals = firstSignal},
                              firstFence);
  device->GetQueue(1)->Submit(
      QueueSubmitInfo{.waits = firstSignal, .signals = secondSignal},
      secondFence);
  ASSERT_TRUE(device->WaitForFence(secondFence, 5'000'000'000));
  EXPECT_TRUE(device->WaitSemaphore(semaphore, 2, 0));
  EXPECT_EQ(device->GetSemaphoreCounterValue(semaphore), 2u);
  const std::array future{SemaphoreSubmitInfo{semaphore, 3}};
  // Native external backing isn't configured; accepting this would require a
  // deferred scheduler or a CPU GPU-dependency wait, neither of which is valid.
  EXPECT_THROW(device->GetQueue(0)->Submit(QueueSubmitInfo{.waits = future}),
               std::logic_error);
  device->ResetFence(secondFence);
  device->GetQueue(1)->Submit(QueueSubmitInfo{.signals = future}, secondFence);
  ASSERT_TRUE(device->WaitForFence(secondFence, 5'000'000'000));
  EXPECT_TRUE(device->WaitSemaphore(semaphore, 3, 0));
  EXPECT_THROW(device->SignalSemaphore(semaphore, 3), std::logic_error);
  device->DestroyFence(firstFence);
  device->DestroyFence(secondFence);
  device->DestroySemaphore(semaphore);
}

TEST_F(OpenGLQueueTests, ComputeWorksOnGraphicsQueues)
{
  const auto shader = Shader(R"(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) buffer Result { uint value[]; } result;
void main(){result.value[gl_GlobalInvocationID.x]=gl_GlobalInvocationID.x*7u+3u;}
)",
                             ShaderStage::Compute);
  const auto setLayout = device->CreateDescriptorSetLayout(
      {.bindings = {{.binding = 0,
                     .type = DescriptorType::StorageBuffer,
                     .stages = ShaderStageFlags::Compute}}});
  const auto layout = device->CreatePipelineLayout({.sets = {setLayout}});
  const auto pipeline =
      device->CreatePipeline({.layout = layout, .computeShader = shader});
  const auto buffer = device->CreateBuffer(
      {.size = 16, .usage = {BufferUsage::Storage, BufferUsage::TransferSrc}});
  const auto output = Readback(16);
  const auto set = device->CreateDescriptorSet({setLayout});
  const std::array writes{
      DescriptorWrite{.resource = DescriptorBufferInfo{buffer}}};
  device->UpdateDescriptorSet(set, writes);
  auto compute = device->CreateCommandList(0),
       copy = device->CreateCommandList(1);
  compute->Begin();
  compute->BindPipeline(pipeline);
  compute->BindDescriptorSet(0, set);
  compute->Dispatch(4, 1, 1);
  compute->End();
  copy->Begin();
  copy->CopyBuffer(buffer, output, 0, 0, 16);
  copy->End();
  const auto semaphore = device->CreateSemaphore();
  const auto fence = device->CreateFence();
  const std::array signal{SemaphoreSubmitInfo{semaphore}};
  const IRenderCommandList* first = compute.get();
  const IRenderCommandList* second = copy.get();
  device->GetQueue(0)->Submit(
      QueueSubmitInfo{.commandLists = {&first, 1}, .signals = signal});
  device->GetQueue(1)->Submit(
      QueueSubmitInfo{.commandLists = {&second, 1}, .waits = signal}, fence);
  ASSERT_TRUE(device->WaitForFence(fence, 5'000'000'000));
  const std::array<std::uint32_t, 4> expected{3, 10, 17, 24};
  ExpectBytes(output, std::as_bytes(std::span(expected)));
  device->DestroyFence(fence);
  device->DestroySemaphore(semaphore);
  device->DestroyDescriptorSet(set);
  device->DestroyPipeline(pipeline);
  device->DestroyPipelineLayout(layout);
  device->DestroyDescriptorSetLayout(setLayout);
  device->DestroyModule(shader);
  device->DestroyBuffer(buffer);
  device->DestroyBuffer(output);
}

TEST_F(OpenGLQueueTests, QueueLocalVaosFbosAndDescriptorSnapshots)
{
  const auto vertex = Shader(R"(#version 450
layout(location=0) in vec2 position;
void main(){gl_Position=vec4(position,0,1);}
)",
                             ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
layout(set=0,binding=0) uniform Tint {vec4 tint;};
layout(location=0) out vec4 color;
void main(){color=tint;}
)",
                               ShaderStage::Fragment);
  const auto setLayout = device->CreateDescriptorSetLayout(
      {.bindings = {{.binding = 0,
                     .type = DescriptorType::UniformBuffer,
                     .stages = ShaderStageFlags::Fragment}}});
  const auto layout = device->CreatePipelineLayout({.sets = {setLayout}});
  const auto pipeline = device->CreatePipeline(
      {.vertexShader = vertex,
       .fragmentShader = fragment,
       .vertexLayout = {.bindings = {{.binding = 0, .stride = 8}},
                        .attributes = {{.location = 0,
                                        .binding = 0,
                                        .format = VertexFormat::Float2}}},
       .colorFormats = {ImageFormat::RGBA8},
       .layout = layout});
  const std::array positions{-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
  const std::array<std::uint32_t, 3> indices{0, 1, 2};
  const auto vb = device->CreateBuffer(
      {.size = sizeof(positions),
       .usage = BufferUsage::Vertex,
       .initialData = std::as_bytes(std::span(positions))});
  const auto ib =
      device->CreateBuffer({.size = sizeof(indices),
                            .usage = BufferUsage::Index,
                            .initialData = std::as_bytes(std::span(indices))});
  const std::array red{1.f, 0.f, 0.f, 1.f}, blue{0.f, 0.f, 1.f, 1.f};
  const auto redBuffer =
      device->CreateBuffer({.size = 16,
                            .usage = BufferUsage::Uniform,
                            .initialData = std::as_bytes(std::span(red))});
  const auto blueBuffer =
      device->CreateBuffer({.size = 16,
                            .usage = BufferUsage::Uniform,
                            .initialData = std::as_bytes(std::span(blue))});
  std::array<ImageHandle, 2> images;
  std::array<ImageViewHandle, 2> views;
  const std::array sets{device->CreateDescriptorSet({setLayout}),
                        device->CreateDescriptorSet({setLayout})};
  const std::array fences{device->CreateFence(), device->CreateFence()};
  for (unsigned i = 0; i < 2; ++i)
  {
    images[i] = device->CreateImage(
        {.width = 16,
         .height = 16,
         .format = ImageFormat::RGBA8,
         .usage = {ImageUsage::ColorAttachment, ImageUsage::TransferSource}});
    views[i] = device->CreateImageView(
        {.image = images[i], .format = ImageFormat::RGBA8});
  }
  const std::array writeRed{
      DescriptorWrite{.resource = DescriptorBufferInfo{redBuffer}}};
  const std::array writeBlue{
      DescriptorWrite{.resource = DescriptorBufferInfo{blueBuffer}}};
  for (unsigned frame = 0; frame < 3; ++frame)
  {
    device->UpdateDescriptorSet(sets[0], writeRed);
    device->UpdateDescriptorSet(sets[1], writeBlue);
    for (unsigned i = 0; i < 2; ++i)
    {
      if (frame)
        device->ResetFence(fences[i]);
      auto commands = device->CreateCommandList(i);
      commands->Begin();
      commands->PushDebugGroup("Queue-local graphics state");
      commands->BeginRendering({.colorAttachment = views[i],
                                .clearColor = true,
                                .width = 16,
                                .height = 16});
      commands->BindPipeline(pipeline);
      commands->BindDescriptorSet(0, sets[i]);
      commands->BindVertexBuffer(vb, 8);
      commands->BindIndexBuffer(ib);
      commands->DrawIndexed(3);
      commands->EndRendering();
      commands->PopDebugGroup();
      commands->End();
      device->GetQueue(i)->Submit(*commands, fences[i]);
    }
    device->UpdateDescriptorSet(
        sets[0], writeBlue); // Already submitted work must retain red.
    for (unsigned i = 0; i < 2; ++i)
      ASSERT_TRUE(device->WaitForFence(fences[i], 5'000'000'000));
    const auto first = device->ReadImage(images[0]);
    const auto second = device->ReadImage(images[1]);
    EXPECT_GT(std::to_integer<unsigned>(first[(8 * 16 + 8) * 4]), 250u);
    EXPECT_LT(std::to_integer<unsigned>(first[(8 * 16 + 8) * 4 + 2]), 5u);
    EXPECT_GT(std::to_integer<unsigned>(second[(8 * 16 + 8) * 4 + 2]), 250u);
    EXPECT_LT(std::to_integer<unsigned>(second[(8 * 16 + 8) * 4]), 5u);
  }
  device->DestroyPipeline(pipeline);
  device->DestroyModule(vertex);
  device->DestroyModule(fragment);
  for (unsigned i = 0; i < 2; ++i)
  {
    device->DestroyFence(fences[i]);
    device->DestroyDescriptorSet(sets[i]);
    device->DestroyImageView(views[i]);
    device->DestroyImage(images[i]);
  }
  device->DestroyPipelineLayout(layout);
  device->DestroyDescriptorSetLayout(setLayout);
  for (auto handle : {vb, ib, redBuffer, blueBuffer})
    device->DestroyBuffer(handle);
}

TEST_F(OpenGLQueueTests, CompletionFencesAndShutdownWithQueuedDependencies)
{
  const std::array<std::uint32_t, 4> values{2, 4, 6, 8};
  auto first = Upload(std::as_bytes(std::span(values)));
  const auto second = device->CreateBuffer(
      {.size = 16,
       .usage = {BufferUsage::TransferSrc, BufferUsage::TransferDst}});
  const auto third = device->CreateBuffer(
      {.size = 16,
       .usage = {BufferUsage::TransferSrc, BufferUsage::TransferDst}});
  const std::array semaphores{device->CreateSemaphore(),
                              device->CreateSemaphore()};
  const auto fence = device->CreateFence();
  EXPECT_FALSE(device->GetFenceStatus(fence));
  for (unsigned i = 0; i < 24; ++i)
  {
    auto commands = device->CreateCommandList(i % 2);
    commands->Begin();
    commands->CopyBuffer(first, i % 2 ? third : second, 0, 0, 16);
    commands->End();
    const IRenderCommandList* list = commands.get();
    const std::array signal{SemaphoreSubmitInfo{semaphores[i % 2]}};
    const std::array wait{SemaphoreSubmitInfo{semaphores[(i + 1) % 2]}};
    device->GetQueue(i % 2)->Submit(
        QueueSubmitInfo{.commandLists = {&list, 1},
                        .waits = i ? std::span<const SemaphoreSubmitInfo>(wait)
                                   : std::span<const SemaphoreSubmitInfo>{},
                        .signals = signal},
        i == 23 ? fence : FenceHandle{});
    first = i % 2 ? third : second;
  }
  // No caller WaitIdle: destruction must finish queued work, release snapshots,
  // and join both context owners before deleting the shared allocations.
  auto closing =
      std::async(std::launch::async,
                 [owned = std::move(device)]() mutable { owned.reset(); });
  EXPECT_EQ(closing.wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  closing.get();
}
} // namespace

namespace
{
TEST(OpenGLSurfaceQueues, SurfaceAffinityAndResizeKeepOtherQueueAlive)
{
  if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
    GTEST_SKIP() << "Window display required";
  OpenGLBackend backend;
  std::string diagnostic;
  ASSERT_TRUE(backend.Initialize(diagnostic)) << diagnostic;
  WindowManager windows;
  const auto a =
      windows.CreateWindow({.title = "Queue surface A",
                            .width = 64,
                            .height = 64,
                            .eglSurface = backend.RequiresEGLWindow(),
                            .visible = false});
  const auto b =
      windows.CreateWindow({.title = "Queue surface B",
                            .width = 64,
                            .height = 64,
                            .eglSurface = backend.RequiresEGLWindow(),
                            .visible = false});
  const auto leftSurface = backend.CreateSurface(*windows.GetWindow(a));
  const auto rightSurface = backend.CreateSurface(*windows.GetWindow(b));
  auto device = backend.CreateDevice(
      {.queues = {{.capabilities = RenderQueueCapability::Graphics |
                                   RenderQueueCapability::Present,
                   .minCount = 1,
                   .presentSurface = leftSurface},
                  {.capabilities = RenderQueueCapability::Graphics |
                                   RenderQueueCapability::Present,
                   .minCount = 1,
                   .presentSurface = rightSurface}}});
  auto* leftQueue = device->GetQueue(0);
  auto* rightQueue = device->GetQueue(1);
  ASSERT_NE(leftQueue, nullptr);
  ASSERT_NE(rightQueue, nullptr);
  const auto left = device->CreateSwapchain(leftSurface, {.vsync = false});
  const auto right = device->CreateSwapchain(rightSurface, {.vsync = false});
  const std::array semaphores{device->CreateSemaphore(),
                              device->CreateSemaphore()};
  const std::array fences{device->CreateFence(), device->CreateFence()};
  for (unsigned frame = 0; frame < 3; ++frame)
  {
    if (frame == 1)
    {
      glfwSetWindowSize(
          static_cast<GLFWwindow*>(windows.GetWindow(a)->NativeHandle()), 96,
          80);
      windows.PollEvents();
    }
    const std::array images{device->AcquireSwapchainImage(left),
                            device->AcquireSwapchainImage(right)};
    for (unsigned index = 0; index < 2; ++index)
    {
      if (frame)
        device->ResetFence(fences[index]);
      const auto extent =
          windows.GetWindow(index ? b : a)->GetFramebufferSize();
      auto commands = device->CreateCommandList(index);
      commands->Begin();
      commands->BeginRendering(
          {.colorAttachment = images[index].view,
           .clearColor = true,
           .clearColorValue = index ? Vec3{0, 0, 1} : Vec3{1, 0, 0},
           .width = static_cast<std::uint32_t>(extent.width),
           .height = static_cast<std::uint32_t>(extent.height)});
      commands->EndRendering();
      commands->End();
      const IRenderCommandList* list = commands.get();
      const std::array signals{SemaphoreSubmitInfo{semaphores[index]}};
      device->GetQueue(index)->Submit(
          QueueSubmitInfo{.commandLists = {&list, 1}, .signals = signals},
          fences[index]);
    }
    EXPECT_THROW(leftQueue->Present(right), std::invalid_argument);
    const std::array leftWait{semaphores[0]}, rightWait{semaphores[1]};
    leftQueue->Present(left, leftWait);
    rightQueue->Present(right, rightWait);
    for (auto fence : fences)
      ASSERT_TRUE(device->WaitForFence(fence, 5'000'000'000));
    EXPECT_GT(std::to_integer<unsigned>(device->ReadImage(images[0].image)[0]),
              250u);
    EXPECT_GT(std::to_integer<unsigned>(device->ReadImage(images[1].image)[2]),
              250u);
  }
  device->DestroySwapchain(left);
  device->DestroySwapchain(right);
  device->ResetFence(fences[1]);
  rightQueue->Submit(QueueSubmitInfo{}, fences[1]);
  EXPECT_TRUE(device->WaitForFence(fences[1], 5'000'000'000));
  device.reset();
  backend.DestroySurface(leftSurface);
  backend.DestroySurface(rightSurface);
  windows.DestroyWindow(a);
  windows.DestroyWindow(b);
  backend.Shutdown();
}
} // namespace

namespace
{
TEST_F(OpenGLQueueTests,
       ResourceApisCanMoveBetweenCpuThreadsWithoutBorrowingQueueContexts)
{
  std::array<std::exception_ptr, 2> failures;
  std::barrier start(3);
  std::array<std::thread, 2> threads;
  for (unsigned index = 0; index < 2; ++index)
    threads[index] = std::thread(
        [&, index]
        {
          start.arrive_and_wait();
          try
          {
            const auto savedApi = eglQueryAPI();
            const auto savedContext = eglGetCurrentContext();
            for (unsigned iteration = 0; iteration < 4; ++iteration)
            {
              const std::array<std::uint32_t, 4> values{
                  index, iteration, index + 10, iteration + 100};
              const auto bytes = std::as_bytes(std::span(values));
              const auto input = Upload(bytes);
              const auto output = Readback(bytes.size());
              const auto image =
                  device->CreateImage({.width = 8,
                                       .height = 8,
                                       .format = ImageFormat::RGBA8,
                                       .usage = ImageUsage::Sampled});
              const auto fence = device->CreateFence();
              auto commands = device->CreateCommandList(index);
              commands->Begin();
              commands->CopyBuffer(input, output, 0, 0, bytes.size());
              commands->End();
              device->GetQueue(index)->Submit(*commands, fence);
              device->DestroyBuffer(input);
              device->DestroyImage(image);
              EXPECT_TRUE(device->WaitForFence(fence, 5'000'000'000));
              ExpectBytes(output, bytes);
              device->DestroyFence(fence);
              device->DestroyBuffer(output);
              EXPECT_EQ(eglQueryAPI(), savedApi);
              EXPECT_EQ(eglGetCurrentContext(), savedContext);
            }
          }
          catch (...)
          {
            failures[index] = std::current_exception();
          }
        });
  start.arrive_and_wait();
  for (auto& thread : threads)
    thread.join();
  for (const auto& error : failures)
    if (error)
      std::rethrow_exception(error);
}
} // namespace
