// AgilitySdkExports.cpp
//
// The loader reads these from the executable module, not from a linked library,
// so this is compiled into every executable that creates a device. A wrong value
// fails silently and the OS runtime is used instead.

#if R4D_ENABLE_AGILITY_SDK

extern "C"
{
    __declspec(dllexport) extern const unsigned int D3D12SDKVersion = R4D_AGILITY_SDK_VERSION;

    // Must match where CMake stages D3D12Core.dll.
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

#endif // R4D_ENABLE_AGILITY_SDK
