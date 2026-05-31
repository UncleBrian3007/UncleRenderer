#pragma once

#include <memory>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "Object.h"
#include "../Scene/GltfLoader.h"

struct FDrawSectionView
{
    uint32_t DrawSectionIndex = UINT32_MAX;
    uint32_t ObjectSectionIndex = UINT32_MAX;
    FObject* Object = nullptr;
    FMeshSection* Section = nullptr;
};

class FWorldSectionList
{
public:
    using reference = FMeshSection&;

    explicit FWorldSectionList(const std::vector<FDrawSectionView>& InViews)
        : Views(&InViews)
    {
    }

    class iterator
    {
    public:
        using value_type = FMeshSection;
        using difference_type = std::ptrdiff_t;
        using pointer = FMeshSection*;
        using reference = FMeshSection&;
        using iterator_category = std::forward_iterator_tag;

        explicit iterator(std::vector<FDrawSectionView>::const_iterator InIt) : It(InIt) {}
        reference operator*() const { return *It->Section; }
        pointer operator->() const { return It->Section; }
        iterator& operator++() { ++It; return *this; }
        bool operator!=(const iterator& Other) const { return It != Other.It; }

    private:
        std::vector<FDrawSectionView>::const_iterator It;
    };

    size_t size() const { return Views ? Views->size() : 0u; }
    bool empty() const { return size() == 0u; }
    reference operator[](size_t Index) const { return *(*Views)[Index].Section; }
    reference front() const { return *Views->front().Section; }
    const FDrawSectionView& GetView(size_t Index) const { return (*Views)[Index]; }
    iterator begin() const { return iterator(Views ? Views->begin() : EmptyViews().begin()); }
    iterator end() const { return iterator(Views ? Views->end() : EmptyViews().end()); }

private:
    static const std::vector<FDrawSectionView>& EmptyViews()
    {
        static const std::vector<FDrawSectionView> Empty;
        return Empty;
    }

    const std::vector<FDrawSectionView>* Views = nullptr;
};

// Owns the logical scene objects (StaticMesh / SkeletalMesh) produced by the glTF loader.
class FWorld
{
public:
    // Takes ownership of an object and returns a non-owning pointer to it.
    FObject* AddObject(std::unique_ptr<FObject> Object)
    {
        FObject* Ptr = Object.get();
        Objects.push_back(std::move(Object));
        MarkDrawSectionViewsDirty();
        return Ptr;
    }

    const std::vector<std::unique_ptr<FObject>>& GetObjects() const { return Objects; }
    std::vector<std::unique_ptr<FObject>>& GetObjects() { return Objects; }

    const std::vector<FDrawSectionView>& GetDrawSectionViews() const
    {
        EnsureDrawSectionViews();
        return DrawSectionViews;
    }

    FWorldSectionList BuildSectionList() const
    {
        return FWorldSectionList(GetDrawSectionViews());
    }

    size_t GetDrawSectionCount() const
    {
        return GetDrawSectionViews().size();
    }

    void Tick(float DeltaTime);

    bool WasAnySkinningUpdatedLastTick() const { return bAnySkinningUpdatedLastTick; }

    std::vector<FGltfAnimationRuntime>& GetGltfAnimationRuntimes() { return GltfAnimationRuntimes; }
    const std::vector<FGltfAnimationRuntime>& GetGltfAnimationRuntimes() const { return GltfAnimationRuntimes; }

    size_t GetObjectCount() const { return Objects.size(); }

    void Clear()
    {
        Objects.clear();
        GltfAnimationRuntimes.clear();
        bAnySkinningUpdatedLastTick = false;
        MarkDrawSectionViewsDirty();
    }

private:
    void MarkDrawSectionViewsDirty() const { bDrawSectionViewsDirty = true; }
    void EnsureDrawSectionViews() const;

    std::vector<std::unique_ptr<FObject>> Objects;
    std::vector<FGltfAnimationRuntime> GltfAnimationRuntimes;
    bool bAnySkinningUpdatedLastTick = false;
    mutable bool bDrawSectionViewsDirty = true;
    mutable std::vector<FDrawSectionView> DrawSectionViews;
};
