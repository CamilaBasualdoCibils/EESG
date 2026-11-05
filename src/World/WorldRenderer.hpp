#pragma once

#include "Lighthouse/Engine/Assets/Assets.hpp"
#include "Lighthouse/Engine/Assets/Mesh.hpp"
#include "Lighthouse/Engine/Rendering/Graph/RenderGraph.hpp"
#include "Lighthouse/Engine/Rendering/RenderTechnique.hpp"
#include "Lighthouse/Engine/Rendering/RenderTypes.hpp"
#include "Lighthouse/Engine/Rendering/Renderer.hpp"
#include "Math/Types.hpp"
#include "World/World.hpp"
#include <span>
namespace EESG
{

class WorldRenderer : public Lighthouse::IRenderTechnique
{
public:
  WorldRenderer(Lighthouse::Renderer* renderer, World* world) : m_world(world)
  {

  }

  void BuildGraph(Lighthouse::TechniqueBuildContext& context) override
  {
    UVec2 worldRenderResolution = {1280, 720};
    struct TerrainCullData
    {
      Lighthouse::RenderGraphResource cullResult;
    };
    TerrainCullData terrainCullData = context.graph.AddPass<TerrainCullData>(
        "Terrain cull",
        [&](Lighthouse::RenderGraph::Builder& builder, TerrainCullData& data)
        {
          data.cullResult = builder.Create<Lighthouse::RenderGraphBuffer>(
              "cullResult",
              Lighthouse::RenderGraphBuffer::Desc{
                  .size = 1024, .usage = Lighthouse::BufferUsage::Storage});
          data.cullResult = builder.Write(data.cullResult);
        },
        [&](const TerrainCullData& data, FrameGraphPassResources& resources,
            Lighthouse::RenderGraphExecutePassContext& context) {});
    struct TerrainGbufferFillData
    {
      Lighthouse::RenderGraphResource albedo;
      Lighthouse::RenderGraphResource normal;
      Lighthouse::RenderGraphResource depth;
    };
    TerrainGbufferFillData terrainData =
        context.graph.AddPass<TerrainGbufferFillData>(
            "Terrain Gbuffer fill",
            [&](Lighthouse::RenderGraph::Builder& builder,
                TerrainGbufferFillData& data)
            {
              data.albedo = builder.Create<Lighthouse::RenderGraphImage>(
                  "albedo", Lighthouse::RenderGraphImage::Desc{
                                .width = worldRenderResolution.x,
                                .height = worldRenderResolution.y,
                                .format = Lighthouse::ImageFormat::RGB8});
              data.albedo = builder.Write(data.albedo);
              data.normal = builder.Create<Lighthouse::RenderGraphImage>(
                  "normal", Lighthouse::RenderGraphImage::Desc{
                                .width = worldRenderResolution.x,
                                .height = worldRenderResolution.y,
                                .format = Lighthouse::ImageFormat::RGB8});
              data.normal = builder.Write(data.normal);
              data.depth = builder.Create<Lighthouse::RenderGraphImage>(
                  "depth", Lighthouse::RenderGraphImage::Desc{
                               .width = worldRenderResolution.x,
                               .height = worldRenderResolution.y,
                               .format = Lighthouse::ImageFormat::Depth32});
              data.depth = builder.Write(data.depth);
              builder.Read(terrainCullData.cullResult);
            },
            [&](const TerrainGbufferFillData& data,
                FrameGraphPassResources& resources,
                Lighthouse::RenderGraphExecutePassContext& context) {});
    struct EntityCullData
    {
      Lighthouse::RenderGraphResource cullResult;
    };
    EntityCullData entityCullData = context.graph.AddPass<EntityCullData>(
        "Entity cull",
        [&](Lighthouse::RenderGraph::Builder& builder, EntityCullData& data)
        {
          data.cullResult = builder.Create<Lighthouse::RenderGraphBuffer>(
              "cullResult",
              Lighthouse::RenderGraphBuffer::Desc{
                  .size = 1024, .usage = Lighthouse::BufferUsage::Storage});
          data.cullResult = builder.Write(data.cullResult);
        },
        [&](const EntityCullData& data, FrameGraphPassResources& resources,
            Lighthouse::RenderGraphExecutePassContext& context) {});
    struct EntityGbufferFillData
    {
      Lighthouse::RenderGraphResource albedo;
      Lighthouse::RenderGraphResource normal;
      Lighthouse::RenderGraphResource depth;
    };
    EntityGbufferFillData entityGbufferFillData =
        context.graph.AddPass<EntityGbufferFillData>(
            "Entity Gbuffer fill",
            [&](Lighthouse::RenderGraph::Builder& builder,
                EntityGbufferFillData& data)
            {
              data.albedo = builder.Write(terrainData.albedo);
              data.normal = builder.Write(terrainData.normal);
              data.depth = builder.Write(terrainData.depth);
              builder.Read(entityCullData.cullResult);
              builder.Read(entityCullData.cullResult);
            },
            [](const EntityGbufferFillData& data,
               FrameGraphPassResources& resources,
               Lighthouse::RenderGraphExecutePassContext& context) {});

   
    struct LightingPassData
    {
      Lighthouse::RenderGraphResource color;
    };
    context.graph.AddPass<LightingPassData>(
        "Lighting pass",
        [&](Lighthouse::RenderGraph::Builder& builder, LightingPassData& data)
        {
          data.color = builder.Create<Lighthouse::RenderGraphImage>(
              "color", Lighthouse::RenderGraphImage::Desc{
                           .width = worldRenderResolution.x,
                           .height = worldRenderResolution.y,
                           .format = Lighthouse::ImageFormat::RGB16f});
          data.color = builder.Write(data.color);
          //builder.Read(entityGbufferFillData.albedo);
          //builder.Read(entityGbufferFillData.normal);
          //builder.Read(entityGbufferFillData.depth);
        },
        [](const LightingPassData& data, FrameGraphPassResources& resources,
           Lighthouse::RenderGraphExecutePassContext& context) {});

    // m_world->Registry().all_of<const >(const entity_type entt)
  }

  void Render();

private:
  World* m_world;

};

} // namespace EESG