#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "World.h"

#include <cmath>
#include <vector>

#include "../Scene/GltfAnimation.h"

namespace
{
    // Returns true if at least one section in the world has skinning state
    // hooked up. Used to early-out for static-only scenes.
    bool HasAnySkinnedSection(const std::vector<FDrawSectionView>& DrawSections)
    {
        for (const FDrawSectionView& DrawSection : DrawSections)
        {
            if (DrawSection.Section->BoneMatrixBufferMapped != nullptr)
            {
                return true;
            }
        }
        return false;
    }

    void AdvanceAnimationPoses(std::vector<FGltfAnimationRuntime>& Runtimes, float DeltaTime)
    {
        for (FGltfAnimationRuntime& Runtime : Runtimes)
        {
            if (Runtime.Pose.LocalMatrices.empty())
            {
                InitializeGltfAnimationPose(Runtime, Runtime.Pose);
            }
            Runtime.AnimationTime += DeltaTime;
            UpdateGltfAnimationPose(Runtime, Runtime.AnimationTime, Runtime.Pose);
        }
    }
}

void FWorld::Tick(float DeltaTime)
{
    const std::vector<FDrawSectionView>& DrawSections = GetDrawSectionViews();
    for (const FDrawSectionView& DrawSection : DrawSections)
    {
        DrawSection.Section->bSkinningUpdatedThisFrame = false;
    }

    bAnySkinningUpdatedLastTick = false;

    FWorldTickContext Ctx;
    Ctx.AnimationRuntimes = &GltfAnimationRuntimes;
    Ctx.bAnimationTimeAdvanced = std::abs(DeltaTime) > 1e-6f;

    const bool bHasSkinning = HasAnySkinnedSection(DrawSections);
    if (bHasSkinning && !GltfAnimationRuntimes.empty())
    {
        AdvanceAnimationPoses(GltfAnimationRuntimes, DeltaTime);
    }

    for (const std::unique_ptr<FObject>& Object : Objects)
    {
        if (Object)
        {
            Object->Tick(DeltaTime, Ctx);
        }
    }

    bAnySkinningUpdatedLastTick = Ctx.bAnySkinningUpdated;
}

void FWorld::EnsureDrawSectionViews() const
{
    if (!bDrawSectionViewsDirty)
    {
        return;
    }

    DrawSectionViews.clear();
    uint32_t DrawSectionIndex = 0;
    for (const std::unique_ptr<FObject>& ObjectPtr : Objects)
    {
        if (!ObjectPtr)
        {
            continue;
        }

        std::vector<FMeshSection>& Sections = ObjectPtr->GetSections();
        DrawSectionViews.reserve(DrawSectionViews.size() + Sections.size());
        for (uint32_t SectionIndex = 0; SectionIndex < static_cast<uint32_t>(Sections.size()); ++SectionIndex)
        {
            FDrawSectionView View;
            View.DrawSectionIndex = DrawSectionIndex++;
            View.ObjectSectionIndex = SectionIndex;
            View.Object = ObjectPtr.get();
            View.Section = &Sections[SectionIndex];
            DrawSectionViews.push_back(View);
        }
    }

    bDrawSectionViewsDirty = false;
}
