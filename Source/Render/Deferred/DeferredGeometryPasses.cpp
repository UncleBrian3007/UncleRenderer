#include "DeferredGeometryPasses.h"

#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"

#include <cmath>
#include <cstring>

void FDeferredGeometryPasses::AddShadowPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FShadowPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Context.Graph.AddPass<FShadowPassData>("ShadowMap", [&Context](FShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bRenderShadows;
        Data.Camera = &Context.Camera;
        Data.LightViewProjection = Context.FrameState.LightViewProjection;

        if (Context.FrameState.bRenderShadows)
        {
            Builder.WriteTexture(Context.Resources.ShadowHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }, [&Owner](const FShadowPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ShadowEvent(LocalCommandList, L"ShadowMap");
        Cmd.ClearDepth(Owner.ShadowDSVHandle, 1.0f);

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12PipelineState* CurrentShadowPipeline = nullptr;
        const auto SetShadowPipeline = [&](bool bUseSkinning, bool bDoubleSided)
        {
            ID3D12PipelineState* Pipeline = bUseSkinning ? Owner.ShadowPipelinesSkinned[bDoubleSided ? 1u : 0u].Get() : Owner.ShadowPipelines[bDoubleSided ? 1u : 0u].Get();
            if (Pipeline != CurrentShadowPipeline)
            {
                LocalCommandList->SetPipelineState(Pipeline);
                CurrentShadowPipeline = Pipeline;
            }
        };
        SetShadowPipeline(false, false);
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.ShadowViewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ShadowScissor);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &Owner.ShadowDSVHandle);

        std::vector<bool> ShadowVisibility;
        ShadowVisibility.resize(Owner.SceneModels.size(), true);
        DirectX::XMVECTOR ShadowPlanes[6] = {};
        RendererUtils::BuildFrustumPlanesFromMatrix(Data.LightViewProjection, ShadowPlanes);
        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            ShadowVisibility[ModelIndex] = RendererUtils::IsAabbInCameraFrustum(ShadowPlanes, Model.BoundsMin, Model.BoundsMax);
        }

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!ShadowVisibility.empty() && !ShadowVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
            SetShadowPipeline(bUseSkinning, Model.bDoubleSided);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t BindlessIndices[] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
        }

    });
}

