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

option(EESG_BUILD_TESTING "Build EESG tests" OFF)

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
    enable_testing()
    add_executable(EESGOctreeTests tests/AdaptiveOctreeTests.cpp)
    target_include_directories(EESGOctreeTests PRIVATE src/Types)
    add_test(NAME EESGOctreeTests COMMAND EESGOctreeTests)

    add_executable(EESGWorldTerrainTests tests/WorldTerrainTests.cpp)
    target_include_directories(EESGWorldTerrainTests PRIVATE src)
    add_test(NAME EESGWorldTerrainTests COMMAND EESGWorldTerrainTests)

    find_package(GTest CONFIG REQUIRED)
    add_executable(EESGBackendTests tests/BackendDeviceTests.cpp tests/ImGuiViewportTests.cpp
        extern/Lighthouse-Engine/Tests/GraphicsBackendTests.cpp)
    target_link_libraries(EESGBackendTests PRIVATE Lighthouse::OpenGLBackend
        Lighthouse::VulkanBackend GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(EESGBackendTests)
endif()
