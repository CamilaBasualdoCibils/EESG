cmake_minimum_required(VERSION 3.14)

# The toolchain must be selected before project() initializes the compilers.
# Respect an explicitly supplied toolchain, including one from a preset.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT}
   AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "CMake toolchain file")
endif()

project(EESG VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LIGHTHOUSE_IMGUI "Build ImGui views and use GLFW X11/XWayland (OFF selects Wayland)" ON)
option(EESG_ENABLE_TRACY "Build the application with Tracy profiler enabled" ON)
option(EESG_BUILD_TESTING "Build Lighthouse tests" OFF)

set(BUILD_TESTING ${EESG_BUILD_TESTING} CACHE BOOL "Build tests" FORCE)
set(OPENPERIPHERAL_BUILD_TESTS OFF CACHE BOOL "Build OpenPeripherals tests" FORCE)
set(OPENPERIPHERAL_BUILD_TOOLS OFF CACHE BOOL "Build OpenPeripherals tools" FORCE)
set(OPENPERIPHERAL_BUILD_EXAMPLES OFF CACHE BOOL "Build OpenPeripherals examples" FORCE)
set(VISR_BUILD_TESTS OFF CACHE BOOL "Build VISR tests" FORCE)
set(VISR_BUILD_TOOLS OFF CACHE BOOL "Build VISR tools" FORCE)
set(LIGHTHOUSE_BUILD_EXAMPLES OFF CACHE BOOL "Build Lighthouse examples" FORCE)
set(LIGHTHOUSE_BUILD_OPENGL_BACKEND ON CACHE BOOL "Build the OpenGL backend" FORCE)
set(LIGHTHOUSE_BUILD_VULKAN_BACKEND ON CACHE BOOL "Build the Vulkan backend" FORCE)
set(LIGHTHOUSE_ENABLE_TRACY ${EESG_ENABLE_TRACY} CACHE BOOL "Enable Tracy profiler" FORCE)

add_subdirectory(extern/Lighthouse-Engine)

file(GLOB_RECURSE _EESG_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(EESG ${_EESG_sources})
target_include_directories(EESG PRIVATE src)
target_link_libraries(EESG PRIVATE
    Lighthouse::Engine
    Lighthouse::OpenGLBackend
    Lighthouse::VulkanBackend
)

if(EESG_BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(EESGBackendTests tests/BackendDeviceTests.cpp tests/ImGuiViewportTests.cpp
        extern/Lighthouse-Engine/Tests/GraphicsBackendTests.cpp)
    target_link_libraries(EESGBackendTests PRIVATE Lighthouse::OpenGLBackend
        Lighthouse::VulkanBackend GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(EESGBackendTests)
endif()
