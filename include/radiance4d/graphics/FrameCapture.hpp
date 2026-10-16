// FrameCapture.hpp
#pragma once

#include <radiance4d/core/ImageIO.hpp>

namespace r4d
{
    // Same type as a loaded image, so a capture compares against a reference PNG
    // without conversion.
    using FrameCapture = ImageRgba8;
} // namespace r4d
