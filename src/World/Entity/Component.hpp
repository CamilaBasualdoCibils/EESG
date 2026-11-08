#pragma once
#ifdef LIGHTHOUSE_IMGUI
#include <algorithm>
#include <boost/describe/enum_to_string.hpp>
#include <entt/entt.hpp>
#include <imgui.h>
#include <vector>
namespace EESG
{
struct ComponentInspectorInfo
{
  const char* name;
};
inline constexpr auto InspectComponentID =
    entt::hashed_string::value("InspectComponent");
inline constexpr auto InspectEntityID =
    entt::hashed_string::value("InspectEntity");
template <class Type>
bool InspectEntityComponent(entt::registry& registry, entt::entity entity)
{
  if (auto* component = registry.try_get<Type>(entity))
    return Type::InspectComponent(*component);
  return false;
}
template <class Type> bool RegisterComponentInspector(const char* name)
{
  entt::meta_factory<Type>{}
      .type(entt::type_hash<Type>::value())
      .template func<&Type::InspectComponent>(InspectComponentID)
      .template func<&InspectEntityComponent<Type>>(InspectEntityID);
  entt::meta_factory<Type>{}.template custom<ComponentInspectorInfo>(name);
  return true;
}
} // namespace EESG
// Usage inside a component: EESG_COMPONENT_INSPECTOR(Type, { /* draw component
// */ return false; }) The inline body and EnTT registration disappear when
// ImGui is disabled. Return true only when an edit requires rebuilding the
// render graph.
#define EESG_COMPONENT_INSPECTOR(Type, ...)                                    \
private:                                                                       \
  inline static const bool s_inspectorRegistered =                             \
      ::EESG::RegisterComponentInspector<Type>(#Type);                         \
                                                                               \
public:                                                                        \
  static bool InspectComponent(Type& component) __VA_ARGS__
#else
#define EESG_COMPONENT_INSPECTOR(Type, ...)
#endif
