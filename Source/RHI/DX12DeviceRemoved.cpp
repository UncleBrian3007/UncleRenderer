#include "DX12DeviceRemoved.h"

#include "../Core/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    constexpr uint32 GDredLogNodeLimit = 200;

    std::string NarrowFromWide(const wchar_t* Text)
    {
        if (!Text || Text[0] == L'\0')
        {
            return "";
        }

        const int WideLength = static_cast<int>(wcslen(Text));
        const int RequiredBytes = WideCharToMultiByte(CP_UTF8, 0, Text, WideLength, nullptr, 0, nullptr, nullptr);
        if (RequiredBytes <= 0)
        {
            return "";
        }

        std::string Result(static_cast<size_t>(RequiredBytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, Text, WideLength, Result.data(), RequiredBytes, nullptr, nullptr);
        return Result;
    }

    std::string NarrowFromAnsi(const char* Text)
    {
        if (!Text)
        {
            return "";
        }

        return Text;
    }

    std::string ToUtf8(const wchar_t* WideText, const char* AnsiText)
    {
        std::string Wide = NarrowFromWide(WideText);
        if (!Wide.empty())
        {
            return Wide;
        }

        return NarrowFromAnsi(AnsiText);
    }

    std::string HrToHex(HRESULT Hr)
    {
        std::ostringstream Oss;
        Oss << "0x" << std::hex << std::uppercase << static_cast<uint32>(Hr);
        return Oss.str();
    }

    const char* DredAllocationTypeToString(D3D12_DRED_ALLOCATION_TYPE Type)
    {
        switch (Type)
        {
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "COMMAND_QUEUE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "COMMAND_ALLOCATOR";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PIPELINE_STATE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "COMMAND_LIST";
        case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "FENCE";
        case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DESCRIPTOR_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QUERY_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "COMMAND_SIGNATURE";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "PIPELINE_LIBRARY";
        case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "RESOURCE";
        case D3D12_DRED_ALLOCATION_TYPE_PASS: return "PASS";
        case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "STATE_OBJECT";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_ENCODER: return "VIDEO_ENCODER";
        default: return "UNKNOWN";
        }
    }

    std::string AutoBreadcrumbOpToString(D3D12_AUTO_BREADCRUMB_OP Op)
    {
        switch (Op)
        {
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DRAWINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DRAWINDEXEDINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "DISPATCH";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "EXECUTEINDIRECT";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "RESOURCEBARRIER";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "COPYRESOURCE";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "COPYBUFFERREGION";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "COPYTEXTUREREGION";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "CLEARRENDERTARGETVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "CLEARDEPTHSTENCILVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "RESOLVEQUERYDATA";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "PRESENT";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BUILD_RAYTRACING_AS";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DISPATCHRAYS";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DISPATCHMESH";
        case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "BARRIER";
        default: return "OTHER";
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress(const IUnknown* Unknown)
    {
        if (!Unknown)
        {
            return 0;
        }

        ComPtr<ID3D12Resource> Resource;
        if (SUCCEEDED(const_cast<IUnknown*>(Unknown)->QueryInterface(IID_PPV_ARGS(Resource.GetAddressOf()))))
        {
            return Resource->GetGPUVirtualAddress();
        }

        return 0;
    }

    uint64 GetResourceSizeInBytes(const IUnknown* Unknown)
    {
        if (!Unknown)
        {
            return 0;
        }

        ComPtr<ID3D12Resource> Resource;
        if (FAILED(const_cast<IUnknown*>(Unknown)->QueryInterface(IID_PPV_ARGS(Resource.GetAddressOf()))))
        {
            return 0;
        }

        D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
        if (Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            return static_cast<uint64>(Desc.Width);
        }

        return 0;
    }

    uint32 FindContextIndex(const D3D12_DRED_BREADCRUMB_CONTEXT* Contexts, uint32 Count, uint32 BreadcrumbIndex)
    {
        if (!Contexts || Count == 0)
        {
            return Count;
        }

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            if (Contexts[Index].BreadcrumbIndex >= BreadcrumbIndex)
            {
                return Index;
            }
        }

        return Count;
    }

    ID3D12Device* RecoverDeviceFromObject(IUnknown* RelatedObject, ComPtr<ID3D12Device>& OutRecovered)
    {
        if (!RelatedObject)
        {
            return nullptr;
        }

        ComPtr<ID3D12Device> AsDevice;
        if (SUCCEEDED(RelatedObject->QueryInterface(IID_PPV_ARGS(AsDevice.GetAddressOf()))))
        {
            OutRecovered = AsDevice;
            LogWarning("Recovered ID3D12Device from RelatedObject directly");
            return OutRecovered.Get();
        }

        ComPtr<ID3D12Resource> Resource;
        if (SUCCEEDED(RelatedObject->QueryInterface(IID_PPV_ARGS(Resource.GetAddressOf()))))
        {
            if (SUCCEEDED(Resource->GetDevice(IID_PPV_ARGS(OutRecovered.ReleaseAndGetAddressOf()))))
            {
                LogWarning("Recovered ID3D12Device from RelatedObject as ID3D12Resource");
                return OutRecovered.Get();
            }
        }

        ComPtr<ID3D12DeviceChild> DeviceChild;
        if (SUCCEEDED(RelatedObject->QueryInterface(IID_PPV_ARGS(DeviceChild.GetAddressOf()))))
        {
            if (SUCCEEDED(DeviceChild->GetDevice(IID_PPV_ARGS(OutRecovered.ReleaseAndGetAddressOf()))))
            {
                LogWarning("Recovered ID3D12Device from RelatedObject as ID3D12DeviceChild");
                return OutRecovered.Get();
            }
        }

        return nullptr;
    }
}

std::atomic<bool> GDeviceRemoved{ false };
std::atomic<bool> GDredDumped{ false };

bool IsDeviceRemovedHr(HRESULT Hr)
{
    return Hr == DXGI_ERROR_DEVICE_REMOVED
        || Hr == DXGI_ERROR_DEVICE_HUNG
        || Hr == DXGI_ERROR_DEVICE_RESET
        || Hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR
        || Hr == DXGI_ERROR_INVALID_CALL;
}

void ConfigureDredSettingsBeforeDeviceCreation()
{
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings2> DredSettings2;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(DredSettings2.GetAddressOf()))))
    {
        DredSettings2->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        DredSettings2->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        DredSettings2->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        DredSettings2->SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        LogInfo("DRED settings enabled (AutoBreadcrumbs + PageFault + BreadcrumbContext + Watson)");
        return;
    }

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> DredSettings1;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(DredSettings1.GetAddressOf()))))
    {
        DredSettings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        DredSettings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        DredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        DredSettings1->SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        LogInfo("DRED settings enabled (AutoBreadcrumbs + PageFault + BreadcrumbContext + Watson)");
        return;
    }

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> DredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(DredSettings.GetAddressOf()))))
    {
        DredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        DredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        LogInfo("DRED settings enabled (AutoBreadcrumbs + PageFault)");
        return;
    }

    LogWarning("DRED settings unavailable");
}

