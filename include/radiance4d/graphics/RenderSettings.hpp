// RenderSettings.hpp
#pragma once

namespace r4d
{

    // Scene-independent tone and lighting controls. Different assets need very
    // different exposure, so this is a runtime setting rather than a constant.
    struct RenderSettings
    {
        float exposure = 0.6f;
        float ambientIntensity = 0.55f;
    };

} // namespace r4d
