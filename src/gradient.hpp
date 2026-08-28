#pragma once

#include "image.hpp"

#include <string>
#include <vector>

namespace sgm {

struct ColorStop {
    float t = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

enum class LumaMode {
    Photoshop, // 0.30 / 0.59 / 0.11
    Rec709     // 0.2126 / 0.7152 / 0.0722
};

struct Gradient {
    std::vector<ColorStop> stops;
    bool reverse = false;

    void ensureValid();
    void sortStops();
    void eval(float t, float outRgba[4]) const;
    void buildLut(std::uint8_t lut[256][4]) const;
};

struct MapSettings {
    LumaMode luma = LumaMode::Photoshop;
    bool preserveAlpha = true;
    float mix = 1.0f; // 0 = original, 1 = full map
};

struct Preset {
    const char* name;
    Gradient gradient;
};

Gradient makeDefaultGradient();
std::vector<Preset> builtinPresets();
Image applyGradientMap(const Image& src, const Gradient& gradient, const MapSettings& settings);

} // namespace sgm
