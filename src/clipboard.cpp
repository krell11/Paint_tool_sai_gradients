#include "clipboard.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace sgm {
namespace {

UINT pngClipboardFormat() {
    static const UINT fmt = RegisterClipboardFormatW(L"PNG");
    return fmt;
}

void treatZeroAlphaAsOpaque(Image& img) {
    if (img.empty()) {
        return;
    }
    std::uint8_t maxA = 0;
    for (int i = 3; i < static_cast<int>(img.rgba.size()); i += 4) {
        maxA = static_cast<std::uint8_t>((std::max)(maxA, img.rgba[static_cast<std::size_t>(i)]));
    }
    if (maxA == 0) {
        for (int i = 3; i < static_cast<int>(img.rgba.size()); i += 4) {
            img.rgba[static_cast<std::size_t>(i)] = 255;
        }
    }
}

bool decodeDib(const void* data, std::size_t size, Image& out, std::string* err) {
    if (!data || size < sizeof(BITMAPINFOHEADER)) {
        if (err) {
            *err = "DIB is too short";
        }
        return false;
    }

    const auto* hdr = static_cast<const BITMAPINFOHEADER*>(data);
    const int headerSize = hdr->biSize;
    if (headerSize < static_cast<int>(sizeof(BITMAPINFOHEADER)) ||
        size < static_cast<std::size_t>(headerSize)) {
        if (err) {
            *err = "Corrupt DIB header";
        }
        return false;
    }

    const int width = hdr->biWidth;
    const int heightAbs = hdr->biHeight < 0 ? -hdr->biHeight : hdr->biHeight;
    const bool topDown = hdr->biHeight < 0;
    const int bpp = hdr->biBitCount;
    const DWORD compression = hdr->biCompression;

    if (width <= 0 || heightAbs <= 0) {
        if (err) {
            *err = "Invalid DIB size";
        }
        return false;
    }
    if (bpp != 24 && bpp != 32) {
        if (err) {
            *err = "Only 24/32-bit DIB is supported";
        }
        return false;
    }
    if (compression != BI_RGB && compression != BI_BITFIELDS) {
        if (err) {
            *err = "Unsupported DIB compression";
        }
        return false;
    }

    std::size_t pixelOffset = static_cast<std::size_t>(headerSize);
    if (compression == BI_BITFIELDS) {
        pixelOffset += 12;
        if (headerSize >= static_cast<int>(sizeof(BITMAPV5HEADER))) {
            pixelOffset = static_cast<std::size_t>(headerSize);
        }
    }
    if (hdr->biClrUsed > 0) {
        pixelOffset += static_cast<std::size_t>(hdr->biClrUsed) * 4u;
    }

    if (size < pixelOffset) {
        if (err) {
            *err = "DIB has no pixels";
        }
        return false;
    }

    const int srcStride = ((width * bpp + 31) / 32) * 4;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto* pixels = bytes + pixelOffset;
    const std::size_t needed = pixelOffset + static_cast<std::size_t>(srcStride) * heightAbs;
    if (size < needed) {
        if (err) {
            *err = "Truncated DIB pixels";
        }
        return false;
    }

    out.reset(width, heightAbs);
    for (int y = 0; y < heightAbs; ++y) {
        const int srcY = topDown ? y : (heightAbs - 1 - y);
        const std::uint8_t* row = pixels + static_cast<std::size_t>(srcY) * srcStride;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* d = out.pixel(x, y);
            if (bpp == 32) {
                d[2] = row[x * 4 + 0]; // B
                d[1] = row[x * 4 + 1]; // G
                d[0] = row[x * 4 + 2]; // R
                d[3] = row[x * 4 + 3]; // A
            } else {
                d[2] = row[x * 3 + 0];
                d[1] = row[x * 3 + 1];
                d[0] = row[x * 3 + 2];
                d[3] = 255;
            }
        }
    }
    treatZeroAlphaAsOpaque(out);
    return true;
}

