#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sgm {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    bool empty() const { return width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width * height * 4); }
    int pixelCount() const { return width * height; }

    void reset(int w, int h) {
        width = w;
        height = h;
        rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0);
    }

    std::uint8_t* pixel(int x, int y) { return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4; }
    const std::uint8_t* pixel(int x, int y) const { return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4; }
};

bool loadImageFromFile(const std::wstring& path, Image& out, std::string* err = nullptr);
bool loadImageFromMemory(const void* data, int size, Image& out, std::string* err = nullptr);
bool savePngToFile(const std::wstring& path, const Image& img, std::string* err = nullptr);
bool encodePng(const Image& img, std::vector<std::uint8_t>& outPng, std::string* err = nullptr);
Image downscaleToFit(const Image& src, int maxSide);

}