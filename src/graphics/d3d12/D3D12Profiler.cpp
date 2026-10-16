// D3D12Profiler.cpp
#include "D3D12Profiler.hpp"

namespace r4d::d3d12
{

    Status D3D12Profiler::initialise(ID3D12Device* device, ID3D12CommandQueue* queue,
                                     UINT frameSlots)
    {
        m_frameSlots = frameSlots;

        // Without a frequency there is nothing to convert ticks with.
        const HRESULT freqResult = queue->GetTimestampFrequency(&m_frequency);
        if (FAILED(freqResult) || m_frequency == 0)
        {
            R4D_WARN("Profiler: GetTimestampFrequency failed ({}); GPU timings will be "
                     "reported as unavailable",
                     hresultToString(freqResult));
            m_frequency = 0;
            return Status::Ok();
        }

        D3D12_QUERY_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = frameSlots * kQueriesPerSlot;
        heapDesc.NodeMask = 0;
        R4D_HR(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap)),
               "ID3D12Device::CreateQueryHeap (timestamps)");
        m_queryHeap->SetName(L"Radiance4D Timestamp Query Heap");

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width =
            static_cast<UINT64>(frameSlots) * kQueriesPerSlot * sizeof(UINT64);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        R4D_HR(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                                               &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&m_readback)),
               "CreateCommittedResource (timestamp readback)");
        m_readback->SetName(L"Radiance4D Timestamp Readback");

        m_slotResolved.assign(frameSlots, false);
        m_slotPassesResolved.assign(frameSlots, false);
        m_slotScopeMask.assign(frameSlots, 0u);

        R4D_INFO("Profiler: timestamp queries ready, frequency {} ticks/s, {} per frame "
                 "slot ({} pass scopes)",
                 m_frequency, kQueriesPerSlot, kGpuPassCount);
        return Status::Ok();
    }

    void D3D12Profiler::shutdown()
    {
        m_readback.Reset();
        m_queryHeap.Reset();
        m_slotResolved.clear();
        m_slotPassesResolved.clear();
        m_slotScopeMask.clear();
        m_frameSlots = 0;
    }

    void D3D12Profiler::beginFrame(ID3D12GraphicsCommandList7* commandList, UINT slot)
    {
        if (!available() || slot >= m_frameSlots)
        {
            return;
        }
        m_slotScopeMask[slot] = 0u;
        commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                              slot * kQueriesPerSlot);
    }

    void D3D12Profiler::endFrame(ID3D12GraphicsCommandList7* commandList, UINT slot)
    {
        if (!available() || slot >= m_frameSlots)
        {
            return;
        }
        commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                              slot * kQueriesPerSlot + 1);
        commandList->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                      slot * kQueriesPerSlot, kFrameQueries,
                                      m_readback.Get(),
                                      static_cast<UINT64>(slot) * kQueriesPerSlot *
                                          sizeof(UINT64));
        m_slotResolved[slot] = true;

        // Resolving a query that was never written would read back whatever the
        // heap held, so the pass block goes only when every scope was bracketed.
        const bool complete = m_slotScopeMask[slot] == kAllScopes;
        if (complete)
        {
            const UINT first = slot * kQueriesPerSlot + kFrameQueries;
            commandList->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                          first, kPassQueries, m_readback.Get(),
                                          static_cast<UINT64>(first) * sizeof(UINT64));
        }
        m_slotPassesResolved[slot] = complete;
    }

    void D3D12Profiler::beginScope(ID3D12GraphicsCommandList7* commandList, UINT slot,
                                   GpuPass pass)
    {
        if (!passScopesEnabled() || slot >= m_frameSlots || pass == GpuPass::Count)
        {
            return;
        }
        commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                              passQueryIndex(slot, pass));
    }

    void D3D12Profiler::endScope(ID3D12GraphicsCommandList7* commandList, UINT slot,
                                 GpuPass pass)
    {
        if (!passScopesEnabled() || slot >= m_frameSlots || pass == GpuPass::Count)
        {
            return;
        }
        commandList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                              passQueryIndex(slot, pass) + 1);
        m_slotScopeMask[slot] |= 1u << static_cast<std::uint32_t>(pass);
    }

    bool D3D12Profiler::readPairMs(UINT firstQuery, double& outMilliseconds)
    {
        // Map just this pair; one call per read.
        const std::size_t begin = static_cast<std::size_t>(firstQuery) * sizeof(UINT64);
        const D3D12_RANGE readRange{begin, begin + 2 * sizeof(UINT64)};

        void* mapped = nullptr;
        if (FAILED(m_readback->Map(0, &readRange, &mapped)) || mapped == nullptr)
        {
            return false;
        }

        const auto* timestamps = reinterpret_cast<const UINT64*>(
            static_cast<const std::byte*>(mapped) + begin);
        const UINT64 first = timestamps[0];
        const UINT64 second = timestamps[1];

        const D3D12_RANGE writtenNothing{0, 0};
        m_readback->Unmap(0, &writtenNothing);

        // Inverted after a reset or counter wrap. Equal is left to the caller: a
        // cheap pass can genuinely land inside one tick, a whole frame cannot.
        if (second < first)
        {
            return false;
        }

        outMilliseconds =
            (static_cast<double>(second - first) * 1000.0) / static_cast<double>(m_frequency);
        return true;
    }

    bool D3D12Profiler::readFrameTimeMs(UINT slot, double& outMilliseconds)
    {
        if (!available() || slot >= m_frameSlots || !m_slotResolved[slot])
        {
            return false;
        }
        return readPairMs(slot * kQueriesPerSlot, outMilliseconds) &&
               outMilliseconds > 0.0;
    }

    bool D3D12Profiler::readPassTimeMs(UINT slot, GpuPass pass, double& outMilliseconds)
    {
        if (!available() || slot >= m_frameSlots || pass == GpuPass::Count ||
            !m_slotPassesResolved[slot])
        {
            return false;
        }
        return readPairMs(passQueryIndex(slot, pass), outMilliseconds);
    }

} // namespace r4d::d3d12
