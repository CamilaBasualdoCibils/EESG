#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <Lighthouse/Engine/Assets/ShaderCompiler.hpp>
#include <Lighthouse/Engine/Core/EngineRuntime.hpp>
#include <Lighthouse/Backends/Render/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Backends/Render/Vulkan/VulkanBackend.hpp>
#include <Lighthouse/Backends/Render/Vulkan/VulkanCommandList.hpp>
#include <array>
#include <gtest/gtest.h>

using namespace Lighthouse;
namespace
{
class HeadlessDeviceTests : public testing::TestWithParam<bool>
{
protected:
  std::unique_ptr<IGraphicsBackend> backend;
  std::unique_ptr<IRenderDevice> device;
  void SetUp() override
  {
    if (GetParam())
      backend = std::make_unique<VulkanBackend>();
    else
      backend = std::make_unique<OpenGLBackend>();
    std::string diagnostic;
    ASSERT_TRUE(backend->Initialize(diagnostic)) << diagnostic;
    ASSERT_NO_THROW(device = backend->CreateDevice(
                        {.applicationName = "Headless backend regression"}));
    ASSERT_NE(device, nullptr);
    const auto caps = device->GetCapabilities();
    EXPECT_TRUE(caps.headless);
    EXPECT_TRUE(caps.computeShaders);
    EXPECT_FALSE(caps.asyncCompute);
    if (GetParam())
    {
      EXPECT_GE(caps.apiVersion, vk::ApiVersion14);
      EXPECT_TRUE(caps.shaderObjects);
      EXPECT_TRUE(caps.dynamicRendering);
    }
    std::cout << "Device: " << caps.deviceName << "\n";
  }
  void TearDown() override
  {
    device.reset();
    if (backend)
      backend->Shutdown();
  }
  ShaderModuleHandle Shader(std::string_view source, ShaderStage stage)
  {
    return device->CreateShaderModule(
        ShaderCompiler::Compile(source, {.stage = stage}));
  }
  static constexpr std::string_view Fullscreen = R"(#version 450
void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexIndex],0.5,1);}
)";
  ImageHandle Target(unsigned size = 64)
  {
    return device->CreateImage(
        {.width = size,
         .height = size,
         .format = ImageFormat::RGBA8,
         .usage = {ImageUsage::ColorAttachment, ImageUsage::TransferSource,
                   ImageUsage::TransferDestination}});
  }
  ImageViewHandle View(ImageHandle image)
  {
    return device->CreateImageView(
        {.image = image, .format = ImageFormat::RGBA8});
  }
};
TEST_P(HeadlessDeviceTests, MultipleColorAttachmentsClearAndDraw)
{
  const auto a = Target(), b = Target();
  const auto av = View(a), bv = View(b);
  const auto depth =
      device->CreateImage({.width = 64,
                           .height = 64,
                           .format = ImageFormat::Depth24Stencil8,
                           .usage = {ImageUsage::DepthStencilAttachment}});
  const auto depthView = device->CreateImageView(
      {.image = depth, .format = ImageFormat::Depth24Stencil8});
  const auto vertex = Shader(Fullscreen, ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
layout(location=0) out vec4 first;
layout(location=1) out vec4 second;
void main(){first=vec4(1,0,0,1);second=vec4(0,1,0,1);}
)",
                               ShaderStage::Fragment);
  const auto layout = device->CreatePipelineLayout({});
  const auto pipeline = device->CreatePipeline(
      {.vertexShader = vertex,
       .fragmentShader = fragment,
       .colorFormats = {ImageFormat::RGBA8, ImageFormat::RGBA8},
       .depthFormat = ImageFormat::Depth24Stencil8,
       .layout = layout});
  const auto commands = device->CreateCommandList(QueueType::Graphics);
  commands->Begin();
  commands->BeginRendering({.clearColor = true,
                            .clearColorValue = {0, 0, 1},
                            .width = 64,
                            .height = 64,
                            .colorAttachments = {av, bv}});
  commands->EndRendering();
  commands->End();
  device->Submit(*commands);
  for (auto image : {a, b})
  {
    const auto pixels = device->ReadImage(image);
    EXPECT_GT(std::to_integer<unsigned>(pixels[2]), 250u);
    EXPECT_LT(std::to_integer<unsigned>(pixels[0]), 5u);
  }
  commands->Clear();
  commands->Begin();
  commands->BeginRendering({.depthAttachment = depthView,
                            .clearDepth = true,
                            .width = 64,
                            .height = 64,
                            .colorAttachments = {av, bv}});
  commands->SetStencil(StencilMode::Write, 3);
  commands->BindPipeline(pipeline);
  commands->Draw(PrimitiveTopology::TriangleList, 3, 0);
  commands->EndRendering();
  commands->End();
  device->Submit(*commands);
  const auto first = device->ReadImage(a), second = device->ReadImage(b);
  const auto center = (32 * 64 + 32) * 4;
  EXPECT_GT(std::to_integer<unsigned>(first[center]), 250u);
  EXPECT_LT(std::to_integer<unsigned>(first[center + 1]), 5u);
  EXPECT_LT(std::to_integer<unsigned>(second[center]), 5u);
  EXPECT_GT(std::to_integer<unsigned>(second[center + 1]), 250u);
  // A mismatched ID must leave cleared pixels untouched, then the matching
  // ID must draw even though its fullscreen depth equals the stored depth.
  for (uint32_t reference : {2u, 3u})
  {
    commands->Clear();
    commands->Begin();
    commands->BeginRendering({.depthAttachment = depthView,
                              .clearColor = true,
                              .width = 64,
                              .height = 64,
                              .colorAttachments = {av, bv},
                              .depthReadOnly = true});
    commands->SetStencil(StencilMode::Equal, reference);
    commands->BindPipeline(pipeline);
    commands->Draw(PrimitiveTopology::TriangleList, 3, 0);
    commands->EndRendering();
    commands->End();
    device->Submit(*commands);
    const auto red = device->ReadImage(a), green = device->ReadImage(b);
    EXPECT_EQ(std::to_integer<unsigned>(red[center]),
              reference == 3 ? 255u : 0u);
    EXPECT_EQ(std::to_integer<unsigned>(green[center + 1]),
              reference == 3 ? 255u : 0u);
  }
  device->WaitIdle();
  device->DestroyPipeline(pipeline);
  device->DestroyModule(fragment);
  device->DestroyModule(vertex);
  device->DestroyImageView(bv);
  device->DestroyImageView(av);
  device->DestroyImageView(depthView);
  device->DestroyImage(depth);
  device->DestroyImage(b);
  device->DestroyImage(a);
  device->DestroyPipelineLayout(layout);
}
TEST_P(HeadlessDeviceTests, SharedIndexedMeshesDepthAndLateDescriptorUpdates)
{
  auto* gpu = device.get();
  Renderer renderer(std::move(device));
  Mesh3D left, right;
  left.vertices = {
      {{-.9f, -.8f, .5f}}, {{-.1f, -.8f, .5f}}, {{-.5f, .8f, .5f}}};
  right.vertices = {{{.1f, -.8f, .5f}}, {{.9f, -.8f, .5f}}, {{.5f, .8f, .5f}}};
  left.indices = right.indices = {0, 1, 2};
  const auto a = renderer.UploadMesh(left), b = renderer.UploadMesh(right);
  const auto* first = renderer.GetRenderMesh(a);
  const auto* second = renderer.GetRenderMesh(b);
  ASSERT_EQ(first->vertexBuffers[0].buffer, second->vertexBuffers[0].buffer);
  ASSERT_EQ(first->indexBuffer->buffer, second->indexBuffer->buffer);
  const auto vertex = gpu->CreateShaderModule(
      ShaderCompiler::Compile(R"(#version 450
layout(location=0) in vec3 position;void main(){gl_Position=vec4(position,1);}
)",
                              {.stage = ShaderStage::Vertex}));
  const auto fragment = gpu->CreateShaderModule(
      ShaderCompiler::Compile(R"(#version 450
layout(set=0,binding=0) uniform Color {vec4 value;} tint;
layout(location=0) out vec4 color;void main(){color=tint.value;}
)",
                              {.stage = ShaderStage::Fragment}));
  const auto setLayout = gpu->CreateDescriptorSetLayout(
      {.bindings = {{.binding = 0,
                     .type = DescriptorType::UniformBuffer,
                     .stages = ShaderStageFlags::Fragment}}});
  const auto layout = gpu->CreatePipelineLayout({.sets = {setLayout}});
  const auto pipeline =
      gpu->CreatePipeline({.vertexShader = vertex,
                           .fragmentShader = fragment,
                           .vertexLayout = first->layout,
                           .colorFormats = {ImageFormat::RGBA8},
                           .depthFormat = ImageFormat::Depth32,
                           .layout = layout});
  const std::array red{1.f, 0.f, 0.f, 1.f}, green{0.f, 1.f, 0.f, 1.f};
  auto redBuffer =
      gpu->CreateBuffer({.size = 16,
                         .usage = BufferUsage::Uniform,
                         .initialData = std::as_bytes(std::span(red))});
  auto greenBuffer =
      gpu->CreateBuffer({.size = 16,
                         .usage = BufferUsage::Uniform,
                         .initialData = std::as_bytes(std::span(red))});
  const auto set = gpu->CreateDescriptorSet({setLayout});
  const std::array initial{
      DescriptorWrite{.resource = DescriptorBufferInfo{redBuffer}}};
  gpu->UpdateDescriptorSet(set, initial);
  auto image = gpu->CreateImage(
      {.width = 64,
       .height = 64,
       .format = ImageFormat::RGBA8,
       .usage = {ImageUsage::ColorAttachment, ImageUsage::TransferSource}});
  auto view =
      gpu->CreateImageView({.image = image, .format = ImageFormat::RGBA8});
  auto depth = gpu->CreateImage({.width = 64,
                                 .height = 64,
                                 .format = ImageFormat::Depth32,
                                 .usage = {ImageUsage::DepthAttachment}});
  auto depthView =
      gpu->CreateImageView({.image = depth, .format = ImageFormat::Depth32});
  auto commands = gpu->CreateCommandList(QueueType::Graphics);
  if (GetParam())
    ASSERT_TRUE(static_cast<bool>(
        dynamic_cast<VulkanCommandList*>(commands.get())->Native()));
  commands->Begin();
  commands->BeginRendering({.colorAttachment = view,
                            .depthAttachment = depthView,
                            .clearColor = true,
                            .clearDepth = true,
                            .width = 64,
                            .height = 64});
  commands->BindPipeline(pipeline);
  commands->BindDescriptorSet(0, set);
  commands->BindVertexBuffer(first->vertexBuffers[0].buffer,
                             sizeof(Mesh3DVertex));
  commands->BindIndexBuffer(first->indexBuffer->buffer);
  std::vector<DrawIndexedIndirectCommand> draws;
  for (auto* mesh : {first, second})
    draws.push_back({mesh->indexCount, 1,
        static_cast<unsigned>(mesh->indexBuffer->offset / 4),
        static_cast<int>(mesh->vertexBuffers[0].offset / sizeof(Mesh3DVertex)), 0});
  const auto indirect = gpu->CreateBuffer({
      .size = draws.size() * sizeof(draws[0]), .usage = BufferUsage::Indirect,
      .initialData = std::as_bytes(std::span(draws))});
  commands->MultiDrawIndexedIndirect(indirect, static_cast<unsigned>(draws.size()));
  commands->EndRendering();
  commands->End();
  // The interface promises descriptor/host updates before Submit are visible,
  // even after a native command buffer was recorded.
  gpu->UpdateBuffer(greenBuffer, 0, std::as_bytes(std::span(green)));
  const std::array updated{
      DescriptorWrite{.resource = DescriptorBufferInfo{greenBuffer}}};
  gpu->UpdateDescriptorSet(set, updated);
  gpu->Submit(*commands);
  auto pixels = gpu->ReadImage(image);
  gpu->DestroyBuffer(indirect);
  for (auto x : {16u, 48u})
  {
    EXPECT_LT(std::to_integer<unsigned>(pixels[(24 * 64 + x) * 4]), 5u);
    EXPECT_GT(std::to_integer<unsigned>(pixels[(24 * 64 + x) * 4 + 1]), 250u);
  }
  EXPECT_LT(std::to_integer<unsigned>(pixels[(60 * 64 + 32) * 4 + 1]), 5u);
}
TEST_P(HeadlessDeviceTests, ComputeStorageWritesAreVisibleToGraphics)
{
  const auto compute = Shader(R"(#version 450
layout(local_size_x=1) in;layout(set=0,binding=0,std430) buffer Result {vec4 value;} data;
void main(){data.value=vec4(0.2,0.8,0.3,1);}
)",
                              ShaderStage::Compute);
  const auto vertex = Shader(Fullscreen, ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
layout(set=0,binding=0,std430) readonly buffer Result {vec4 value;} data;
layout(location=0) out vec4 color;void main(){color=data.value;}
)",
                               ShaderStage::Fragment);
  const auto setLayout = device->CreateDescriptorSetLayout(
      {.bindings = {{.binding = 0,
                     .type = DescriptorType::StorageBuffer,
                     .stages = ShaderStageFlags::Compute |
                               ShaderStageFlags::Fragment}}});
  const auto layout = device->CreatePipelineLayout({.sets = {setLayout}});
  const auto computePipeline =
      device->CreatePipeline({.layout = layout, .computeShader = compute});
  const auto graphics =
      device->CreatePipeline({.vertexShader = vertex,
                              .fragmentShader = fragment,
                              .colorFormats = {ImageFormat::RGBA8},
                              .layout = layout});
  const auto buffer =
      device->CreateBuffer({.size = 16, .usage = BufferUsage::Storage});
  const auto set = device->CreateDescriptorSet({setLayout});
  const std::array writes{
      DescriptorWrite{.resource = DescriptorBufferInfo{buffer}}};
  device->UpdateDescriptorSet(set, writes);
  auto commands = device->CreateCommandList(QueueType::Compute);
  commands->Begin();
  commands->BindPipeline(computePipeline);
  commands->BindDescriptorSet(0, set);
  commands->Dispatch(1, 1, 1);
  commands->End();
  device->Submit(*commands);
  auto image = Target();
  auto view = View(image);
  auto draw = device->CreateCommandList(QueueType::Graphics);
  draw->Begin();
  draw->BeginRendering(
      {.colorAttachment = view, .clearColor = true, .width = 64, .height = 64});
  draw->BindPipeline(graphics);
  draw->BindDescriptorSet(0, set);
  draw->Draw(PrimitiveTopology::TriangleList, 3, 0);
  draw->EndRendering();
  draw->End();
  device->Submit(*draw);
  const auto pixels = device->ReadImage(image);
  EXPECT_NEAR(std::to_integer<int>(pixels[(32 * 64 + 32) * 4]), 51, 2);
  EXPECT_NEAR(std::to_integer<int>(pixels[(32 * 64 + 32) * 4 + 1]), 204, 2);
}
TEST_P(HeadlessDeviceTests, SampledImageAndScaledBlit)
{
  const auto vertex = Shader(Fullscreen, ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
layout(set=0,binding=0) uniform sampler2D sourceImage;layout(location=0) out vec4 color;
void main(){color=texture(sourceImage,vec2(0.5));}
)",
                               ShaderStage::Fragment);
  const auto setLayout = device->CreateDescriptorSetLayout(
      {.bindings = {{.binding = 0,
                     .type = DescriptorType::CombinedImageSampler,
                     .stages = ShaderStageFlags::Fragment}}});
  const auto layout = device->CreatePipelineLayout({.sets = {setLayout}});
  const auto pipeline =
      device->CreatePipeline({.vertexShader = vertex,
                              .fragmentShader = fragment,
                              .colorFormats = {ImageFormat::RGBA8},
                              .layout = layout});
  const std::array<std::uint8_t, 16> data{255, 0, 255, 255, 255, 0, 255, 255,
                                          255, 0, 255, 255, 255, 0, 255, 255};
  const auto source =
      device->CreateImage({.width = 2,
                           .height = 2,
                           .format = ImageFormat::RGBA8,
                           .usage = {ImageUsage::Sampled},
                           .initialData = std::as_bytes(std::span(data))});
  auto sourceView = View(source);
  auto sampler = device->CreateSampler({});
  const auto set = device->CreateDescriptorSet({setLayout});
  const std::array writes{
      DescriptorWrite{.resource = DescriptorImageInfo{sourceView, sampler}}};
  device->UpdateDescriptorSet(set, writes);
  auto image = Target();
  auto view = View(image);
  auto output = Target(128);
  auto commands = device->CreateCommandList(QueueType::Graphics);
  commands->Begin();
  commands->BeginRendering(
      {.colorAttachment = view, .clearColor = true, .width = 64, .height = 64});
  commands->BindPipeline(pipeline);
  commands->BindDescriptorSet(0, set);
  commands->Draw(PrimitiveTopology::TriangleList, 3, 0);
  commands->EndRendering();
  const BlitRegion region{.srcSubresource = {ImageAspect::Color, 0, 0, 1},
                          .srcOffsets = {{{0, 0, 0}, {64, 64, 1}}},
                          .dstSubresource = {ImageAspect::Color, 0, 0, 1},
                          .dstOffsets = {{{0, 0, 0}, {128, 128, 1}}}};
  commands->BlitImage(image, output, region, ImageFilter::Linear);
  commands->End();
  device->Submit(*commands);
  const auto pixels = device->ReadImage(output);
  EXPECT_GT(std::to_integer<unsigned>(pixels[(64 * 128 + 64) * 4]), 250u);
  EXPECT_LT(std::to_integer<unsigned>(pixels[(64 * 128 + 64) * 4 + 1]), 5u);
  EXPECT_GT(std::to_integer<unsigned>(pixels[(64 * 128 + 64) * 4 + 2]), 250u);
}
INSTANTIATE_TEST_SUITE_P(Backends, HeadlessDeviceTests,
                         testing::Values(false, true), [](const auto& info)
                         { return info.param ? "Vulkan14" : "OpenGL"; });

