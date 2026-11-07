#include "Materials/MaterialDefinition.hpp"

#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace EESG::Detail
{
namespace
{
using Json = nlohmann::ordered_json;
using Slot = std::array<uint32_t, 4>;

bool Identifier(const std::string& name)
{
  auto letter = [](char c)
  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
  if (name.empty() || !letter(name[0]) || name.starts_with("gl_") ||
      name.starts_with("EESG_") || name.find("__") != std::string::npos)
    return false;
  for (auto c : name)
    if (!letter(c) && !(c >= '0' && c <= '9'))
      return false;
  return true;
}
Json ToJson(const MaterialValue& value)
{
  return std::visit(
      [](const auto& v) -> Json
      {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_arithmetic_v<T>)
          return v;
        else
        {
          Json result = Json::array();
          for (int i = 0; i < v.length(); ++i)
            result.push_back(v[i]);
          return result;
        }
      },
      value);
}
uint32_t FloatBits(const Json& value)
{
  if (!value.is_number())
    throw std::runtime_error("Expected a finite floating-point value");
  const double v = value.get<double>();
  if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
    throw std::runtime_error("Float parameter is out of range");
  return std::bit_cast<uint32_t>(static_cast<float>(v));
}
Slot Encode(const std::string& type, const Json& value)
{
  Slot result{};
  if (type == "float")
    result[0] = FloatBits(value);
  else if (type == "bool")
  {
    if (!value.is_boolean())
      throw std::runtime_error("Expected a boolean");
    result[0] = value.get<bool>();
  }
  else if (type == "int" || type == "uint")
  {
    if (!value.is_number_integer())
      throw std::runtime_error("Expected an integer");
    const long double v = value.is_number_unsigned()
                              ? static_cast<long double>(value.get<uint64_t>())
                              : static_cast<long double>(value.get<int64_t>());
    const long double low =
        type == "int" ? std::numeric_limits<int32_t>::min() : 0;
    const long double high = type == "int"
                                 ? std::numeric_limits<int32_t>::max()
                                 : std::numeric_limits<uint32_t>::max();
    if (v < low || v > high)
      throw std::runtime_error("Integer parameter is out of range");
    result[0] = type == "int" ? std::bit_cast<uint32_t>(static_cast<int32_t>(v))
                              : static_cast<uint32_t>(v);
  }
  else
  {
    const int count = type == "vec2"   ? 2
                      : type == "vec3" ? 3
                      : type == "vec4" ? 4
                                       : 0;
    if (!count)
      throw std::runtime_error("Unsupported material parameter type: " + type);
    if (!value.is_array() || value.size() != size_t(count))
      throw std::runtime_error("Wrong vector parameter size");
    for (int i = 0; i < count; ++i)
      result[i] = FloatBits(value[i]);
  }
  return result;
}
std::string Decode(const MaterialParameter& parameter)
{
  const auto raw = "EESG_data.values[" + std::to_string(parameter.slot) + "]";
  if (parameter.type == "float")
    return "uintBitsToFloat(" + raw + ".x)";
  if (parameter.type == "int")
    return "int(" + raw + ".x)";
  if (parameter.type == "uint")
    return raw + ".x";
  if (parameter.type == "bool")
    return "(" + raw + ".x != 0u)";
  const auto swizzle = parameter.type == "vec2"   ? ".xy"
                       : parameter.type == "vec3" ? ".xyz"
                                                  : "";
  return "uintBitsToFloat(" + raw + swizzle + ")";
}
} // namespace

