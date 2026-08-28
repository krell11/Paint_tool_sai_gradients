#pragma once

#include "image.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace sgm {

bool clipboardHasImage();
bool getClipboardImage(HWND hwnd, Image& out, std::string* err = nullptr);
bool setClipboardImage(HWND hwnd, const Image& img, std::string* err = nullptr);

} // namespace sgm