class DeviceCreationFailure final : public IGraphicsBackend
{
public:
  std::string_view Name() const noexcept override
  {
    return "FailDevice";
  }
  bool Initialize(std::string&) override
  {
    return true;
  }
  GraphicsCapabilities Capabilities() const override
  {
    return {};
  }
  std::unique_ptr<IRenderDevice> CreateDevice(const DeviceConfig&) override
  {
    throw std::runtime_error("intentional device creation failure");
  }
  void Shutdown() noexcept override {}
};
TEST(BackendDeviceSelectionTests, FirstAvailableRetriesDeviceFailure)
{
  BackendRegistry registry;
  registry.Register("FailDevice",
                    [] { return std::make_unique<DeviceCreationFailure>(); });
  registry.Register("OpenGL", [] { return std::make_unique<OpenGLBackend>(); });
  Config config;
  config.name = "Device fallback";
  config.graphics.backends = {"FailDevice", "OpenGL"};
  EngineRuntime runtime;
  ASSERT_TRUE(runtime.Initialize(config, registry));
  ASSERT_TRUE(runtime.HasGraphicsBackend());
  EXPECT_EQ(runtime.GetGraphicsBackend()->Name(), "OpenGL");
  ASSERT_EQ(runtime.BackendDiagnostics().size(), 2u);
  EXPECT_NE(runtime.BackendDiagnostics()[0].diagnostic.find("intentional"),
            std::string::npos);
  EXPECT_TRUE(runtime.BackendDiagnostics()[1].selected);
  EXPECT_FALSE(runtime.GetWindowManager().IsAvailable());
}
TEST(BackendDeviceSelectionTests, RequireFirstDoesNotRetryDeviceFailure)
{
  BackendRegistry registry;
  registry.Register("FailDevice",
                    [] { return std::make_unique<DeviceCreationFailure>(); });
  registry.Register("OpenGL", [] { return std::make_unique<OpenGLBackend>(); });
  Config config;
  config.graphics.backends = {"FailDevice", "OpenGL"};
  config.graphics.selection = BackendSelection::RequireFirst;
  EngineRuntime runtime;
  EXPECT_FALSE(runtime.Initialize(config, registry));
  EXPECT_FALSE(runtime.HasGraphicsBackend());
  EXPECT_EQ(runtime.BackendDiagnostics().size(), 1u);
}
TEST(BackendDeviceSelectionTests, FactoryExceptionsAreDiagnosed)
{
  BackendRegistry registry;
  registry.Register("Throws", []() -> std::unique_ptr<IGraphicsBackend>
                    { throw std::runtime_error("factory failure"); });
  GraphicsConfig config;
  config.backends = {"Throws"};
  auto selected = registry.Select(config);
  EXPECT_FALSE(selected);
  ASSERT_EQ(selected.attempts.size(), 1u);
  EXPECT_EQ(selected.attempts[0].diagnostic, "factory failure");
}
} // namespace

