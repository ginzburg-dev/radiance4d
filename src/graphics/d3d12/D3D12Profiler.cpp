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

        R4D_INFO("Profiler: timestamp queries ready, frequency {} ticks/s", m_frequency);
        return Status::Ok();
    }

    void D3D12Profiler::shutdown()
    {
        m_readback.Reset();
        m_queryHeap.Reset();
        m_slotResolved.clear();
        m_frameSlots = 0;
    }

    void D3D12Profiler::beginFrame(ID3D12GraphicsCommandList7* commandList, UINT slot)
    {
        if (!available() || slot >= m_frameSlots)
        {
            return;
        }
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
                                      slot * kQueriesPerSlot, kQueriesPerSlot,
                                      m_readback.Get(),
                                      static_cast<UINT64>(slot) * kQueriesPerSlot *
                                          sizeof(UINT64));
        m_slotResolved[slot] = true;
    }

    bool D3D12Profiler::readFrameTimeMs(UINT slot, double& outMilliseconds)
    {
        if (!available() || slot >= m_frameSlots || !m_slotResolved[slot])
        {
            return false;
        }

        // Map just this slot range; one call per frame.
        const std::size_t begin =
            static_cast<std::size_t>(slot) * kQueriesPerSlot * sizeof(UINT64);
        const D3D12_RANGE readRange{begin, begin + kQueriesPerSlot * sizeof(UINT64)};

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

        // Equal or inverted after a reset or counter wrap.
        if (second <= first)
        {
            return false;
        }

        outMilliseconds =
            (static_cast<double>(second - first) * 1000.0) / static_cast<double>(m_frequency);
        return true;
    }

} // namespace r4d::d3d12
