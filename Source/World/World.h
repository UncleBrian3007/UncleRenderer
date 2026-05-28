#pragma once

#include <memory>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "Object.h"

struct FDrawSectionView
{
    uint32_t DrawSectionIndex = UINT32_MAX;
    uint32_t ObjectSectionIndex = UINT32_MAX;
    FObject* Object = nullptr;
    FMeshSection* Section = nullptr;
};

struct FConstDrawSectionView
{
    uint32_t DrawSectionIndex = UINT32_MAX;
    uint32_t ObjectSectionIndex = UINT32_MAX;
    const FObject* Object = nullptr;
    const FMeshSection* Section = nullptr;
};

template <typename TView, typename TSection>
class TWorldSectionList
{
public:
    using reference = TSection&;

    explicit TWorldSectionList(const std::vector<TView>& InViews)
        : Views(&InViews)
    {
    }

    class iterator
    {
    public:
        using value_type = TSection;
        using difference_type = std::ptrdiff_t;
        using pointer = TSection*;
        using reference = TSection&;
        using iterator_category = std::forward_iterator_tag;

        explicit iterator(typename std::vector<TView>::const_iterator InIt) : It(InIt) {}
        reference operator*() const { return *It->Section; }
        pointer operator->() const { return It->Section; }
        iterator& operator++() { ++It; return *this; }
        bool operator!=(const iterator& Other) const { return It != Other.It; }

    private:
        typename std::vector<TView>::const_iterator It;
    };

    size_t size() const { return Views ? Views->size() : 0u; }
    bool empty() const { return size() == 0u; }
    reference operator[](size_t Index) const { return *(*Views)[Index].Section; }
    reference front() const { return *Views->front().Section; }
    const TView& GetView(size_t Index) const { return (*Views)[Index]; }
    iterator begin() const { return iterator(Views ? Views->begin() : EmptyViews().begin()); }
    iterator end() const { return iterator(Views ? Views->end() : EmptyViews().end()); }

private:
    static const std::vector<TView>& EmptyViews()
    {
        static const std::vector<TView> Empty;
        return Empty;
    }

    const std::vector<TView>* Views = nullptr;
};

using FWorldSectionList = TWorldSectionList<FDrawSectionView, FMeshSection>;
using FConstWorldSectionList = TWorldSectionList<FConstDrawSectionView, const FMeshSection>;

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

    const std::vector<FDrawSectionView>& GetDrawSectionViews()
    {
        EnsureDrawSectionViews();
        return DrawSectionViews;
    }

    FWorldSectionList BuildSectionList()
    {
        return FWorldSectionList(GetDrawSectionViews());
    }

    const std::vector<FConstDrawSectionView>& GetDrawSectionViews() const
    {
        EnsureConstDrawSectionViews();
        return ConstDrawSectionViews;
    }

    FConstWorldSectionList BuildSectionList() const
    {
        return FConstWorldSectionList(GetDrawSectionViews());
    }

    size_t GetDrawSectionCount() const
    {
        return GetDrawSectionViews().size();
    }

    void Tick(float DeltaTime)
    {
        for (const std::unique_ptr<FObject>& Object : Objects)
        {
            if (Object)
            {
                Object->Tick(DeltaTime);
            }
        }
    }

    size_t GetObjectCount() const { return Objects.size(); }

    void Clear()
    {
        Objects.clear();
        MarkDrawSectionViewsDirty();
    }

private:
    void MarkDrawSectionViewsDirty() const
    {
        bDrawSectionViewsDirty = true;
        bConstDrawSectionViewsDirty = true;
    }

    void EnsureDrawSectionViews()
    {
        if (!bDrawSectionViewsDirty)
        {
            return;
        }

        DrawSectionViews.clear();
        uint32_t DrawSectionIndex = 0;
        for (std::unique_ptr<FObject>& Object : Objects)
        {
            if (!Object)
            {
                continue;
            }

            std::vector<FMeshSection>& Sections = Object->GetSections();
            DrawSectionViews.reserve(DrawSectionViews.size() + Sections.size());
            for (uint32_t SectionIndex = 0; SectionIndex < static_cast<uint32_t>(Sections.size()); ++SectionIndex)
            {
                FDrawSectionView View;
                View.DrawSectionIndex = DrawSectionIndex++;
                View.ObjectSectionIndex = SectionIndex;
                View.Object = Object.get();
                View.Section = &Sections[SectionIndex];
                DrawSectionViews.push_back(View);
            }
        }

        bDrawSectionViewsDirty = false;
    }

    void EnsureConstDrawSectionViews() const
    {
        if (!bConstDrawSectionViewsDirty)
        {
            return;
        }

        ConstDrawSectionViews.clear();
        uint32_t DrawSectionIndex = 0;
        for (const std::unique_ptr<FObject>& Object : Objects)
        {
            if (!Object)
            {
                continue;
            }

            const std::vector<FMeshSection>& Sections = Object->GetSections();
            ConstDrawSectionViews.reserve(ConstDrawSectionViews.size() + Sections.size());
            for (uint32_t SectionIndex = 0; SectionIndex < static_cast<uint32_t>(Sections.size()); ++SectionIndex)
            {
                FConstDrawSectionView View;
                View.DrawSectionIndex = DrawSectionIndex++;
                View.ObjectSectionIndex = SectionIndex;
                View.Object = Object.get();
                View.Section = &Sections[SectionIndex];
                ConstDrawSectionViews.push_back(View);
            }
        }

        bConstDrawSectionViewsDirty = false;
    }

    std::vector<std::unique_ptr<FObject>> Objects;
    mutable bool bDrawSectionViewsDirty = true;
    mutable bool bConstDrawSectionViewsDirty = true;
    std::vector<FDrawSectionView> DrawSectionViews;
    mutable std::vector<FConstDrawSectionView> ConstDrawSectionViews;
};
