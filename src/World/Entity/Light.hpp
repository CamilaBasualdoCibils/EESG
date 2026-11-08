#pragma once
#include "Component.hpp"

#include "Math/Types.hpp"
#include <cstdint>
#include <boost/describe/enum.hpp>

namespace EESG
{
enum class LightType : uint32_t
{
  Directional,
  Point,
  Spot
};

BOOST_DESCRIBE_ENUM(LightType, Directional, Point, Spot)

// Position and orientation come from Transform. Local -Z is the emission
// direction. Directional intensity is radiance; local lights use intensity
// scaled by inverse-square distance. These are unshadowed lights.
struct Light
{
  EESG_COMPONENT_INSPECTOR(Light, {
    ImGui::Checkbox("Enabled", &component.enabled);
    if (ImGui::BeginCombo("Type", boost::describe::enum_to_string(component.type, "Unknown"))) {
        for (auto type : {LightType::Directional, LightType::Point, LightType::Spot})
            if (ImGui::Selectable(boost::describe::enum_to_string(type, "Unknown"), component.type == type)) component.type = type;
        ImGui::EndCombo();
    }
    ImGui::ColorEdit3("Color", &component.color.x);
    ImGui::DragFloat("Intensity", &component.intensity, .1f, 0, 100000);
    ImGui::DragFloat("Range", &component.range, .1f, .001f, 100000);
    ImGui::DragFloat("Inner angle", &component.innerAngle, .1f, 0, component.outerAngle);
    ImGui::DragFloat("Outer angle", &component.outerAngle, .1f, component.innerAngle, 89.9f);
    return false;
})
  LightType type = LightType::Directional;
  Vec3 color{1.0f};
  float intensity = 1.0f;
  float range = 10.0f;
  float innerAngle = 20.0f; // Cone half angles in degrees; Spot only.
  float outerAngle = 30.0f;
  bool enabled = true;
};
} // namespace EESG
