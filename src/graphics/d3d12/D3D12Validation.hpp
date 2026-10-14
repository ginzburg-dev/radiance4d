// D3D12Validation.hpp
#pragma once

#include "D3D12Common.hpp"

#include <atomic>
#include <cstdint>

namespace r4d::d3d12
{

    // Both must be called before device creation.
    bool enableDebugLayer(bool gpuValidation);
    void enableDeviceRemovedExtendedData();

    // Routes ID3D12InfoQueue1 messages into the logger and counts them, which is
    // what lets a run assert it was clean.
    class ValidationSink
    {
    public:
        ValidationSink() = default;
        ~ValidationSink();

        ValidationSink(const ValidationSink&) = delete;
        ValidationSink& operator=(const ValidationSink&) = delete;

        void attach(ID3D12Device* device);
        void detach();

        std::uint64_t errorCount() const { return m_errorCount.load(); }
        std::uint64_t warningCount() const { return m_warningCount.load(); }
        bool active() const { return m_cookie != 0; }

        void onMessage(D3D12_MESSAGE_SEVERITY severity, const char* description);

    private:
        ComPtr<ID3D12InfoQueue1> m_infoQueue;
        DWORD m_cookie = 0;
        std::atomic<std::uint64_t> m_errorCount{0};
        std::atomic<std::uint64_t> m_warningCount{0};
    };

    void logDeviceRemovedDiagnostics(ID3D12Device* device, HRESULT removalReason);

    // Call after the device and swapchain are released.
    void reportLiveObjects();

} // namespace r4d::d3d12
