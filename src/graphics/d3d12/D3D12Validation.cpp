// D3D12Validation.cpp
#include "D3D12Validation.hpp"

#include <vector>

namespace r4d::d3d12
{
    namespace
    {
        void CALLBACK messageCallback(D3D12_MESSAGE_CATEGORY /*category*/,
                                      D3D12_MESSAGE_SEVERITY severity,
                                      D3D12_MESSAGE_ID /*id*/,
                                      LPCSTR description,
                                      void* context)
        {
            if (auto* sink = static_cast<ValidationSink*>(context); sink != nullptr)
            {
                sink->onMessage(severity, description);
            }
        }

        const char* dredAllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type)
        {
            switch (type)
            {
                case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:    return "CommandQueue";
                case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "CommandAllocator";
                case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:   return "PipelineState";
                case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:     return "CommandList";
                case D3D12_DRED_ALLOCATION_TYPE_FENCE:            return "Fence";
                case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:  return "DescriptorHeap";
                case D3D12_DRED_ALLOCATION_TYPE_HEAP:             return "Heap";
                case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:       return "QueryHeap";
                case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:         return "Resource";
                default:                                          return "Other";
            }
        }
    } // namespace

    bool enableDebugLayer(bool gpuValidation)
    {
        ComPtr<ID3D12Debug> debug;
        const HRESULT hr = ::D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (FAILED(hr))
        {
            R4D_WARN("Validation: debug layer unavailable ({}). Continuing WITHOUT "
                     "validation -- results are not validated.",
                     hresultToString(hr));
            return false;
        }

        debug->EnableDebugLayer();
        R4D_INFO("Validation: D3D12 debug layer enabled");

        if (gpuValidation)
        {
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(TRUE);
                debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
                R4D_INFO("Validation: GPU-based validation enabled (expect a large "
                         "performance cost; do not benchmark in this mode)");
            }
            else
            {
                R4D_WARN("Validation: GPU-based validation requested but ID3D12Debug1 "
                         "is unavailable");
            }
        }

        return true;
    }

    void enableDeviceRemovedExtendedData()
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
        if (FAILED(::D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
        {
            R4D_DEBUG("Validation: DRED settings interface unavailable");
            return;
        }
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        R4D_INFO("Validation: DRED auto-breadcrumbs and page-fault reporting enabled");
    }

    ValidationSink::~ValidationSink()
    {
        detach();
    }

    void ValidationSink::attach(ID3D12Device* device)
    {
        if (device == nullptr)
        {
            return;
        }

        // ID3D12InfoQueue1 only exists when the debug layer is active.
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&m_infoQueue))))
        {
            R4D_DEBUG("Validation: ID3D12InfoQueue1 unavailable; debug-layer messages "
                      "will not be captured or counted");
            m_infoQueue.Reset();
            return;
        }

        const HRESULT hr = m_infoQueue->RegisterMessageCallback(
            &messageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &m_cookie);
        if (FAILED(hr))
        {
            R4D_WARN("Validation: RegisterMessageCallback failed ({})", hresultToString(hr));
            m_cookie = 0;
            m_infoQueue.Reset();
            return;
        }

        R4D_INFO("Validation: debug-layer messages are being captured and counted");
    }

    void ValidationSink::detach()
    {
        if (m_infoQueue && m_cookie != 0)
        {
            m_infoQueue->UnregisterMessageCallback(m_cookie);
            m_cookie = 0;
        }
        m_infoQueue.Reset();
    }

    void ValidationSink::onMessage(D3D12_MESSAGE_SEVERITY severity, const char* description)
    {
        const char* text = (description != nullptr) ? description : "(no description)";

        switch (severity)
        {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                m_errorCount.fetch_add(1);
                R4D_CRIT("D3D12 CORRUPTION: {}", text);
                break;
            case D3D12_MESSAGE_SEVERITY_ERROR:
                m_errorCount.fetch_add(1);
                R4D_ERROR("D3D12 ERROR: {}", text);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                m_warningCount.fetch_add(1);
                R4D_WARN("D3D12 WARNING: {}", text);
                break;
            case D3D12_MESSAGE_SEVERITY_INFO:
                R4D_DEBUG("D3D12 INFO: {}", text);
                break;
            case D3D12_MESSAGE_SEVERITY_MESSAGE:
            default:
                R4D_TRACE("D3D12: {}", text);
                break;
        }
    }

    void logDeviceRemovedDiagnostics(ID3D12Device* device, HRESULT removalReason)
    {
        R4D_CRIT("Device removed: {}", hresultToString(removalReason));

        if (device == nullptr)
        {
            return;
        }

        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        {
            R4D_ERROR("DRED: no extended data available (was it enabled before device "
                      "creation?)");
            return;
        }

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
            int nodeIndex = 0;
            while (node != nullptr && nodeIndex < 8)
            {
                const UINT32 completed =
                    (node->pLastBreadcrumbValue != nullptr) ? *node->pLastBreadcrumbValue : 0;
                R4D_ERROR("DRED breadcrumb node {}: queue='{}' list='{}' "
                          "{} of {} operations completed",
                          nodeIndex,
                          node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?",
                          node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?",
                          completed, node->BreadcrumbCount);
                node = node->pNext;
                ++nodeIndex;
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)))
        {
            R4D_ERROR("DRED page fault at GPU VA 0x{:016X}",
                      static_cast<std::uint64_t>(pageFault.PageFaultVA));
            for (const D3D12_DRED_ALLOCATION_NODE1* node = pageFault.pHeadExistingAllocationNode;
                 node != nullptr; node = node->pNext)
            {
                R4D_ERROR("DRED   existing allocation: {} ({})",
                          node->ObjectNameA ? node->ObjectNameA : "?",
                          dredAllocationTypeName(node->AllocationType));
            }
            for (const D3D12_DRED_ALLOCATION_NODE1* node = pageFault.pHeadRecentFreedAllocationNode;
                 node != nullptr; node = node->pNext)
            {
                R4D_ERROR("DRED   recently freed: {} ({})",
                          node->ObjectNameA ? node->ObjectNameA : "?",
                          dredAllocationTypeName(node->AllocationType));
            }
        }
    }

    void reportLiveObjects()
    {
        ComPtr<IDXGIDebug1> dxgiDebug;
        if (FAILED(::DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
        {
            R4D_DEBUG("Validation: IDXGIDebug1 unavailable; cannot report live objects");
            return;
        }

        // The report writes into the DXGI info queue; drain it so the output
        // reaches our log and not just an attached debugger.
        ComPtr<IDXGIInfoQueue> infoQueue;
        const bool haveQueue = SUCCEEDED(::DXGIGetDebugInterface1(0, IID_PPV_ARGS(&infoQueue)));
        const UINT64 before =
            haveQueue ? infoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL) : 0;

        dxgiDebug->ReportLiveObjects(
            DXGI_DEBUG_ALL,
            static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_SUMMARY |
                                              DXGI_DEBUG_RLO_DETAIL |
                                              DXGI_DEBUG_RLO_IGNORE_INTERNAL));

        if (!haveQueue)
        {
            R4D_INFO("Validation: live-object report emitted to the debug output");
            return;
        }

        const UINT64 after = infoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        if (after <= before)
        {
            R4D_INFO("Validation: live-object report produced no entries");
            return;
        }

        // Anything still alive here is a defect.
        R4D_WARN("Validation: {} live-object entries reported at shutdown; "
                 "something was not released",
                 after - before);

        for (UINT64 i = before; i < after; ++i)
        {
            SIZE_T length = 0;
            if (FAILED(infoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &length)) ||
                length == 0)
            {
                continue;
            }
            std::vector<std::byte> storage(length);
            auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(storage.data());
            if (FAILED(infoQueue->GetMessage(DXGI_DEBUG_ALL, i, message, &length)))
            {
                continue;
            }
            R4D_WARN("Validation: live object: {}",
                     message->pDescription ? message->pDescription : "(no description)");
        }
    }

} // namespace r4d::d3d12
