#pragma once
#include "Materials/Materials.hpp"
#include <Lighthouse/Engine/Rendering/RenderTechnique.hpp>
#include <Lighthouse/Engine/Rendering/RenderView.hpp>
#include <map>

namespace EESG
{
enum class PostProcessStage
{
  AfterGBuffer,
  AfterLighting,
  BeforeToneMapping,
  AfterToneMapping,
  BeforePresentation
};
struct GBufferResources
{
  std::map<GBufferAttribute, Lighthouse::RenderGraphResource> attributes;
  Lighthouse::RenderGraphResource depth{Lighthouse::kInvalidGraphResource};
};
struct PostProcessContext
{
  Lighthouse::IRenderDevice& device;
  const Lighthouse::IRenderView& view;
  const GBufferResources& gbuffer;
  uint32_t width;
  uint32_t height;
  float deltaTime;
};
class IPostProcess
{
public:
  virtual ~IPostProcess() = default;
  virtual PostProcessStage Stage() const noexcept = 0;
  // Called while building the graph. Capture values needed by execution passes.
  // AfterGBuffer receives an invalid color handle; use the G-buffer instead.
  virtual Lighthouse::RenderGraphResource
  BuildGraph(Lighthouse::TechniqueBuildContext& graph,
             const PostProcessContext& context,
             Lighthouse::RenderGraphResource color) = 0;
};
} // namespace EESG