HGLOBAL makeDibHandle(const Image& img) {
    const int width = img.width;
    const int height = img.height;
    const int stride = width * 4;
    const std::size_t pixelBytes = static_cast<std::size_t>(stride) * height;
    const std::size_t total = sizeof(BITMAPINFOHEADER) + pixelBytes;

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!mem) {
        return nullptr;
    }
    auto* bytes = static_cast<std::uint8_t*>(GlobalLock(mem));
    if (!bytes) {
        GlobalFree(mem);
        return nullptr;
    }

    auto* hdr = reinterpret_cast<BITMAPINFOHEADER*>(bytes);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->biSize = sizeof(BITMAPINFOHEADER);
    hdr->biWidth = width;
    hdr->biHeight = height;
    hdr->biPlanes = 1;
    hdr->biBitCount = 32;
    hdr->biCompression = BI_RGB;
    hdr->biSizeImage = static_cast<DWORD>(pixelBytes);

    auto* pixels = bytes + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < height; ++y) {
        const int srcY = height - 1 - y;
        std::uint8_t* row = pixels + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* s = img.pixel(x, srcY);
            row[x * 4 + 0] = s[2];
            row[x * 4 + 1] = s[1];
            row[x * 4 + 2] = s[0];
            row[x * 4 + 3] = s[3];
        }
    }

    GlobalUnlock(mem);
    return mem;
}

HGLOBAL makePngHandle(const Image& img, std::string* err) {
    std::vector<std::uint8_t> png;
    if (!encodePng(img, png, err)) {
        return nullptr;
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!mem) {
        return nullptr;
    }
    void* dest = GlobalLock(mem);
    if (!dest) {
        GlobalFree(mem);
        return nullptr;
    }
    std::memcpy(dest, png.data(), png.size());
    GlobalUnlock(mem);
    return mem;
}

class ClipboardGuard {
public:
    explicit ClipboardGuard(HWND hwnd) : ok_(OpenClipboard(hwnd) != 0) {}
    ~ClipboardGuard() {
        if (ok_) {
            CloseClipboard();
        }
    }
    bool ok() const { return ok_; }
    ClipboardGuard(const ClipboardGuard&) = delete;
    ClipboardGuard& operator=(const ClipboardGuard&) = delete;

private:
    bool ok_;
};

}

bool clipboardHasImage() {
    const UINT png = pngClipboardFormat();
    return IsClipboardFormatAvailable(png) ||
           IsClipboardFormatAvailable(CF_DIBV5) ||
           IsClipboardFormatAvailable(CF_DIB) ||
           IsClipboardFormatAvailable(CF_BITMAP);
}

bool getClipboardImage(HWND hwnd, Image& out, std::string* err) {
    ClipboardGuard clip(hwnd);
    if (!clip.ok()) {
        if (err) {
            *err = "Failed to open the clipboard";
        }
        return false;
    }

    const UINT pngFmt = pngClipboardFormat();
    if (HANDLE png = GetClipboardData(pngFmt)) {
        const SIZE_T sz = GlobalSize(png);
        const void* data = GlobalLock(png);
        if (data && sz > 0) {
            const bool ok = loadImageFromMemory(data, static_cast<int>(sz), out, err);
            GlobalUnlock(png);
            if (ok) {
                return true;
            }
        } else if (png) {
            GlobalUnlock(png);
        }
    }

    const UINT dibFormats[] = {CF_DIBV5, CF_DIB};
    for (UINT fmt : dibFormats) {
        HANDLE dib = GetClipboardData(fmt);
        if (!dib) {
            continue;
        }
        const SIZE_T sz = GlobalSize(dib);
        const void* data = GlobalLock(dib);
        bool ok = false;
        if (data && sz > 0) {
            ok = decodeDib(data, sz, out, err);
        }
        GlobalUnlock(dib);
        if (ok) {
            return true;
        }
    }

    if (err) {
        *err = "Clipboard has no image. In SAI: layer → Ctrl+C";
    }
    return false;
}

bool setClipboardImage(HWND hwnd, const Image& img, std::string* err) {
    if (img.empty()) {
        if (err) {
            *err = "No result to copy";
        }
        return false;
    }

    HGLOBAL png = makePngHandle(img, err);
    HGLOBAL dib = makeDibHandle(img);
    if (!dib) {
        if (png) {
            GlobalFree(png);
        }
        if (err) {
            *err = "Failed to build clipboard DIB";
        }
        return false;
    }

    ClipboardGuard clip(hwnd);
    if (!clip.ok()) {
        GlobalFree(dib);
        if (png) {
            GlobalFree(png);
        }
        if (err) {
            *err = "Failed to open the clipboard";
        }
        return false;
    }

    EmptyClipboard();
    if (png) {
        SetClipboardData(pngClipboardFormat(), png);
    }
    SetClipboardData(CF_DIB, dib);
    return true;
}

}
