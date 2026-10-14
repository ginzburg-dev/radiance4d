// D3D12Profiler.hpp
#pragma once

#include "D3D12Common.hpp"

#include <vector>

namespace r4d::d3d12
{

    // Timestamps are resolved per frame slot and read back only after the caller
    // has waited on that slot's fence, so profiling adds no synchronisation. The
    // reported time therefore lags the CPU by the number of frames in flight.
    class D3D12Profiler
    {
    public:
        Status initialise(ID3D12Device* device, ID3D12CommandQueue* queue, UINT frameSlots);

        void beginFrame(ID3D12GraphicsCommandList7* commandList, UINT slot);

        // Records the closing timestamp and resolves; call before Close().
        void endFrame(ID3D12GraphicsCommandList7* commandList, UINT slot);

        bool readFrameTimeMs(UINT slot, double& outMilliseconds);

        // Must run before the live-object report; member ComPtrs are released too
        // late to be covered by it otherwise.
        void shutdown();

        bool available() const { return m_queryHeap != nullptr; }
        UINT64 frequency() const { return m_frequency; }

    private:
        static constexpr UINT kQueriesPerSlot = 2;

        ComPtr<ID3D12QueryHeap> m_queryHeap;
        ComPtr<ID3D12Resource> m_readback;
        UINT64 m_frequency = 0;
        UINT m_frameSlots = 0;
        std::vector<bool> m_slotResolved;
    };

} // namespace r4d::d3d12