void HandleDeviceRemoved(ID3D12Device* Device, HRESULT Hr, const wchar_t* Where)
{
    std::ostringstream Oss;
    Oss << "Device removed path entered at " << ToUtf8(Where, nullptr) << ", hr=" << HrToHex(Hr);
    LogError(Oss.str());

    bool Expected = false;
    if (!GDredDumped.compare_exchange_strong(Expected, true))
    {
        return;
    }

    DumpDRED(Device);

    if (Device)
    {
        std::atomic<bool> Done{ false };
        HRESULT RemovedReason = S_OK;
        std::thread ReasonThread([&]()
        {
            RemovedReason = Device->GetDeviceRemovedReason();
            Done.store(true);
        });

        constexpr int32 MaxPollCount = 20;
        for (int32 Poll = 0; Poll < MaxPollCount && !Done.load(); ++Poll)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (Done.load())
        {
            if (ReasonThread.joinable())
            {
                ReasonThread.join();
            }
            LogError(std::string("GetDeviceRemovedReason = ") + HrToHex(RemovedReason));
        }
        else
        {
            if (ReasonThread.joinable())
            {
                ReasonThread.detach();
            }
            LogWarning("GetDeviceRemovedReason timed out (2s)");
        }
    }
}

void ReportDxFailure(ID3D12Device* Device, HRESULT Hr, const wchar_t* Where, IUnknown* RelatedObject)
{
    if (!FAILED(Hr))
    {
        return;
    }

    ComPtr<ID3D12Device> RecoveredDevice;
    ID3D12Device* EffectiveDevice = Device;
    if (!EffectiveDevice && RelatedObject)
    {
        EffectiveDevice = RecoverDeviceFromObject(RelatedObject, RecoveredDevice);
    }

    if (IsDeviceRemovedHr(Hr))
    {
        GDeviceRemoved.store(true);
        if (!EffectiveDevice)
        {
            LogWarning("DumpDRED skipped: device is null (even after recovery)");
            return;
        }

        HandleDeviceRemoved(EffectiveDevice, Hr, Where);
    }
    else
    {
        std::ostringstream Oss;
        Oss << "DX call failed at " << ToUtf8(Where, nullptr) << ", hr=" << HrToHex(Hr);
        LogError(Oss.str());
    }

#if defined(_DEBUG)
    __debugbreak();
#endif
}