// Presentation tests are intentionally separate from headless tests: run these
// with a native display, and run the others with both display variables
// removed.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
namespace
{
class WindowDeviceTests : public HeadlessDeviceTests
{
protected:
  void SetUp() override
  {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
      GTEST_SKIP() << "Native display required";
    HeadlessDeviceTests::SetUp();
  }
};
TEST_P(WindowDeviceTests, MultipleSwapchainsResizeAndDestroy)
{
  const auto survivingImage=Target();
  WindowManager windows;
  const auto a =
      windows.CreateWindow({.title = "Backend window test A",
                            .width = 64,
                            .height = 64,
                            .eglSurface = device->RequiresEGLWindow(),
                            .visible = false});
  const auto b =
      windows.CreateWindow({.title = "Backend window test B",
                            .width = 64,
                            .height = 64,
                            .eglSurface = device->RequiresEGLWindow(),
                            .visible = false});
  const auto first =
      device->CreateSwapchain(*windows.GetWindow(a), {.vsync = false});
  const auto second =
      device->CreateSwapchain(*windows.GetWindow(b), {.vsync = false});
  for (unsigned frame = 0; frame < 12; ++frame)
  {
    windows.PollEvents();
    auto left = device->AcquireSwapchainImage(first);
    auto right = device->AcquireSwapchainImage(second);
    auto commands = device->CreateCommandList(QueueType::Graphics);
    commands->Begin();
    const auto leftSize = windows.GetWindow(a)->GetFramebufferSize();
    const auto rightSize = windows.GetWindow(b)->GetFramebufferSize();
    commands->BeginRendering({.colorAttachment = left.view,
                              .clearColor = true,
                              .clearColorValue = {1, 0, 0},
                              .width = leftSize.width,
                              .height = leftSize.height});
    commands->EndRendering();
    commands->BeginRendering({.colorAttachment = right.view,
                              .clearColor = true,
                              .clearColorValue = {0, 1, 0},
                              .width = rightSize.width,
                              .height = rightSize.height});
    commands->EndRendering();
    commands->End();
    device->Submit(*commands);
    const auto leftPixels = device->ReadImage(left.image);
    const auto rightPixels = device->ReadImage(right.image);
    ASSERT_GE(leftPixels.size(), 4u);
    ASSERT_GE(rightPixels.size(), 4u);
    EXPECT_GT(std::to_integer<unsigned>(leftPixels[0]), 250u);
    EXPECT_LT(std::to_integer<unsigned>(leftPixels[1]), 5u);
    EXPECT_GT(std::to_integer<unsigned>(rightPixels[1]), 250u);
    EXPECT_LT(std::to_integer<unsigned>(rightPixels[0]), 5u);
    device->Present(first);
    device->Present(second);
    if (frame == 4)
    {
      glfwSetWindowSize(
          static_cast<GLFWwindow*>(windows.GetWindow(a)->NativeHandle()), 96,
          80);
      windows.PollEvents();
    }
  }
  EXPECT_TRUE(device->DestroySwapchain(first));
  EXPECT_FALSE(device->IsValid(first));
  EXPECT_TRUE(device->DestroySwapchain(second));
  windows.DestroyWindow(a);
  windows.DestroyWindow(b);
  // All presentation resources are gone; the device must still work.
  const auto image = survivingImage;
  const auto view = View(image);
  auto clear = device->CreateCommandList(QueueType::Graphics);
  clear->Begin();
  clear->BeginRendering({.colorAttachment = view,
                         .clearColor = true,
                         .clearColorValue = {0, 0, 1},
                         .width = 64,
                         .height = 64});
  clear->EndRendering();
  clear->End();
  device->Submit(*clear);
  EXPECT_GT(std::to_integer<unsigned>(device->ReadImage(image)[2]), 250u);
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowDeviceTests,
                         testing::Values(false, true), [](const auto& info)
                         { return info.param ? "Vulkan14" : "OpenGL"; });
TEST(BackendRuntimePriorityTests, VulkanFirstWithRealFallback)
{
  Config config;
  config.name = "Real backend fallback";
  config.AddRenderBackend<VulkanBackend>();
  config.AddRenderBackend<OpenGLBackend>();
  EngineRuntime runtime;
  ASSERT_TRUE(runtime.Initialize(config));
  ASSERT_FALSE(runtime.BackendDiagnostics().empty());
  EXPECT_EQ(runtime.BackendDiagnostics().front().backend, "Vulkan");
  if (const auto* expected = std::getenv("EESG_EXPECT_BACKEND"))
    EXPECT_EQ(runtime.GetGraphicsBackend()->Name(), expected);
  EXPECT_TRUE(
      runtime.GetRenderer().GetRenderDevice()->GetCapabilities().headless);
}
} // namespace
namespace
{
TEST_P(HeadlessDeviceTests, ShaderObjectMultiviewIsReportedUnavailable)
{
  const auto caps = device->GetCapabilities();
  EXPECT_FALSE(caps.multiView);
  if (!GetParam())
    return;
  EXPECT_TRUE(caps.shaderObjects);
  const auto image =
      device->CreateImage({.width = 32,
                           .height = 32,
                           .format = ImageFormat::RGBA8,
                           .arrayLayers = 2,
                           .usage = {ImageUsage::ColorAttachment},
                           .type = ImageType::Image2DArray});
  const auto view = device->CreateImageView({.image = image,
                                             .format = ImageFormat::RGBA8,
                                             .type = ImageType::Image2DArray,
                                             .arrayLayerCount = 2});
  auto command = device->CreateCommandList(QueueType::Graphics);
  command->Begin();
  EXPECT_THROW(
      command->BeginRendering(
          {.colorAttachment = view, .width = 32, .height = 32, .viewMask = 3}),
      std::runtime_error);
  command->End();
}
TEST_P(HeadlessDeviceTests, BindlessCapabilitySupportsNonuniformSampledArray)
{
  if (!GetParam())
  {
    EXPECT_FALSE(device->GetCapabilities().bindlessResources);
    return;
  }
  if (!device->GetCapabilities().bindlessResources)
    GTEST_SKIP() << "Device has no descriptor indexing";
  const auto vertex = Shader(Fullscreen, ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=0,binding=0) uniform sampler2D images[];layout(location=0) out vec4 color;
void main(){color=texture(images[nonuniformEXT(int(gl_FragCoord.x)&1)],vec2(0.5));}
)",
                               ShaderStage::Fragment);
  const auto setLayout = device->CreateDescriptorSetLayout(
      {.bindings = {{.binding = 0,
                     .type = DescriptorType::CombinedImageSampler,
                     .count = 2,
                     .stages = ShaderStageFlags::Fragment}}});
  const auto layout = device->CreatePipelineLayout({.sets = {setLayout}});
  const auto pipeline =
      device->CreatePipeline({.vertexShader = vertex,
                              .fragmentShader = fragment,
                              .colorFormats = {ImageFormat::RGBA8},
                              .layout = layout});
  const std::array<std::uint8_t, 4> red{255, 0, 0, 255}, green{0, 255, 0, 255};
  const auto a =
      device->CreateImage({.format = ImageFormat::RGBA8,
                           .usage = ImageUsage::Sampled,
                           .initialData = std::as_bytes(std::span(red))});
  const auto b =
      device->CreateImage({.format = ImageFormat::RGBA8,
                           .usage = ImageUsage::Sampled,
                           .initialData = std::as_bytes(std::span(green))});
  const auto sampler = device->CreateSampler({});
  const auto set = device->CreateDescriptorSet({setLayout});
  const std::array writes{
      DescriptorWrite{.binding = 0,
                      .arrayElement = 0,
                      .resource = DescriptorImageInfo{View(a), sampler}},
      DescriptorWrite{.binding = 0,
                      .arrayElement = 1,
                      .resource = DescriptorImageInfo{View(b), sampler}}};
  device->UpdateDescriptorSet(set, writes);
  const auto target = Target();
  const auto view = View(target);
  auto command = device->CreateCommandList(QueueType::Graphics);
  command->Begin();
  command->BeginRendering(
      {.colorAttachment = view, .clearColor = true, .width = 64, .height = 64});
  command->BindPipeline(pipeline);
  command->BindDescriptorSet(0, set);
  command->Draw(PrimitiveTopology::TriangleList, 3, 0);
  command->EndRendering();
  command->End();
  device->Submit(*command);
  const auto pixels = device->ReadImage(target);
  EXPECT_GT(std::to_integer<unsigned>(pixels[(32 * 64 + 32) * 4]), 250u);
  EXPECT_GT(std::to_integer<unsigned>(pixels[(32 * 64 + 33) * 4 + 1]), 250u);
}
} // namespace
namespace
{
TEST_P(HeadlessDeviceTests, LegacyTextureSlotsAndCommandListClear)
{
  const auto vertex = Shader(Fullscreen, ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
layout(set=0,binding=0) uniform sampler2D image;layout(location=0) out vec4 color;
void main(){color=texture(image,vec2(0.5));}
)",
                               ShaderStage::Fragment);
  const auto setLayout = device->CreateDescriptorSetLayout(
      {.bindings = {{.binding = 0,
                     .type = DescriptorType::CombinedImageSampler,
                     .stages = ShaderStageFlags::Fragment}}});
  const auto layout = device->CreatePipelineLayout({.sets = {setLayout}});
  const auto pipeline =
      device->CreatePipeline({.vertexShader = vertex,
                              .fragmentShader = fragment,
                              .colorFormats = {ImageFormat::RGBA8},
                              .layout = layout});
  const std::array<std::uint8_t, 4> red{255, 0, 0, 255}, green{0, 255, 0, 255};
  const auto a =
      device->CreateImage({.format = ImageFormat::RGBA8,
                           .usage = ImageUsage::Sampled,
                           .initialData = std::as_bytes(std::span(red))});
  const auto b =
      device->CreateImage({.format = ImageFormat::RGBA8,
                           .usage = ImageUsage::Sampled,
                           .initialData = std::as_bytes(std::span(green))});
  View(a);
  View(b);
  const auto target = Target();
  const auto view = View(target);
  auto command = device->CreateCommandList(QueueType::Graphics);
  command->Begin();
  command->BeginRendering(
      {.colorAttachment = view, .clearColor = true, .width = 32, .height = 64});
  command->BindPipeline(pipeline);
  command->BindTexture(0, a);
  command->Draw(PrimitiveTopology::TriangleList, 3, 0);
  command->EndRendering();
  command->BeginRendering(
      {.colorAttachment = view, .width = 32, .height = 64, .offsetX = 32});
  command->BindPipeline(pipeline);
  command->BindTexture(0, b);
  command->Draw(PrimitiveTopology::TriangleList, 3, 0);
  command->EndRendering();
  command->End();
  device->Submit(*command);
  auto pixels = device->ReadImage(target);
  EXPECT_GT(std::to_integer<unsigned>(pixels[(32 * 64 + 16) * 4]), 250u);
  EXPECT_GT(std::to_integer<unsigned>(pixels[(32 * 64 + 48) * 4 + 1]), 250u);
  EXPECT_LT(std::to_integer<unsigned>(pixels[(32 * 64 + 16) * 4 + 1]), 5u);
  command->Clear();
  command->Begin();
  command->BeginRendering({.colorAttachment = view,
                           .clearColor = true,
                           .clearColorValue = {0, 0, 1},
                           .width = 64,
                           .height = 64});
  command->EndRendering();
  command->End();
  device->Submit(*command);
  pixels = device->ReadImage(target);
  EXPECT_GT(std::to_integer<unsigned>(pixels[(32 * 64 + 16) * 4 + 2]), 250u);
  EXPECT_LT(std::to_integer<unsigned>(pixels[(32 * 64 + 16) * 4]), 5u);
}
} // namespace
namespace
{
TEST_P(HeadlessDeviceTests, DepthOcclusionAndDepthBlit)
{
  const auto vertex = Shader(R"(#version 450
layout(location=0) in vec3 position;void main(){gl_Position=vec4(position,1);}
)",
                             ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
layout(set=0,binding=0) uniform Tint {vec4 color;} tint;
layout(location=0) out vec4 color;void main(){color=tint.color;}
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
       .vertexLayout = {.bindings = {{.binding = 0, .stride = 12}},
                        .attributes = {{.location = 0,
                                        .binding = 0,
                                        .format = VertexFormat::Float3}}},
       .colorFormats = {ImageFormat::RGBA8},
       .depthFormat = ImageFormat::Depth32,
       .layout = layout});
  const std::array nearVertices{-1.f, -1.f, .2f, 3.f, -1.f,
                                .2f,  -1.f, 3.f, .2f};
  const std::array farVertices{-1.f, -1.f, .8f, 3.f, -1.f, .8f, -1.f, 3.f, .8f};
  const auto nearBuffer = device->CreateBuffer(
      {.size = 36,
       .usage = BufferUsage::Vertex,
       .initialData = std::as_bytes(std::span(nearVertices))});
  const auto farBuffer = device->CreateBuffer(
      {.size = 36,
       .usage = BufferUsage::Vertex,
       .initialData = std::as_bytes(std::span(farVertices))});
  const std::array green{0.f, 1.f, 0.f, 1.f}, red{1.f, 0.f, 0.f, 1.f};
  const auto makeSet = [&](const auto& color)
  {
    auto buffer =
        device->CreateBuffer({.size = 16,
                              .usage = BufferUsage::Uniform,
                              .initialData = std::as_bytes(std::span(color))});
    auto set = device->CreateDescriptorSet({setLayout});
    const std::array writes{
        DescriptorWrite{.resource = DescriptorBufferInfo{buffer}}};
    device->UpdateDescriptorSet(set, writes);
    return set;
  };
  const auto greenSet = makeSet(green), redSet = makeSet(red);
  const auto makeDepth = [&]
  {
    return device->CreateImage(
        {.width = 64,
         .height = 64,
         .format = ImageFormat::Depth32,
         .usage = {ImageUsage::DepthAttachment, ImageUsage::TransferSource,
                   ImageUsage::TransferDestination}});
  };
  const auto depthA = makeDepth(), depthB = makeDepth();
  const auto depthViewA = device->CreateImageView(
                 {.image = depthA, .format = ImageFormat::Depth32}),
             depthViewB = device->CreateImageView(
                 {.image = depthB, .format = ImageFormat::Depth32});
  const auto colorA = Target(), colorB = Target();
  const auto viewA = View(colorA), viewB = View(colorB);
  auto command = device->CreateCommandList(QueueType::Graphics);
  command->Begin();
  command->BeginRendering({.colorAttachment = viewA,
                           .depthAttachment = depthViewA,
                           .clearColor = true,
                           .clearDepth = true,
                           .width = 64,
                           .height = 64});
  command->BindPipeline(pipeline);
  command->BindDescriptorSet(0, greenSet);
  command->BindVertexBuffer(nearBuffer, 12);
  command->Draw(PrimitiveTopology::TriangleList, 3, 0);
  command->BindDescriptorSet(0, redSet);
  command->BindVertexBuffer(farBuffer, 12);
  command->Draw(PrimitiveTopology::TriangleList, 3, 0);
  command->EndRendering();
  const BlitRegion region{.srcSubresource = {ImageAspect::Depth, 0, 0, 1},
                          .srcOffsets = {{{0, 0, 0}, {64, 64, 1}}},
                          .dstSubresource = {ImageAspect::Depth, 0, 0, 1},
                          .dstOffsets = {{{0, 0, 0}, {64, 64, 1}}}};
  command->BlitImage(depthA, depthB, region, ImageFilter::Nearest);
  command->BeginRendering({.colorAttachment = viewB,
                           .depthAttachment = depthViewB,
                           .clearColor = true,
                           .width = 64,
                           .height = 64});
  command->BindPipeline(pipeline);
  command->BindDescriptorSet(0, redSet);
  command->BindVertexBuffer(farBuffer, 12);
  command->Draw(PrimitiveTopology::TriangleList, 3, 0);
  command->EndRendering();
  command->End();
  device->Submit(*command);
  const auto a = device->ReadImage(colorA), b = device->ReadImage(colorB);
  EXPECT_GT(std::to_integer<unsigned>(a[(32 * 64 + 32) * 4 + 1]), 250u);
  EXPECT_LT(std::to_integer<unsigned>(a[(32 * 64 + 32) * 4]), 5u);
  EXPECT_LT(std::to_integer<unsigned>(b[(32 * 64 + 32) * 4]), 5u);
}
} // namespace
namespace
{
TEST_P(HeadlessDeviceTests, IndexedDrawUsesPipelineLineTopology)
{
  const auto vertex = Shader(R"(#version 450
layout(location=0) in vec3 position;void main(){gl_Position=vec4(position,1);}
)",
                             ShaderStage::Vertex);
  const auto fragment = Shader(R"(#version 450
layout(location=0) out vec4 color;void main(){color=vec4(1);}
)",
                               ShaderStage::Fragment);
  const auto layout = device->CreatePipelineLayout({});
  const auto pipeline = device->CreatePipeline(
      {.vertexShader = vertex,
       .fragmentShader = fragment,
       .vertexLayout = {.bindings = {{.binding = 0, .stride = 12}},
                        .attributes = {{.location = 0,
                                        .binding = 0,
                                        .format = VertexFormat::Float3}}},
       .topology = PrimitiveTopology::LineList,
       .colorFormats = {ImageFormat::RGBA8},
       .layout = layout});
  const std::array vertices{-.8f, 0.f, .5f, .8f, 0.f, .5f};
  const std::array<std::uint32_t, 2> indices{0, 1};
  const auto vb =
      device->CreateBuffer({.size = sizeof(vertices),
                            .usage = BufferUsage::Vertex,
                            .initialData = std::as_bytes(std::span(vertices))});
  const auto ib =
      device->CreateBuffer({.size = sizeof(indices),
                            .usage = BufferUsage::Index,
                            .initialData = std::as_bytes(std::span(indices))});
  const auto image = Target();
  const auto view = View(image);
  auto command = device->CreateCommandList(QueueType::Graphics);
  command->Begin();
  command->BeginRendering(
      {.colorAttachment = view, .clearColor = true, .width = 64, .height = 64});
  command->BindPipeline(pipeline);
  command->BindVertexBuffer(vb, 12);
  command->BindIndexBuffer(ib);
  command->DrawIndexed(2);
  command->EndRendering();
  command->End();
  device->Submit(*command);
  const auto pixels = device->ReadImage(image);
  unsigned lit = 0;
  for (unsigned y = 30; y <= 33; ++y)
    for (unsigned x = 8; x < 56; ++x)
      if (std::to_integer<unsigned>(pixels[(y * 64 + x) * 4]) > 200)
        ++lit;
  EXPECT_GT(lit, 8u);
}
} // namespace