void FDeferredGeometryPasses::AddDepthPrepass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FDepthPrepassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FDepthPrepassData>("DepthPrepass", [&Context](FDepthPrepassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bDoDepthPrepass;
        Data.Camera = &Context.Camera;

        if (Context.FrameState.bDoDepthPrepass)
        {
            Builder.WriteTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }, [&Owner](const FDepthPrepassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent DepthEvent(LocalCommandList, L"DepthPrepass");

        Cmd.ClearDepth(Owner.GetDSVHandle());

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthHandle);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        ID3D12PipelineState* CurrentPipeline = nullptr;
        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Mask)
                || Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
            ID3D12PipelineState* DesiredPipeline = bUseSkinning ? Owner.DepthPrepassPipelinesSkinned[Model.bDoubleSided ? 1u : 0u].Get() : Owner.DepthPrepassPipelines[Model.bDoubleSided ? 1u : 0u].Get();
            if (DesiredPipeline != CurrentPipeline)
            {
                CurrentPipeline = DesiredPipeline;
                LocalCommandList->SetPipelineState(CurrentPipeline);
            }

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t BindlessIndices[] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
        }
    });
}
void FDeferredGeometryPasses::AddBasePass(FDeferredPassContext& Context, bool bClearTargets, bool bClearDepth, const char* PassName, bool bAllowSkinningFallback) const
{
    FDeferredRenderer& Owner = Context.Owner;

    const std::wstring PassLabel = PassName
        ? std::wstring(PassName, PassName + std::strlen(PassName))
        : L"GBuffer";
    struct FBasePassData
    {
        bool bDoDepthPrepass = false;
        bool bClearTargets = false;
        bool bClearDepth = false;
        bool bAllowSkinningFallback = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FBasePassData>(PassName, [&Context, bClearTargets, bClearDepth, bAllowSkinningFallback](FBasePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bDoDepthPrepass = Context.FrameState.bDoDepthPrepass;
        Data.bClearTargets = bClearTargets;
        Data.bClearDepth = bClearDepth;
        Data.bAllowSkinningFallback = bAllowSkinningFallback;
        Data.Camera = &Context.Camera;

        for (int i = 0; i < 4; ++i)
        {
            Builder.WriteTexture(Context.Resources.GBufferHandles[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        Builder.WriteTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Builder.WriteTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }, [&Owner, PassLabel](const FBasePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent GBufferEvent(LocalCommandList, PassLabel.c_str());

        D3D12_CPU_DESCRIPTOR_HANDLE BasePassRTVs[5] =
        {
            Owner.GBufferRTVHandles[0],
            Owner.GBufferRTVHandles[1],
            Owner.GBufferRTVHandles[2],
            Owner.GBufferRTVHandles[3],
            Owner.LightingRTVHandle
        };

        if (Data.bClearDepth)
        {
            Cmd.ClearDepth(Owner.GetDSVHandle());
        }

        if (Data.bClearTargets)
        {
            for (const D3D12_CPU_DESCRIPTOR_HANDLE& Handle : Owner.GBufferRTVHandles)
            {
                const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                Cmd.ClearRenderTarget(Handle, ClearValue);
            }

            const float SceneClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            Cmd.ClearRenderTarget(Owner.LightingRTVHandle, SceneClear);
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(_countof(BasePassRTVs), BasePassRTVs, FALSE, &DepthHandle);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        ID3D12Resource* IndirectBuffer = Owner.GetIndirectCommandBuffer();
        ID3D12Resource* RunCountBuffer = Owner.GetMeshletRunCountBuffer();
        if (Owner.bEnableIndirectDraw && Owner.IndirectCommandSignature && IndirectBuffer && RunCountBuffer && !Owner.IndirectDrawRanges.empty())
        {
            auto SelectPipelineByKey = [&](uint32_t Key) -> ID3D12PipelineState*
            {
                const bool bUseSkinning = (Key & (1u << 8)) != 0;
                const uint32_t MaterialKey = (Key & 0xFFu) | (((Key >> 9) & 1u) << 8);
                if (!Owner.EnsureBasePassPipelineOrFail(MaterialKey, bUseSkinning, "DeferredBasePass/Indirect"))
                {
                    return nullptr;
                }
                return bUseSkinning ? Owner.BasePassPipelinesSkinned[MaterialKey].Get() : Owner.BasePassPipelines[MaterialKey].Get();
            };

            for (size_t RangeIndex = 0; RangeIndex < Owner.IndirectDrawRanges.size(); ++RangeIndex)
            {
                const FRenderer::FIndirectDrawRange& Range = Owner.IndirectDrawRanges[RangeIndex];
                const bool bRangeSkinning = (Range.PipelineKey & (1u << 8)) != 0;
                if (bRangeSkinning && !Owner.bEnableSkinningIndirectDraw)
                {
                    continue;
                }
                ID3D12PipelineState* Pipeline = SelectPipelineByKey(Range.PipelineKey);
                if (!Pipeline)
                {
                    return;
                }
                LocalCommandList->SetPipelineState(Pipeline);
                LocalCommandList->SetGraphicsRoot32BitConstants(1, static_cast<UINT>(Range.MaterialBindlessIndices.size()), Range.MaterialBindlessIndices.data(), 0);

                const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
                const uint64_t CountOffset = RangeIndex * sizeof(uint32_t);
                if (AreModelPixEventsEnabled())
                {
                    const wchar_t* Label = Range.Name.empty() ? L"IndirectDrawRange" : Range.Name.c_str();
                    FScopedPixEvent ModelEvent(LocalCommandList, Label);
                    LocalCommandList->ExecuteIndirect(Owner.IndirectCommandSignature.Get(), Range.Count, IndirectBuffer, Offset, RunCountBuffer, CountOffset);
                }
                else
                {
                    LocalCommandList->ExecuteIndirect(Owner.IndirectCommandSignature.Get(), Range.Count, IndirectBuffer, Offset, RunCountBuffer, CountOffset);
                }
            }

            if (!Owner.bEnableSkinningIndirectDraw && Data.bAllowSkinningFallback)
            {
                for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
                {
                    if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
                    {
                        continue;
                    }

                    const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
                    if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
                    {
                        continue;
                    }

                    const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
                    if (!bUseSkinning)
                    {
                        continue;
                    }

                    const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

                    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
                    LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
                    const uint32_t BindlessIndices[] =
                    {
                        Model.BaseColorBindlessIndex,
                        Model.MetallicRoughnessBindlessIndex,
                        Model.NormalBindlessIndex,
                        Model.EmissiveBindlessIndex,
                        Model.SheenColorBindlessIndex,
                        Model.SheenRoughnessBindlessIndex,
                        Model.ClearcoatBindlessIndex,
                        Model.ClearcoatRoughnessBindlessIndex,
                        Model.ClearcoatNormalBindlessIndex,
                        Model.AnisotropyBindlessIndex
                    };
                    LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

                    const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                    const uint32_t PipelineKey = (ModelPipelineKey & 0xFFu) | (((ModelPipelineKey >> 9) & 1u) << 8);
                    if (!Owner.EnsureBasePassPipelineOrFail(PipelineKey, true, "DeferredBasePass/SkinningFallback"))
                    {
                        return;
                    }
                    LocalCommandList->SetPipelineState(Owner.BasePassPipelinesSkinned[PipelineKey].Get());

                    if (AreModelPixEventsEnabled())
                    {
                        const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                        FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                    }
                    else
                    {
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                    }
                }
            }
        }
        else
        {
            for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
            {
                if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
                {
                    continue;
                }

                const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
                if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
                {
                    continue;
                }
                const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

                const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
                LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
                const uint32_t BindlessIndices[] =
                {
                    Model.BaseColorBindlessIndex,
                    Model.MetallicRoughnessBindlessIndex,
                    Model.NormalBindlessIndex,
                    Model.EmissiveBindlessIndex,
                    Model.SheenColorBindlessIndex,
                    Model.SheenRoughnessBindlessIndex,
                    Model.ClearcoatBindlessIndex,
                    Model.ClearcoatRoughnessBindlessIndex,
                    Model.ClearcoatNormalBindlessIndex,
                    Model.AnisotropyBindlessIndex
                };
                LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

                const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                const bool bUseSkinning = (ModelPipelineKey & (1u << 8)) != 0;
                const uint32_t PipelineKey = (ModelPipelineKey & 0xFFu) | (((ModelPipelineKey >> 9) & 1u) << 8);
                if (!Owner.EnsureBasePassPipelineOrFail(PipelineKey, bUseSkinning, "DeferredBasePass/Direct"))
                {
                    return;
                }
                LocalCommandList->SetPipelineState(bUseSkinning ? Owner.BasePassPipelinesSkinned[PipelineKey].Get() : Owner.BasePassPipelines[PipelineKey].Get());

                if (AreModelPixEventsEnabled())
                {
                    const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                    FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                }
                else
                {
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                }
            }
        }

    });
}
void FDeferredGeometryPasses::AddObjectIdPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FObjectIdPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FObjectIdPassData>("ObjectId", [&Owner, &Context](FObjectIdPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.bObjectIdReadbackRequested && Owner.ObjectIdPipeline && Owner.ObjectIdTexture;
        Data.Camera = &Context.Camera;
        if (Data.bEnabled)
        {
            Builder.WriteTexture(Context.Resources.ObjectIdHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Builder.ReadTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
        }
    }, [&Owner](const FObjectIdPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ObjectIdEvent(LocalCommandList, L"ObjectIdPass");

        LocalCommandList->SetPipelineState(Owner.ObjectIdPipeline.Get());
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(1, &Owner.ObjectIdRtvHandle, FALSE, &DepthHandle);

        const UINT ClearValue[4] = { 0, 0, 0, 0 };
        LocalCommandList->ClearRenderTargetView(Owner.ObjectIdRtvHandle, reinterpret_cast<const float*>(ClearValue), 0, nullptr);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            LocalCommandList->IASetVertexBuffers(0, Model.Geometry.VertexBufferCount, Model.Geometry.VertexBufferViews.data());
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
            else
            {
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
        }

        const uint32_t Width = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t Height = static_cast<uint32_t>(Owner.Viewport.Height);
        const uint32_t ReadX = (std::min)(Owner.ObjectIdReadbackX, Width > 0 ? Width - 1 : 0);
        const uint32_t ReadY = (std::min)(Owner.ObjectIdReadbackY, Height > 0 ? Height - 1 : 0);

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = Owner.ObjectIdTexture.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        LocalCommandList->ResourceBarrier(1, &Barrier);

        D3D12_TEXTURE_COPY_LOCATION Src = {};
        Src.pResource = Owner.ObjectIdTexture.Get();
        Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst = {};
        Dst.pResource = Owner.ObjectIdReadback.Get();
        Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint = Owner.ObjectIdFootprint;

        D3D12_BOX SourceBox = {};
        SourceBox.left = ReadX;
        SourceBox.top = ReadY;
        SourceBox.front = 0;
        SourceBox.right = ReadX + 1;
        SourceBox.bottom = ReadY + 1;
        SourceBox.back = 1;

        LocalCommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &SourceBox);

        std::swap(Barrier.Transition.StateBefore, Barrier.Transition.StateAfter);
        LocalCommandList->ResourceBarrier(1, &Barrier);

        Owner.bObjectIdReadbackRecorded = true;
    });
}

void FDeferredGeometryPasses::AddVelocityPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FVelocityPassData
    {
        const FCamera* Camera = nullptr;
        bool bCameraMoved = false;
        bool bAnySkinningUpdated = false;
    };

    Context.Graph.AddPass<FVelocityPassData>("Velocity", [&Context](FVelocityPassData& Data, FRGPassBuilder& Builder)
    {
        Data.Camera = &Context.Camera;
        Data.bCameraMoved = Context.FrameState.bCameraMoved;
        Data.bAnySkinningUpdated = Context.FrameState.bAnySkinningUpdated;
        Builder.WriteTexture(Context.Resources.VelocityHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Builder.ReadTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
    }, [&Owner](const FVelocityPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Owner.VelocityRootSignature || !Owner.VelocityTexture)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent VelocityEvent(LocalCommandList, L"VelocityPass");

        const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cmd.ClearRenderTarget(Owner.VelocityRtvHandle, ClearValue);

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.VelocityRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthDsvHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(1, &Owner.VelocityRtvHandle, FALSE, &DepthDsvHandle);

        struct FVelocityPassConstants
        {
            DirectX::XMFLOAT4X4 CurrentUnjitteredViewProjection{};
            DirectX::XMFLOAT4X4 PreviousUnjitteredViewProjection{};
            uint32_t HasPreviousUnjitteredViewProjection = 0u;
        };

        FVelocityPassConstants VelocityConstants = {};
        VelocityConstants.CurrentUnjitteredViewProjection = Owner.CurrentUnjitteredViewProjectionMatrix;
        VelocityConstants.PreviousUnjitteredViewProjection = Owner.PreviousUnjitteredViewProjectionMatrix;
        VelocityConstants.HasPreviousUnjitteredViewProjection = Owner.bHasPreviousUnjitteredViewProjection ? 1u : 0u;
        LocalCommandList->SetGraphicsRoot32BitConstants(2, sizeof(FVelocityPassConstants) / sizeof(uint32_t), &VelocityConstants, 0);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        const auto IsWorldTransformChanged = [](const DirectX::XMFLOAT4X4& Current, const DirectX::XMFLOAT4X4& Previous)
        {
            const float* CurrentData = reinterpret_cast<const float*>(&Current);
            const float* PreviousData = reinterpret_cast<const float*>(&Previous);
            constexpr float MatrixEpsilon = 1e-6f;
            for (int ElementIndex = 0; ElementIndex < 16; ++ElementIndex)
            {
                if (std::abs(CurrentData[ElementIndex] - PreviousData[ElementIndex]) > MatrixEpsilon)
                {
                    return true;
                }
            }

            return false;
        };

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }

            bool bNeedsVelocity = Data.bCameraMoved;
            if (!bNeedsVelocity)
            {
                const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
                const bool bWorldMoved = Model.bHasPreviousWorldMatrix && IsWorldTransformChanged(Model.WorldMatrix, Model.PreviousWorldMatrix);
                const bool bSkinningMoved = bUseSkinning && Data.bAnySkinningUpdated && Model.bSkinningUpdatedThisFrame;
                bNeedsVelocity = bWorldMoved || bSkinningMoved;
            }

            if (!bNeedsVelocity)
            {
                continue;
            }

            const bool bUseAlphaMask = Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Mask);
            const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
            const uint32_t PipelineIndex = (bUseAlphaMask ? 1u : 0u) | (Model.bDoubleSided ? 2u : 0u);
            ID3D12PipelineState* Pipeline = bUseSkinning ? Owner.VelocityPipelinesSkinned[PipelineIndex].Get() : Owner.VelocityPipelines[PipelineIndex].Get();
            if (!Pipeline)
            {
                continue;
            }

            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetPipelineState(Pipeline);
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);

            const uint32_t BindlessIndices[] =
            {
                Model.BaseColorBindlessIndex,
                Model.MetallicRoughnessBindlessIndex,
                Model.NormalBindlessIndex,
                Model.EmissiveBindlessIndex,
                Model.SheenColorBindlessIndex,
                Model.SheenRoughnessBindlessIndex,
                Model.ClearcoatBindlessIndex,
                Model.ClearcoatRoughnessBindlessIndex,
                Model.ClearcoatNormalBindlessIndex,
                Model.AnisotropyBindlessIndex
            };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);
            LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
        }
    });
}
