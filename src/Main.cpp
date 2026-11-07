#include "GameManager.hpp"
#include <Lighthouse/Backends/Input/GLFW/GLFWInputBackend.hpp>
#include <Lighthouse/Backends/Render/OpenGL/OpenGLBackend.hpp>
#include <Lighthouse/Backends/Render/Vulkan/VulkanBackend.hpp>
#include <Lighthouse/Engine/Engine.hpp>
#include <boost/program_options.hpp>

int main(int argc, char** argv)
{
  boost::program_options::options_description desc{"Options"};
  desc.add_options()("vulkan", "Use Vulkan backend")("opengl",
                                                     "Use OpenGL backend");
  boost::program_options::variables_map vm;
  boost::program_options::store(
      boost::program_options::parse_command_line(argc, argv, desc), vm);
  boost::program_options::notify(vm);
  Lighthouse::Config config{
      .name = "EESG",
      .graphics = {.selection = Lighthouse::BackendSelection::FirstAvailable}};
  config.AddInputBackend<Lighthouse::GLFWInputBackend>();
  if (vm.count("vulkan"))
    config.AddRenderBackend<Lighthouse::VulkanBackend>();
  if (vm.count("opengl"))
    config.AddRenderBackend<Lighthouse::OpenGLBackend>();
  if (!vm.count("vulkan") && !vm.count("opengl"))
  {
    config.AddRenderBackend<Lighthouse::VulkanBackend>();
    config.AddRenderBackend<Lighthouse::OpenGLBackend>();
  }
  return Run<EESG::GameManager>(config);
}
