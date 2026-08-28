#include "gradient.hpp"

#include <algorithm>
#include <cmath>

namespace sgm {
namespace {

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

std::uint8_t toU8(float v) {
    v = clampf(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

float lumaPs(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (0.30f * r + 0.59f * g + 0.11f * b) / 255.0f;
}

float luma709(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
}

ColorStop color(float t, float r, float g, float b, float a = 1.0f) {
    return {t, r, g, b, a};
}

} // namespace

void Gradient::sortStops() {
    std::sort(stops.begin(), stops.end(), [](const ColorStop& a, const ColorStop& b) {
        return a.t < b.t;
    });
}

void Gradient::ensureValid() {
    if (stops.size() < 2) {
        stops = {
            color(0.0f, 0.0f, 0.0f, 0.0f),
            color(1.0f, 1.0f, 1.0f, 1.0f),
        };
    }
    for (auto& s : stops) {
        s.t = clampf(s.t, 0.0f, 1.0f);
        s.r = clampf(s.r, 0.0f, 1.0f);
        s.g = clampf(s.g, 0.0f, 1.0f);
        s.b = clampf(s.b, 0.0f, 1.0f);
        s.a = clampf(s.a, 0.0f, 1.0f);
    }
    sortStops();
}

void Gradient::eval(float t, float outRgba[4]) const {
    t = clampf(t, 0.0f, 1.0f);
    if (reverse) {
        t = 1.0f - t;
    }
    if (stops.empty()) {
        outRgba[0] = outRgba[1] = outRgba[2] = 0.0f;
        outRgba[3] = 1.0f;
        return;
    }
    if (stops.size() == 1 || t <= stops.front().t) {
        const ColorStop& s = stops.front();
        outRgba[0] = s.r;
        outRgba[1] = s.g;
        outRgba[2] = s.b;
        outRgba[3] = s.a;
        return;
    }
    if (t >= stops.back().t) {
        const ColorStop& s = stops.back();
        outRgba[0] = s.r;
        outRgba[1] = s.g;
        outRgba[2] = s.b;
        outRgba[3] = s.a;
        return;
    }

    for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
        const ColorStop& a = stops[i];
        const ColorStop& b = stops[i + 1];
        if (t > b.t) {
            continue;
        }
        const float span = (std::max)(b.t - a.t, 1.0e-6f);
        const float u = (t - a.t) / span;
        outRgba[0] = a.r + (b.r - a.r) * u;
        outRgba[1] = a.g + (b.g - a.g) * u;
        outRgba[2] = a.b + (b.b - a.b) * u;
        outRgba[3] = a.a + (b.a - a.a) * u;
        return;
    }

    const ColorStop& s = stops.back();
    outRgba[0] = s.r;
    outRgba[1] = s.g;
    outRgba[2] = s.b;
    outRgba[3] = s.a;
}

void Gradient::buildLut(std::uint8_t lut[256][4]) const {
    for (int i = 0; i < 256; ++i) {
        float c[4];
        eval(static_cast<float>(i) / 255.0f, c);
        lut[i][0] = toU8(c[0]);
        lut[i][1] = toU8(c[1]);
        lut[i][2] = toU8(c[2]);
        lut[i][3] = toU8(c[3]);
    }
}

Gradient makeDefaultGradient() {
    Gradient g;
    g.stops = {
        color(0.00f, 0.05f, 0.07f, 0.18f),
        color(0.35f, 0.55f, 0.18f, 0.32f),
        color(0.65f, 0.95f, 0.55f, 0.28f),
        color(1.00f, 1.00f, 0.95f, 0.82f),
    };
    return g;
}

std::vector<Preset> builtinPresets() {
    Gradient bw;
    bw.stops = {color(0.0f, 0, 0, 0), color(1.0f, 1, 1, 1)};

    Gradient sepia;
    sepia.stops = {
        color(0.00f, 0.10f, 0.05f, 0.00f),
        color(0.50f, 0.62f, 0.40f, 0.20f),
        color(1.00f, 0.96f, 0.86f, 0.70f),
    };

    Gradient sunset;
    sunset.stops = {
        color(0.00f, 0.05f, 0.04f, 0.16f),
        color(0.35f, 0.55f, 0.12f, 0.32f),
        color(0.62f, 0.95f, 0.40f, 0.18f),
        color(1.00f, 1.00f, 0.85f, 0.45f),
    };

    Gradient tealOrange;
    tealOrange.stops = {
        color(0.00f, 0.02f, 0.12f, 0.18f),
        color(0.45f, 0.10f, 0.45f, 0.50f),
        color(0.72f, 0.92f, 0.48f, 0.20f),
        color(1.00f, 1.00f, 0.92f, 0.78f),
    };

    Gradient ice;
    ice.stops = {
        color(0.00f, 0.02f, 0.05f, 0.14f),
        color(0.40f, 0.10f, 0.28f, 0.48f),
        color(0.75f, 0.45f, 0.78f, 0.92f),
        color(1.00f, 0.95f, 0.98f, 1.00f),
    };

    Gradient blood;
    blood.stops = {
        color(0.00f, 0.05f, 0.01f, 0.02f),
        color(0.45f, 0.45f, 0.04f, 0.08f),
        color(0.75f, 0.82f, 0.12f, 0.10f),
        color(1.00f, 1.00f, 0.85f, 0.70f),
    };

    return {
        {"Sunset", sunset},
        {"Чёрно-белый", bw},
        {"Сепия", sepia},
        {"Teal / Orange", tealOrange},
        {"Лёд", ice},
        {"Кровь", blood},
    };
}

Image applyGradientMap(const Image& src, const Gradient& gradient, const MapSettings& settings) {
    Image dst;
    if (src.empty()) {
        return dst;
    }

    Gradient g = gradient;
    g.ensureValid();

    std::uint8_t lut[256][4];
    g.buildLut(lut);

    dst.reset(src.width, src.height);
    const float mix = clampf(settings.mix, 0.0f, 1.0f);
    const float inv = 1.0f - mix;

    for (int i = 0; i < src.pixelCount(); ++i) {
        const std::uint8_t* s = src.rgba.data() + static_cast<std::size_t>(i) * 4;
        std::uint8_t* d = dst.rgba.data() + static_cast<std::size_t>(i) * 4;

        if (s[3] == 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
        }

        const float y = (settings.luma == LumaMode::Photoshop)
                            ? lumaPs(s[0], s[1], s[2])
                            : luma709(s[0], s[1], s[2]);
        int idx = static_cast<int>(y * 255.0f + 0.5f);
        idx = idx < 0 ? 0 : (idx > 255 ? 255 : idx);

        const std::uint8_t* m = lut[idx];
        d[0] = static_cast<std::uint8_t>(m[0] * mix + s[0] * inv + 0.5f);
        d[1] = static_cast<std::uint8_t>(m[1] * mix + s[1] * inv + 0.5f);
        d[2] = static_cast<std::uint8_t>(m[2] * mix + s[2] * inv + 0.5f);
        if (settings.preserveAlpha) {
            d[3] = s[3];
        } else {
            const float a = (m[3] / 255.0f) * (s[3] / 255.0f);
            d[3] = toU8(a);
        }
    }
    return dst;
}

} // namespace sgm
