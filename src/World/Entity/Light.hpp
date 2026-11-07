#pragma once

#include "Math/Types.hpp"
#include <cstdint>

namespace EESG
{
enum class LightType : uint32_t
{
  Directional,
  Point,
  Spot
};

// Position and orientation come from Transform. Local -Z is the emission
// direction. Directional intensity is radiance; local lights use intensity
// scaled by inverse-square distance. These are unshadowed lights.
struct Light
{
  LightType type = LightType::Directional;
  Vec3 color{1.0f};
  float intensity = 1.0f;
  float range = 10.0f;
  float innerAngle = 20.0f; // Cone half angles in degrees; Spot only.
  float outerAngle = 30.0f;
  bool enabled = true;
};
} // namespace EESG