MaterialDefinition ReadDefinition(const std::filesystem::path& path,
                                  const MaterialLoadOptions& options,
                                  bool schemaOnly)
{
  MaterialDefinition result;
  const bool legacy = path.extension() == ".glsl";
  if (!legacy && path.extension() != ".json")
    throw std::runtime_error("Expected .json or .glsl material");
  Json parameters = Json::object();
  if (legacy)
  {
    if (!options.parameters.empty() || !options.textures.empty())
      throw std::runtime_error(
          "Exposed parameters require a JSON material definition");
    result.shader = path;
    result.data.resize(6);
    result.data[0] = Encode("vec4", Json::array({1, 1, 1, 1}));
  }
  else
  {
    std::ifstream file(path);
    if (!file)
      throw std::runtime_error("Cannot read material JSON");
    const auto definition = Json::parse(file);
    if (!definition.is_object())
      throw std::runtime_error("Material definition must be an object");
    for (auto it = definition.begin(); it != definition.end(); ++it)
      if (it.key() != "shader" && it.key() != "base" &&
          it.key() != "parameters")
        throw std::runtime_error("Unknown material field: " + it.key());
    const auto base = definition.value("base", std::string("Unlit"));
    if (base != "Unlit" && base != "Lit")
      throw std::runtime_error("Unknown material base: " + base);
    result.model = base == "Lit" ? SurfaceModel::Lit : SurfaceModel::Unlit;
    result.shader = std::filesystem::canonical(
        path.parent_path() / definition.at("shader").get<std::string>());
    parameters = definition.value("parameters", Json::object());
    if (!parameters.is_object())
      throw std::runtime_error("Material parameters must be an object");
  }
  std::set<std::string> usedValues, usedTextures;
  for (auto it = parameters.begin(); it != parameters.end(); ++it)
  {
    const auto& name = it.key();
    if (!Identifier(name))
      throw std::runtime_error("Invalid GLSL parameter name: " + name);
    const auto& desc = it.value();
    if (!desc.is_object())
      throw std::runtime_error("Parameter declaration must be an object: " +
                               name);
    for (auto field = desc.begin(); field != desc.end(); ++field)
      if (field.key() != "type" && field.key() != "default")
        throw std::runtime_error("Unknown parameter field: " + field.key());
    const auto type = desc.at("type").get<std::string>();
    MaterialParameter parameter{name, type, uint32_t(result.data.size())};
    Slot data{};
    if (type == "texture2D")
    {
      if (desc.contains("default"))
        throw std::runtime_error("Texture parameter '" + name +
                                 "' cannot declare a default");
      const auto texture = options.textures.find(name);
      if (!schemaOnly && (texture == options.textures.end() ||
                          !texture->second.image || !texture->second.sampler))
        throw std::runtime_error(
            "Missing explicit texture and sampler for parameter '" + name +
            "'");
      data[0] = uint32_t(result.textures.size());
      result.textures.push_back(schemaOnly ? Lighthouse::DescriptorImageInfo{}
                                           : texture->second);
      usedTextures.insert(name);
    }
    else
    {
      const auto supplied = options.parameters.find(name);
      if (supplied != options.parameters.end())
      {
        data = Encode(type, ToJson(supplied->second));
        usedValues.insert(name);
      }
      else if (desc.contains("default"))
        data = Encode(type, desc.at("default"));
      else if (!schemaOnly)
        throw std::runtime_error("Missing value for parameter '" + name + "'");
      else
        data = Encode(type, type == "bool"   ? Json(false)
                            : type == "vec2" ? Json::array({0, 0})
                            : type == "vec3" ? Json::array({0, 0, 0})
                            : type == "vec4" ? Json::array({0, 0, 0, 0})
                                             : Json(0));
    }
    result.parameters.push_back(parameter);
    result.data.push_back(data);
  }
  for (const auto& [name, value] : options.parameters)
    if (!usedValues.contains(name))
      throw std::runtime_error("Unknown or non-numeric parameter override: " +
                               name);
  for (const auto& [name, value] : options.textures)
    if (!usedTextures.contains(name))
      throw std::runtime_error("Unknown texture binding: " + name);
  if (result.data.empty())
    result.data.resize(1);
  result.declarations = "layout(set=1, binding=0, std140) uniform "
                        "EESG_ParameterData { uvec4 values[" +
                        std::to_string(result.data.size()) +
                        "]; } EESG_data;\nstruct MaterialParameters {\n";
  if (legacy)
    result.declarations += "vec4 baseColor; vec4 emission; vec4 custom[4];\n";
  else if (result.parameters.empty())
    result.declarations += "uint EESG_unused;\n";
  else
    for (const auto& p : result.parameters)
      result.declarations +=
          (p.type == "texture2D" ? "uint" : p.type) + " " + p.name + ";\n";
  result.declarations +=
      "};\nMaterialParameters EESG_LoadParameters() { MaterialParameters p;\n";
  if (legacy)
    result.declarations +=
        "p.baseColor=uintBitsToFloat(EESG_data.values[0]); "
        "p.emission=uintBitsToFloat(EESG_data.values[1]); for(int i=0;i<4;++i) "
        "p.custom[i]=uintBitsToFloat(EESG_data.values[i+2]);\n";
  else if (result.parameters.empty())
    result.declarations += "p.EESG_unused=0u;\n";
  else
    for (const auto& p : result.parameters)
      result.declarations +=
          "p." + p.name + "=" +
          (p.type == "texture2D"
               ? "EESG_data.values[" + std::to_string(p.slot) + "].x"
               : Decode(p)) +
          ";\n";
  result.declarations += "return p; }\n";
  for (size_t i = 0; i < result.textures.size(); ++i)
    result.declarations += "layout(set=1,binding=" + std::to_string(i + 1) +
                           ") uniform sampler2D EESG_texture" +
                           std::to_string(i) + ";\n";
  result.declarations += "vec4 SampleTexture2D(uint textureIndex, vec2 uv) { "
                         "switch(textureIndex) {\n";
  for (size_t i = 0; i < result.textures.size(); ++i)
  {
    result.declarations +=
        "case " + std::to_string(i) +
        "u:\n#ifdef EESG_RAY_TRACING\nreturn textureLod(EESG_texture" +
        std::to_string(i) + ",uv,0.0);\n#else\nreturn texture(EESG_texture" +
        std::to_string(i) + ",uv);\n#endif\n";
  }
  result.declarations += "default: return vec4(0.0); } }\n";
  return result;
}
} // namespace EESG::Detail
