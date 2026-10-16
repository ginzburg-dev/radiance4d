// D3D12Profiler.hpp
#pragma once

#include "D3D12Common.hpp"

#include <radiance4d/graphics/GpuStats.hpp>

#include <cstdint>
#include <vector>

namespace r4d::d3d12
{

    // Timestamps are resolved per frame slot and read back only after the caller
    // has waited on that slot's fence, so profiling adds no synchronisation. The
    // reported time therefore lags the CPU by the number of frames in flight.
    //
    // Each slot holds the frame's own pair plus a pair per GpuPass. The pass
    // pairs are resolved only when every scope was bracketed, so a frame with no
    // splats reads back nothing for them rather than garbage.
    class D3D12Profiler
    {
    public:
        Status initialise(ID3D12Device* device, ID3D12CommandQueue* queue, UINT frameSlots);

        // Off leaves the frame pair alone, which is how the cost of the per-pass
        // timestamps is measured.
        void setPassScopesEnabled(bool enabled) { m_passScopesEnabled = enabled; }
        bool passScopesEnabled() const { return m_passScopesEnabled && available(); }

        void beginFrame(ID3D12GraphicsCommandList7* commandList, UINT slot);

        // Records the closing timestamp and resolves; call before Close().
        void endFrame(ID3D12GraphicsCommandList7* commandList, UINT slot);

        void beginScope(ID3D12GraphicsCommandList7* commandList, UINT slot, GpuPass pass);
        void endScope(ID3D12GraphicsCommandList7* commandList, UINT slot, GpuPass pass);

        bool readFrameTimeMs(UINT slot, double& outMilliseconds);
        bool readPassTimeMs(UINT slot, GpuPass pass, double& outMilliseconds);

        // Must run before the live-object report; member ComPtrs are released too
        // late to be covered by it otherwise.
        void shutdown();

        bool available() const { return m_queryHeap != nullptr; }
        UINT64 frequency() const { return m_frequency; }

    private:
        static constexpr UINT kFrameQueries = 2;
        static constexpr UINT kPassQueries = 2 * static_cast<UINT>(kGpuPassCount);
        static constexpr UINT kQueriesPerSlot = kFrameQueries + kPassQueries;
        static constexpr std::uint32_t kAllScopes =
            (1u << static_cast<std::uint32_t>(kGpuPassCount)) - 1u;

        static UINT passQueryIndex(UINT slot, GpuPass pass)
        {
            return slot * kQueriesPerSlot + kFrameQueries +
                   2 * static_cast<UINT>(pass);
        }

        // Reads the pair starting at firstQuery out of the readback buffer.
        bool readPairMs(UINT firstQuery, double& outMilliseconds);

        ComPtr<ID3D12QueryHeap> m_queryHeap;
        ComPtr<ID3D12Resource> m_readback;
        UINT64 m_frequency = 0;
        UINT m_frameSlots = 0;
        bool m_passScopesEnabled = true;
        std::vector<bool> m_slotResolved;
        std::vector<bool> m_slotPassesResolved;
        // One bit per bracketed GpuPass, cleared at beginFrame.
        std::vector<std::uint32_t> m_slotScopeMask;
    };

    // Brackets one pass. The destructor closes the scope, so an early return
    // cannot leave a begin unmatched.
    class D3D12PassScope
    {
    public:
        D3D12PassScope(D3D12Profiler& profiler, ID3D12GraphicsCommandList7* commandList,
                       UINT slot, GpuPass pass)
            : m_profiler(profiler), m_commandList(commandList), m_slot(slot), m_pass(pass)
        {
            m_profiler.beginScope(m_commandList, m_slot, m_pass);
        }

        ~D3D12PassScope() { m_profiler.endScope(m_commandList, m_slot, m_pass); }

        D3D12PassScope(const D3D12PassScope&) = delete;
        D3D12PassScope& operator=(const D3D12PassScope&) = delete;

    private:
        D3D12Profiler& m_profiler;
        ID3D12GraphicsCommandList7* m_commandList;
        UINT m_slot;
        GpuPass m_pass;
    };

} // namespace r4d::d3d12
