#if defined(LIGHTHOUSE_IMGUI) && defined(__linux__)
#include <Lighthouse/Engine/Rendering/ImGuiView.hpp>
#include <Lighthouse/Engine/Rendering/Renderer.hpp>
#include <Lighthouse/Backends/Render/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Backends/Render/Vulkan/VulkanBackend.hpp>
#include <gtest/gtest.h>
#include <imgui.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/Xutil.h>

using namespace Lighthouse;
namespace
{
class ImGuiViewportPixels : public testing::TestWithParam<bool>
{
};
TEST_P(ImGuiViewportPixels, SecondaryViewportContainsRenderedPixels)
{
  if (!std::getenv("DISPLAY"))
    GTEST_SKIP() << "X11 display required";
  WindowManager windows;
  std::unique_ptr<IGraphicsBackend> backend;
  if (GetParam())
    backend = std::make_unique<VulkanBackend>();
  else
    backend = std::make_unique<OpenGLBackend>();
  std::string diagnostic;
  ASSERT_TRUE(backend->Initialize(diagnostic)) << diagnostic;
  Renderer renderer(backend->CreateDevice({}), &windows);
  ImGuiContext* context{};
  ImGuiViewport* viewport{};
  ImVec2 windowSize{320, 200};
  renderer.CreateView<ImGuiView>(
      [&]
      {
        context = ImGui::GetCurrentContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::SetNextWindowPos({80, 80}, ImGuiCond_Always);
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
        ImGui::Begin("Backend ImGui pixel regression", nullptr,
                     ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("Visible text and geometry");
        auto p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled({p.x + 10, p.y + 10},
                                                  {p.x + 110, p.y + 70},
                                                  IM_COL32(255, 0, 255, 255));
        viewport = ImGui::GetWindowViewport();
        ImGui::End();
      });
  renderer.Compile();
  for (int frame = 0; frame < 8; ++frame)
  {
    windows.PollEvents();
    renderer.Render();
    renderer.GetRenderDevice()->WaitIdle();
    glfwWaitEventsTimeout(.02);
  }
  ASSERT_NE(context, nullptr);
  ASSERT_NE(viewport, nullptr);
  ASSERT_NE(viewport->PlatformHandle, nullptr);
  ImGui::SetCurrentContext(context);
  EXPECT_NE(viewport, ImGui::GetMainViewport());
  EXPECT_TRUE(ImGui::GetIO().BackendFlags &
              ImGuiBackendFlags_RendererHasViewports);
  ASSERT_NE(viewport->DrawData, nullptr);
  EXPECT_GT(viewport->DrawData->TotalVtxCount, 0);
  std::cout << "ImGui draw size " << viewport->DrawData->DisplaySize.x << "x"
            << viewport->DrawData->DisplaySize.y << " scale "
            << viewport->DrawData->FramebufferScale.x << ","
            << viewport->DrawData->FramebufferScale.y << "\n";
  ImGui::SetCurrentContext(nullptr);
  auto* native = static_cast<GLFWwindow*>(viewport->PlatformHandle);
  ASSERT_TRUE(glfwGetWindowAttrib(native, GLFW_VISIBLE));
  EXPECT_EQ(glfwGetWindowAttrib(native, GLFW_CLIENT_API),
            GetParam() ? GLFW_NO_API : GLFW_OPENGL_API);
  if (GetParam())
    ASSERT_NE(viewport->RendererUserData, nullptr);
  windowSize = {420, 280};
  glfwSetWindowSize(native, 420, 280);
  for (int frame = 0; frame < 4; ++frame)
  {
    windows.PollEvents();
    renderer.Render();
    renderer.GetRenderDevice()->WaitIdle();
    glfwWaitEventsTimeout(.02);
  }
  int width{}, height{};
  glfwGetFramebufferSize(native, &width, &height);
  EXPECT_EQ(width, 420);
  EXPECT_EQ(height, 280);
  auto* display = glfwGetX11Display();
  XSync(display, False);
  auto* pixels = XGetImage(display, glfwGetX11Window(native), 0, 0, width,
                           height, AllPlanes, ZPixmap);
  ASSERT_NE(pixels, nullptr);
  unsigned colored = 0;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
    {
      auto value = XGetPixel(pixels, x, y);
      if ((value & pixels->red_mask) && !(value & pixels->green_mask) &&
          (value & pixels->blue_mask))
        ++colored;
    }
  XDestroyImage(pixels);
  EXPECT_GT(colored, 3000u)
      << "ImGui platform window has no rendered magenta rectangle";
}
INSTANTIATE_TEST_SUITE_P(Backends, ImGuiViewportPixels,
                         testing::Values(false, true), [](const auto& info)
                         { return info.param ? "Vulkan14" : "OpenGL"; });
} // namespace
#endif
