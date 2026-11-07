#pragma once

#include <Lighthouse/Engine/Rendering/IRenderDevice.hpp>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <boost/describe.hpp>
namespace EESG
{
struct alignas(16) MaterialFrameData
{
  Lighthouse::Mat4 viewProjection{1.0f};
  Lighthouse::Mat4 model{1.0f};
};
static_assert(sizeof(MaterialFrameData) == 128);

enum class SurfaceModel
{
  Unlit,
  Lit
};
BOOST_DESCRIBE_ENUM(EESG::SurfaceModel, Unlit, Lit)

using MaterialValue =
    std::variant<float, int32_t, uint32_t, bool, Lighthouse::Vec2,
                 Lighthouse::Vec3, Lighthouse::Vec4>;

struct MaterialLoadOptions
{
  std::map<std::string, MaterialValue> parameters;
  // Explicit image-view/sampler pairs. No implicit texture or sampler defaults.
  std::map<std::string, Lighthouse::DescriptorImageInfo> textures;
};

struct MaterialParameter
{
  std::string name;
  std::string type;
  uint32_t slot = 0; // Each numeric value occupies one 16-byte GPU slot.
};

enum class GBufferAttribute
{
  Color,
  WorldNormal,
  WorldPosition,
  SurfaceProperties,
  Emission
};
struct GBufferAttributeDeclaration
{
  GBufferAttribute attribute;
  const char* name;
  Lighthouse::ImageFormat format;
};
// Stable locations shared by the initial shading models.
inline constexpr std::array GBufferAttributes{
    GBufferAttributeDeclaration{GBufferAttribute::Color, "Color",
                                Lighthouse::ImageFormat::RGBA16f},
    GBufferAttributeDeclaration{GBufferAttribute::WorldNormal, "World normal",
                                Lighthouse::ImageFormat::RGBA16f},
    GBufferAttributeDeclaration{GBufferAttribute::WorldPosition,
                                "World position",
                                Lighthouse::ImageFormat::RGBA32f},
    GBufferAttributeDeclaration{GBufferAttribute::SurfaceProperties,
                                "Surface properties",
                                Lighthouse::ImageFormat::RGBA16f},
    GBufferAttributeDeclaration{GBufferAttribute::Emission, "Emission",
                                Lighthouse::ImageFormat::RGBA16f}};

// std430 layout shared with EvaluateLight.glsl. Records follow the frame
// header.
struct alignas(16) LightingFrameData
{
  Lighthouse::Vec4 cameraPosition{0.0f, 0.0f, 3.6f, 1.0f};
  Lighthouse::Vec4 ambientRadiance{0.03f, 0.03f, 0.03f, 0.0f};
  std::array<uint32_t, 4> lightCount{};
  Lighthouse::Mat4 viewProjection{1.0f};
};
struct alignas(16) LightData
{
  Lighthouse::Vec4 colorIntensity{};
  Lighthouse::Vec4 positionRange{};
  Lighthouse::Vec4 directionType{};
  Lighthouse::Vec4 spotCosines{};
};
static_assert(sizeof(LightingFrameData) == 112);
static_assert(offsetof(LightingFrameData, lightCount) == 32);
static_assert(sizeof(LightData) == 64);
static_assert(offsetof(LightData, positionRange) == 16);
static_assert(offsetof(LightData, directionType) == 32);
static_assert(offsetof(LightData, spotCosines) == 48);

struct DeferredLightingPipeline
{
  Lighthouse::GraphicsPipelineHandle pipeline{};
  Lighthouse::PipelineLayoutHandle layout{};
  Lighthouse::DescriptorSetLayoutHandle attributesLayout{};
  Lighthouse::DescriptorSetLayoutHandle lightingLayout{}; // Optional set 1.
};
struct ShadingModel
{
  ~ShadingModel();
  ShadingModel(const ShadingModel&) = delete;
  ShadingModel& operator=(const ShadingModel&) = delete;
  SurfaceModel model = SurfaceModel::Unlit;
  std::vector<GBufferAttribute> attributes;
  DeferredLightingPipeline deferredLightingPipeline;

private:
  friend class Materials;
  explicit ShadingModel(Lighthouse::IRenderDevice& device) : m_device(device) {}
  Lighthouse::IRenderDevice& m_device;
  Lighthouse::ShaderModuleHandle m_vertex{}, m_fragment{};
};
struct MaterialInstance;
// Shared shader definition and pipeline state. Device must outlive all owners.
struct Material : std::enable_shared_from_this<Material>
{
  ~Material();
  Material(const Material&) = delete;
  Material& operator=(const Material&) = delete;
  std::shared_ptr<const MaterialInstance>
  Instantiate(const MaterialLoadOptions& options = {}) const;

  SurfaceModel model = SurfaceModel::Unlit;
  std::shared_ptr<const ShadingModel> shadingModel;
  std::filesystem::path path;
  std::string source;
  Lighthouse::GraphicsPipelineHandle gBufferPipeline{};
  std::optional<Lighthouse::RayTracingPipelineHandle> rayTracingPipeline;
  Lighthouse::DescriptorSetLayoutHandle frameLayout{};
  Lighthouse::PipelineLayoutHandle graphicsLayout{};
  Lighthouse::DescriptorSetLayoutHandle parametersLayout{};
  std::vector<MaterialParameter> parameters;

private:
  friend class Materials;
  friend struct MaterialInstance;
  explicit Material(Lighthouse::IRenderDevice& device) : m_device(device) {}
  Lighthouse::IRenderDevice& m_device;
  std::string m_declarations;
  std::filesystem::path m_shader;
  Lighthouse::PipelineLayoutHandle m_rayLayout{};
  Lighthouse::ShaderModuleHandle m_vertex{}, m_fragment{};
};
struct MaterialInstance
{
  ~MaterialInstance();
  MaterialInstance(const MaterialInstance&) = delete;
  MaterialInstance& operator=(const MaterialInstance&) = delete;
  std::shared_ptr<const Material> material;
  Lighthouse::DescriptorSetHandle parametersSet{};
  Lighthouse::BufferHandle parametersBuffer{};
  // Borrowed; callers keep textures alive through GPU completion.
  std::vector<Lighthouse::DescriptorImageInfo> textures;

private:
  friend struct Material;
  explicit MaterialInstance(std::shared_ptr<const Material> owner)
      : material(std::move(owner))
  {
  }
};

class Materials
{
public:
  explicit Materials(Lighthouse::IRenderDevice& device);
  Materials(Lighthouse::IRenderDevice& device,
            std::filesystem::path resourceRoot);
  std::shared_ptr<const Material> Load(const std::filesystem::path& path);

private:
  std::shared_ptr<const ShadingModel> LoadShadingModel(SurfaceModel model);
  std::map<SurfaceModel, std::shared_ptr<const ShadingModel>> m_models;
  Lighthouse::IRenderDevice& m_device;
  std::filesystem::path m_resourceRoot;
  std::unordered_map<std::string, std::shared_ptr<const Material>> m_loaded;
};
} // namespace EESG
