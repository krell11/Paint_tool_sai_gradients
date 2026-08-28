#define _CRT_SECURE_NO_WARNINGS
#include "image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void pngWriteFn(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

namespace sgm {

bool loadImageFromMemory(const void* data, int size, Image& out, std::string* err) {
    if (!data || size <= 0) {
        if (err) {
            *err = "Empty image data";
        }
        return false;
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        static_cast<const stbi_uc*>(data), size, &w, &h, &channels, 4);
    if (!pixels) {
        if (err) {
            *err = stbi_failure_reason() ? stbi_failure_reason() : "Failed to decode image";
        }
        return false;
    }

    out.reset(w, h);
    std::memcpy(out.rgba.data(), pixels, out.rgba.size());
    stbi_image_free(pixels);
    return true;
}

bool loadImageFromFile(const std::wstring& path, Image& out, std::string* err) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        if (err) {
            *err = "Failed to open file";
        }
        return false;
    }

    struct FileCloser {
        void operator()(FILE* file) const {
            if (file) {
                std::fclose(file);
            }
        }
    };
    std::unique_ptr<FILE, FileCloser> closer(f);
    if (std::fseek(f, 0, SEEK_END) != 0) {
        if (err) {
            *err = "Failed to read file";
        }
        return false;
    }
    const long sz = std::ftell(f);
    if (sz <= 0) {
        if (err) {
            *err = "File is empty";
        }
        return false;
    }
    std::fseek(f, 0, SEEK_SET);

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    const std::size_t read = std::fread(buf.data(), 1, buf.size(), f);
    if (read != buf.size()) {
        if (err) {
            *err = "Failed to read file";
        }
        return false;
    }
    closer.reset();
    return loadImageFromMemory(buf.data(), static_cast<int>(buf.size()), out, err);
}

bool encodePng(const Image& img, std::vector<std::uint8_t>& outPng, std::string* err) {
    outPng.clear();
    if (img.empty()) {
        if (err) {
            *err = "No image to save";
        }
        return false;
    }

    const int ok = stbi_write_png_to_func(
        pngWriteFn, &outPng, img.width, img.height, 4, img.rgba.data(), img.width * 4);
    if (!ok || outPng.empty()) {
        if (err) {
            *err = "Failed to encode PNG";
        }
        return false;
    }
    return true;
}

bool savePngToFile(const std::wstring& path, const Image& img, std::string* err) {
    std::vector<std::uint8_t> png;
    if (!encodePng(img, png, err)) {
        return false;
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        if (err) {
            *err = "Failed to create file";
        }
        return false;
    }
    const std::size_t written = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    if (written != png.size()) {
        if (err) {
            *err = "Failed to write PNG";
        }
        return false;
    }
    return true;
}

Image downscaleToFit(const Image& src, int maxSide) {
    if (src.empty() || maxSide <= 0) {
        return {};
    }

    const int longest = std::max(src.width, src.height);
    if (longest <= maxSide) {
        return src;
    }

    const float scale = static_cast<float>(maxSide) / static_cast<float>(longest);
    const int dw = std::max(1, static_cast<int>(src.width * scale + 0.5f));
    const int dh = std::max(1, static_cast<int>(src.height * scale + 0.5f));

    Image dst;
    dst.reset(dw, dh);

    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(src.height - 1, y * src.height / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(src.width - 1, x * src.width / dw);
            const std::uint8_t* s = src.pixel(sx, sy);
            std::uint8_t* d = dst.pixel(x, y);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
    return dst;
}

}