void DumpDRED(ID3D12Device* Device)
{
    if (!Device)
    {
        LogWarning("DumpDRED skipped: device is null");
        return;
    }

    ComPtr<ID3D12DeviceRemovedExtendedData1> Dred1;
    if (SUCCEEDED(Device->QueryInterface(IID_PPV_ARGS(Dred1.GetAddressOf()))))
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 Breadcrumbs = {};
        if (SUCCEEDED(Dred1->GetAutoBreadcrumbsOutput1(&Breadcrumbs)))
        {
            const D3D12_AUTO_BREADCRUMB_NODE1* Node = Breadcrumbs.pHeadAutoBreadcrumbNode;
            uint32 NodeIndex = 0;
            for (; Node && NodeIndex < GDredLogNodeLimit; Node = Node->pNext, ++NodeIndex)
            {
                const std::string QueueName = ToUtf8(Node->pCommandQueueDebugNameW, Node->pCommandQueueDebugNameA);
                const std::string CommandListName = ToUtf8(Node->pCommandListDebugNameW, Node->pCommandListDebugNameA);
                const uint32 LastValue = Node->pLastBreadcrumbValue ? *Node->pLastBreadcrumbValue : 0;

                std::ostringstream NodeOss;
                NodeOss << "DRED AutoBreadcrumb Node[" << NodeIndex << "]: Queue=\""
                    << (QueueName.empty() ? "<unnamed>" : QueueName)
                    << "\", CommandList=\""
                    << (CommandListName.empty() ? "<unnamed>" : CommandListName)
                    << "\", BreadcrumbCount=" << Node->BreadcrumbCount
                    << ", LastBreadcrumbValue=" << LastValue;
                LogError(NodeOss.str());

                if (Node->pCommandHistory && Node->BreadcrumbCount > 0)
                {
                    const int32 Last = static_cast<int32>(LastValue);
                    const int32 Start = (std::max)(0, Last - 8);
                    const int32 End = (std::min)(static_cast<int32>(Node->BreadcrumbCount) - 1, Last + 8);
                    for (int32 HistoryIndex = Start; HistoryIndex <= End; ++HistoryIndex)
                    {
                        std::ostringstream OpOss;
                        OpOss << "  Op[" << HistoryIndex << "]=" << AutoBreadcrumbOpToString(Node->pCommandHistory[HistoryIndex]);
                        LogError(OpOss.str());
                    }
                }

                if (Node->pBreadcrumbContexts && Node->BreadcrumbContextsCount > 0)
                {
                    const uint32 ContextIdx = FindContextIndex(Node->pBreadcrumbContexts, Node->BreadcrumbContextsCount, LastValue);
                    const uint32 ContextStart = (ContextIdx > 8) ? (ContextIdx - 8) : 0;
                    const uint32 ContextEnd = (std::min)(Node->BreadcrumbContextsCount, ContextIdx + 9);
                    for (uint32 CtxIndex = ContextStart; CtxIndex < ContextEnd; ++CtxIndex)
                    {
                        const D3D12_DRED_BREADCRUMB_CONTEXT& Ctx = Node->pBreadcrumbContexts[CtxIndex];
                        std::ostringstream CtxOss;
                        CtxOss << "  Context[" << Ctx.BreadcrumbIndex << "]=" << ToUtf8(Ctx.pContextString, nullptr);
                        LogError(CtxOss.str());
                    }
                }
            }

            if (Node != nullptr)
            {
                LogWarning("DRED AutoBreadcrumb nodes truncated by log limit");
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT1 PageFault = {};
        if (SUCCEEDED(Dred1->GetPageFaultAllocationOutput1(&PageFault)))
        {
            std::ostringstream Header;
            Header << "DRED PageFault VA=0x" << std::hex << std::uppercase << static_cast<uint64>(PageFault.PageFaultVA);
            LogError(Header.str());

            const D3D12_DRED_ALLOCATION_NODE1* Existing = PageFault.pHeadExistingAllocationNode;
            uint32 ExistingCount = 0;
            for (; Existing && ExistingCount < GDredLogNodeLimit; Existing = Existing->pNext, ++ExistingCount)
            {
                const std::string Name = ToUtf8(Existing->ObjectNameW, Existing->ObjectNameA);
                const D3D12_GPU_VIRTUAL_ADDRESS BaseAddress = GetGpuVirtualAddress(Existing->pObject);
                const uint64 SizeInBytes = GetResourceSizeInBytes(Existing->pObject);

                std::ostringstream AllocationOss;
                AllocationOss << "  Existing[" << ExistingCount << "]: Name=\"" << (Name.empty() ? "<unnamed>" : Name)
                    << "\", Type=" << DredAllocationTypeToString(Existing->AllocationType)
                    << ", BaseAddress=0x" << std::hex << std::uppercase << static_cast<uint64>(BaseAddress)
                    << ", SizeInBytes=" << std::dec << SizeInBytes;
                LogError(AllocationOss.str());
            }
            if (Existing != nullptr)
            {
                LogWarning("DRED ExistingAllocation nodes truncated by log limit");
            }

            const D3D12_DRED_ALLOCATION_NODE1* Recent = PageFault.pHeadRecentFreedAllocationNode;
            uint32 RecentCount = 0;
            for (; Recent && RecentCount < GDredLogNodeLimit; Recent = Recent->pNext, ++RecentCount)
            {
                const std::string Name = ToUtf8(Recent->ObjectNameW, Recent->ObjectNameA);
                const D3D12_GPU_VIRTUAL_ADDRESS BaseAddress = GetGpuVirtualAddress(Recent->pObject);
                const uint64 SizeInBytes = GetResourceSizeInBytes(Recent->pObject);

                std::ostringstream AllocationOss;
                AllocationOss << "  RecentFreed[" << RecentCount << "]: Name=\"" << (Name.empty() ? "<unnamed>" : Name)
                    << "\", Type=" << DredAllocationTypeToString(Recent->AllocationType)
                    << ", BaseAddress=0x" << std::hex << std::uppercase << static_cast<uint64>(BaseAddress)
                    << ", SizeInBytes=" << std::dec << SizeInBytes;
                LogError(AllocationOss.str());
            }
            if (Recent != nullptr)
            {
                LogWarning("DRED RecentFreedAllocation nodes truncated by log limit");
            }
        }

        return;
    }

    ComPtr<ID3D12DeviceRemovedExtendedData> Dred;
    if (FAILED(Device->QueryInterface(IID_PPV_ARGS(Dred.GetAddressOf()))))
    {
        LogWarning("DRED unavailable on this device");
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT Breadcrumbs = {};
    if (SUCCEEDED(Dred->GetAutoBreadcrumbsOutput(&Breadcrumbs)))
    {
        const D3D12_AUTO_BREADCRUMB_NODE* Node = Breadcrumbs.pHeadAutoBreadcrumbNode;
        uint32 NodeIndex = 0;
        for (; Node && NodeIndex < GDredLogNodeLimit; Node = Node->pNext, ++NodeIndex)
        {
            const std::string QueueName = ToUtf8(Node->pCommandQueueDebugNameW, Node->pCommandQueueDebugNameA);
            const std::string CommandListName = ToUtf8(Node->pCommandListDebugNameW, Node->pCommandListDebugNameA);
            const uint32 LastValue = Node->pLastBreadcrumbValue ? *Node->pLastBreadcrumbValue : 0;

            std::ostringstream NodeOss;
            NodeOss << "DRED AutoBreadcrumb Node[" << NodeIndex << "]: Queue=\""
                << (QueueName.empty() ? "<unnamed>" : QueueName)
                << "\", CommandList=\""
                << (CommandListName.empty() ? "<unnamed>" : CommandListName)
                << "\", BreadcrumbCount=" << Node->BreadcrumbCount
                << ", LastBreadcrumbValue=" << LastValue;
            LogError(NodeOss.str());
        }
        if (Node != nullptr)
        {
            LogWarning("DRED AutoBreadcrumb nodes truncated by log limit");
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT PageFault = {};
    if (SUCCEEDED(Dred->GetPageFaultAllocationOutput(&PageFault)))
    {
        std::ostringstream Header;
        Header << "DRED PageFault VA=0x" << std::hex << std::uppercase << static_cast<uint64>(PageFault.PageFaultVA);
        LogError(Header.str());

        const D3D12_DRED_ALLOCATION_NODE* Existing = PageFault.pHeadExistingAllocationNode;
        uint32 ExistingCount = 0;
        for (; Existing && ExistingCount < GDredLogNodeLimit; Existing = Existing->pNext, ++ExistingCount)
        {
            const std::string Name = ToUtf8(Existing->ObjectNameW, Existing->ObjectNameA);
            std::ostringstream AllocationOss;
            AllocationOss << "  Existing[" << ExistingCount << "]: Name=\"" << (Name.empty() ? "<unnamed>" : Name)
                << "\", Type=" << DredAllocationTypeToString(Existing->AllocationType);
            LogError(AllocationOss.str());
        }

        const D3D12_DRED_ALLOCATION_NODE* Recent = PageFault.pHeadRecentFreedAllocationNode;
        uint32 RecentCount = 0;
        for (; Recent && RecentCount < GDredLogNodeLimit; Recent = Recent->pNext, ++RecentCount)
        {
            const std::string Name = ToUtf8(Recent->ObjectNameW, Recent->ObjectNameA);
            std::ostringstream AllocationOss;
            AllocationOss << "  RecentFreed[" << RecentCount << "]: Name=\"" << (Name.empty() ? "<unnamed>" : Name)
                << "\", Type=" << DredAllocationTypeToString(Recent->AllocationType);
            LogError(AllocationOss.str());
        }
    }
}
