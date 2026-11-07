#pragma once

#include <Lighthouse/Engine/Assets/Mesh.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace EESG::Detail
{
// Indexed meshes share Mesh3DVertex's layout. normal.x tags the draw shape:
// 0 = fullscreen/directional, 1 = point sphere, 2 = spot cone.
inline std::array<Lighthouse::Mesh3D, 3> MakeLightVolumes()
{
  using namespace Lighthouse;
  constexpr uint32_t segments = 32, rings = 16;
  constexpr float pi = std::numbers::pi_v<float>;
  std::array<Mesh3D, 3> meshes;
  meshes[0].vertices = {{{-1, -1, 0}}, {{3, -1, 0}}, {{-1, 3, 0}}};
  meshes[0].indices = {0, 1, 2};
  auto triangle =
      [](Mesh3D& mesh, uint32_t a, uint32_t b, uint32_t c, Vec3 inside)
  {
    const auto p = mesh.vertices[a].position;
    const auto q = mesh.vertices[b].position;
    const auto r = mesh.vertices[c].position;
    if (glm::dot(glm::cross(q - p, r - p), p - inside) < 0)
      std::swap(b, c);
    mesh.indices.insert(mesh.indices.end(), {a, b, c});
  };
  auto& sphere = meshes[1];
  for (uint32_t y = 0; y <= rings; ++y)
    for (uint32_t x = 0; x <= segments; ++x)
    {
      const float theta = pi * y / rings, phi = 2 * pi * x / segments;
      sphere.vertices.push_back(
          {{std::sin(theta) * std::cos(phi), std::cos(theta),
            std::sin(theta) * std::sin(phi)},
           {1, 0, 0}});
    }
  for (uint32_t y = 0; y < rings; ++y)
    for (uint32_t x = 0; x < segments; ++x)
    {
      const uint32_t a = y * (segments + 1) + x, b = a + segments + 1;
      if (y != 0)
        triangle(sphere, a, b, a + 1, Vec3(0));
      if (y + 1 != rings)
        triangle(sphere, a + 1, b, b + 1, Vec3(0));
    }
  // Circumscribe the analytic sphere so faceting never drops lit edge pixels.
  float nearestPlane = std::numeric_limits<float>::max();
  for (size_t i = 0; i < sphere.indices.size(); i += 3)
  {
    const auto a = sphere.vertices[sphere.indices[i]].position;
    const auto b = sphere.vertices[sphere.indices[i + 1]].position;
    const auto c = sphere.vertices[sphere.indices[i + 2]].position;
    nearestPlane = std::min(
        nearestPlane, glm::dot(glm::normalize(glm::cross(b - a, c - a)), a));
  }
  for (auto& vertex : sphere.vertices)
    vertex.position /= nearestPlane;
  auto& cone = meshes[2];
  cone.vertices.push_back({{0, 0, 0}, {2, 0, 0}});
  cone.vertices.push_back({{0, 0, -1}, {2, 0, 0}});
  const float radius = 1 / std::cos(pi / segments);
  for (uint32_t i = 0; i < segments; ++i)
  {
    const float angle = 2 * pi * i / segments;
    cone.vertices.push_back(
        {{radius * std::cos(angle), radius * std::sin(angle), -1}, {2, 0, 0}});
  }
  for (uint32_t i = 0; i < segments; ++i)
  {
    const auto a = 2 + i, b = 2 + (i + 1) % segments;
    triangle(cone, 0, a, b, {0, 0, -0.5f});
    triangle(cone, 1, a, b, {0, 0, -0.5f});
  }
  return meshes;
}
} // namespace EESG::Detail
