#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "clipboard.hpp"
#include "gradient.hpp"
#include "image.hpp"

#include <d3d11.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0;
static UINT g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static HWND g_hwnd = nullptr;
static std::wstring g_droppedFile;
static bool g_hotkeyPaste = false;

static constexpr int kHotkeyId = 1;
static constexpr int kPreviewMaxSide = 1600;

struct GpuTexture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0;
    int h = 0;

    void clear() {
        if (srv) {
            srv->Release();
            srv = nullptr;
        }
        if (tex) {
            tex->Release();
            tex = nullptr;
        }
        w = h = 0;
    }

    bool upload(const sgm::Image& img) {
        clear();
        if (img.empty() || !g_pd3dDevice) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(img.width);
        desc.Height = static_cast<UINT>(img.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = img.rgba.data();
        data.SysMemPitch = static_cast<UINT>(img.width * 4);

        if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &data, &tex))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(g_pd3dDevice->CreateShaderResourceView(tex, &srvDesc, &srv))) {
            clear();
            return false;
        }
        w = img.width;
        h = img.height;
        return true;
    }
};

struct AppState {
    sgm::Image source;
    sgm::Image previewSource;
    sgm::Image previewMapped;
    sgm::Gradient gradient = sgm::makeDefaultGradient();
    sgm::MapSettings settings;
    GpuTexture texOrig;
    GpuTexture texMapped;
    int selectedStop = 0;
    int presetIndex = 0;
    int pickingStop = -1;
    bool pickWaitRelease = false;
    bool pickLmbDown = false;
    float pickBackupR = 0.0f;
    float pickBackupG = 0.0f;
    float pickBackupB = 0.0f;
    bool dirty = true;
    bool alwaysOnTop = true;
    float wipe = 0.5f;
    int hsvStop = -1;
    float hsvH = 0.0f;
    float hsvS = 1.0f;
    float hsvV = 1.0f;
    std::string status = "Copy a layer in SAI (Ctrl+C), then Ctrl+V or Ctrl+Alt+G";
    std::vector<sgm::Preset> presets = sgm::builtinPresets();
};

struct PreviewHit {
    ImVec2 origin{};
    ImVec2 end{};
    bool valid = false;
};

static PreviewHit g_previewHit;

static AppState g_app;

static void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
        backBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &level, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &level, &g_pd3dDeviceContext);
    }
    if (hr != S_OK) {
        return false;
    }
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

static void SetAlwaysOnTop(bool enabled) {
    if (!g_hwnd) {
        return;
    }
    SetWindowPos(
        g_hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE);
}

static std::wstring OpenImageDialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Images (PNG, JPEG, BMP, TGA)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0PNG\0*.png\0All files\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        return file;
    }
    return {};
}

static std::wstring SavePngDialog() {
    wchar_t file[MAX_PATH] = L"gradient_map.png";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"PNG\0*.png\0\0";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) {
        return file;
    }
    return {};
}

static void SetSourceImage(sgm::Image img, const std::string& okStatus) {
    g_app.source = std::move(img);
    g_app.previewSource = sgm::downscaleToFit(g_app.source, kPreviewMaxSide);
    g_app.texOrig.upload(g_app.previewSource);
    g_app.dirty = true;
    g_app.status = okStatus + " (" + std::to_string(g_app.source.width) + "×" +
                   std::to_string(g_app.source.height) + ")";
}

static void LoadFromClipboard() {
    std::string err;
    sgm::Image img;
    if (!sgm::getClipboardImage(g_hwnd, img, &err)) {
        g_app.status = err;
        return;
    }
    SetSourceImage(std::move(img), "Layer from clipboard");
}

static sgm::Image MapFull() {
    g_app.gradient.ensureValid();
    return sgm::applyGradientMap(g_app.source, g_app.gradient, g_app.settings);
}

static void CopyResult() {
    if (g_app.source.empty()) {
        g_app.status = "Paste a layer from SAI first";
        return;
    }
    std::string err;
    sgm::Image mapped = MapFull();
    if (!sgm::setClipboardImage(g_hwnd, mapped, &err)) {
        g_app.status = err;
        return;
    }
    g_app.status = "Result copied — paste in SAI with Ctrl+V (use a new layer)";
}

static void SaveResult() {
    if (g_app.source.empty()) {
        g_app.status = "Paste a layer from SAI first";
        return;
    }
    const std::wstring path = SavePngDialog();
    if (path.empty()) {
        return;
    }
    std::string err;
    sgm::Image mapped = MapFull();
    if (!sgm::savePngToFile(path, mapped, &err)) {
        g_app.status = err;
        return;
    }
    g_app.status = "Saved PNG";
}

static void OpenFile(const std::wstring& path) {
    std::string err;
    sgm::Image img;
    if (!sgm::loadImageFromFile(path, img, &err)) {
        g_app.status = err;
        return;
    }
    SetSourceImage(std::move(img), "Opened file");
}

static void RebuildPreview() {
    if (g_app.previewSource.empty()) {
        g_app.previewMapped = {};
        g_app.texMapped.clear();
        g_app.dirty = false;
        return;
    }
    g_app.gradient.ensureValid();
    g_app.previewMapped = sgm::applyGradientMap(g_app.previewSource, g_app.gradient, g_app.settings);
    g_app.texMapped.upload(g_app.previewMapped);
    g_app.dirty = false;
}

static void ApplyPreset(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.presets.size())) {
        return;
    }
    g_app.presetIndex = index;
    g_app.gradient = g_app.presets[static_cast<std::size_t>(index)].gradient;
    g_app.gradient.ensureValid();
    g_app.selectedStop = 0;
    g_app.hsvStop = -1;
    g_app.dirty = true;
}

static ImU32 ColorU32(const sgm::ColorStop& s) {
    return IM_COL32(
        static_cast<int>(s.r * 255.0f + 0.5f),
        static_cast<int>(s.g * 255.0f + 0.5f),
        static_cast<int>(s.b * 255.0f + 0.5f),
        255);
}

static ImU32 ImCol32(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

static void ApplySaiStyle() {
    ImGui::StyleColorsLight();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.ChildRounding = 0.0f;
    st.FrameRounding = 0.0f;
    st.PopupRounding = 0.0f;
    st.GrabRounding = 0.0f;
    st.ScrollbarRounding = 0.0f;
    st.TabRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.PopupBorderSize = 1.0f;
    st.GrabMinSize = 10.0f;
    st.WindowPadding = ImVec2(6.0f, 6.0f);
    st.FramePadding = ImVec2(5.0f, 3.0f);
    st.ItemSpacing = ImVec2(6.0f, 5.0f);
    st.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
    st.ScrollbarSize = 14.0f;
    st.IndentSpacing = 10.0f;

    ImVec4* c = st.Colors;
    const ImVec4 bg(0.831f, 0.831f, 0.831f, 1.0f);       // #D4D4D4
    const ImVec4 panel(0.804f, 0.804f, 0.804f, 1.0f);
    const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 text(0.08f, 0.08f, 0.08f, 1.0f);
    const ImVec4 border(0.52f, 0.52f, 0.52f, 1.0f);
    const ImVec4 btn(0.90f, 0.90f, 0.90f, 1.0f);
    const ImVec4 hover(0.76f, 0.86f, 0.97f, 1.0f);
    const ImVec4 active(0.62f, 0.78f, 0.94f, 1.0f);

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(1.0f, 1.0f, 1.0f, 0.35f);
    c[ImGuiCol_FrameBg] = white;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.97f, 0.97f, 1.0f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.94f, 1.0f, 1.0f);
    c[ImGuiCol_TitleBg] = panel;
    c[ImGuiCol_TitleBgActive] = panel;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.32f, 0.32f, 0.32f, 1.0f);
    c[ImGuiCol_Button] = btn;
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = active;
    c[ImGuiCol_Header] = ImVec4(0.78f, 0.78f, 0.78f, 1.0f);
    c[ImGuiCol_HeaderHovered] = hover;
    c[ImGuiCol_HeaderActive] = active;
    c[ImGuiCol_Separator] = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TabSelected] = btn;
}

static void RgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    const float maxc = std::max(r, std::max(g, b));
    const float minc = std::min(r, std::min(g, b));
    v = maxc;
    const float d = maxc - minc;
    s = (maxc <= 1.0e-6f) ? 0.0f : d / maxc;
    if (d < 1.0e-6f) {
        h = 0.0f;
        return;
    }
    if (maxc == r) {
        h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    } else if (maxc == g) {
        h = (b - r) / d + 2.0f;
    } else {
        h = (r - g) / d + 4.0f;
    }
    h /= 6.0f;
}

static void HsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    h = std::fmod(h, 1.0f);
    if (h < 0.0f) {
        h += 1.0f;
    }
    const int i = static_cast<int>(h * 6.0f) % 6;
    const float f = h * 6.0f - std::floor(h * 6.0f);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

static void SyncHsvFromSelected(bool force) {
    if (g_app.selectedStop < 0 ||
        g_app.selectedStop >= static_cast<int>(g_app.gradient.stops.size())) {
        return;
    }
    if (!force && g_app.hsvStop == g_app.selectedStop) {
        return;
    }
    const sgm::ColorStop& s = g_app.gradient.stops[static_cast<std::size_t>(g_app.selectedStop)];
    RgbToHsv(s.r, s.g, s.b, g_app.hsvH, g_app.hsvS, g_app.hsvV);
    g_app.hsvStop = g_app.selectedStop;
}

static void ApplyHsvToSelected() {
    if (g_app.selectedStop < 0 ||
        g_app.selectedStop >= static_cast<int>(g_app.gradient.stops.size())) {
        return;
    }
    sgm::ColorStop& s = g_app.gradient.stops[static_cast<std::size_t>(g_app.selectedStop)];
    HsvToRgb(g_app.hsvH, g_app.hsvS, g_app.hsvV, s.r, s.g, s.b);
    g_app.presetIndex = -1;
    g_app.dirty = true;
}

static void SaiHeader(const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, 18.0f));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 18.0f), ImCol32(196, 196, 196));
    dl->AddTriangleFilled(
        ImVec2(p.x + 6.0f, p.y + 5.0f), ImVec2(p.x + 14.0f, p.y + 5.0f),
        ImVec2(p.x + 10.0f, p.y + 13.0f), ImCol32(168, 48, 48));
    dl->AddText(ImVec2(p.x + 18.0f, p.y + 2.0f), ImCol32(20, 20, 20), label);
}

static bool SaiSlider(const char* label, float* value, float vmin, float vmax, const char* fmt) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(72.0f);
    const float rest = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(40.0f, rest - 52.0f));
    bool changed = ImGui::SliderFloat("##sl", value, vmin, vmax, "");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(46.0f);
    changed |= ImGui::InputFloat("##num", value, 0.0f, 0.0f, fmt);
    *value = std::clamp(*value, vmin, vmax);
    ImGui::PopID();
    return changed;
}

static bool SaiToolButton(const char* id, const char* label, bool selected) {
    ImGui::PushID(id);
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.82f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.78f, 0.94f, 1.0f));
    }
    const bool pressed = ImGui::Button(label, ImVec2(124.0f, 36.0f));
    if (selected) {
        ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return pressed;
}

static bool DrawSaiColorWheel(float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 center(p.x + size * 0.5f, p.y + size * 0.5f);
    const float outer = size * 0.48f;
    const float inner = size * 0.34f;
    const float sq = inner * 1.30f;
    const ImVec2 sq0(center.x - sq * 0.5f, center.y - sq * 0.5f);
    const ImVec2 sq1(center.x + sq * 0.5f, center.y + sq * 0.5f);

    const int segs = 72;
    for (int i = 0; i < segs; ++i) {
        const float a0 = (static_cast<float>(i) / segs) * 6.2831853f;
        const float a1 = (static_cast<float>(i + 1) / segs) * 6.2831853f;
        const float h = (static_cast<float>(i) + 0.5f) / segs;
        float r, g, b;
        HsvToRgb(h, 1.0f, 1.0f, r, g, b);
        const ImU32 col = IM_COL32(
            static_cast<int>(r * 255.0f + 0.5f), static_cast<int>(g * 255.0f + 0.5f),
            static_cast<int>(b * 255.0f + 0.5f), 255);
        dl->AddQuadFilled(
            ImVec2(center.x + std::cos(a0) * inner, center.y + std::sin(a0) * inner),
            ImVec2(center.x + std::cos(a0) * outer, center.y + std::sin(a0) * outer),
            ImVec2(center.x + std::cos(a1) * outer, center.y + std::sin(a1) * outer),
            ImVec2(center.x + std::cos(a1) * inner, center.y + std::sin(a1) * inner),
            col);
    }
    dl->AddCircle(center, inner, ImCol32(70, 70, 70), 64, 1.0f);
    dl->AddCircle(center, outer, ImCol32(70, 70, 70), 64, 1.0f);

    const int grid = 24;
    float hueR, hueG, hueB;
    HsvToRgb(g_app.hsvH, 1.0f, 1.0f, hueR, hueG, hueB);
    for (int y = 0; y < grid; ++y) {
        const float v0 = 1.0f - static_cast<float>(y) / grid;
        const float v1 = 1.0f - static_cast<float>(y + 1) / grid;
        for (int x = 0; x < grid; ++x) {
            const float s0 = static_cast<float>(x) / grid;
            const float s1 = static_cast<float>(x + 1) / grid;
            auto cell = [&](float s, float v) {
                float r = (hueR * s + (1.0f - s)) * v;
                float g = (hueG * s + (1.0f - s)) * v;
                float b = (hueB * s + (1.0f - s)) * v;
                return IM_COL32(
                    static_cast<int>(r * 255.0f + 0.5f), static_cast<int>(g * 255.0f + 0.5f),
                    static_cast<int>(b * 255.0f + 0.5f), 255);
            };
            dl->AddRectFilledMultiColor(
                ImVec2(sq0.x + (sq1.x - sq0.x) * s0, sq0.y + (sq1.y - sq0.y) * (1.0f - v0)),
                ImVec2(sq0.x + (sq1.x - sq0.x) * s1, sq0.y + (sq1.y - sq0.y) * (1.0f - v1)),
                cell(s0, v0), cell(s1, v0), cell(s1, v1), cell(s0, v1));
        }
    }
    dl->AddRect(sq0, sq1, ImCol32(40, 40, 40));

    const float ha = g_app.hsvH * 6.2831853f;
    const float hm = (inner + outer) * 0.5f;
    const ImVec2 hp(center.x + std::cos(ha) * hm, center.y + std::sin(ha) * hm);
    dl->AddCircleFilled(hp, 5.0f, ImCol32(255, 255, 255));
    dl->AddCircle(hp, 5.0f, ImCol32(20, 20, 20), 16, 1.5f);

    const ImVec2 sp(
        sq0.x + (sq1.x - sq0.x) * g_app.hsvS,
        sq0.y + (sq1.y - sq0.y) * (1.0f - g_app.hsvV));
    dl->AddCircleFilled(sp, 5.0f, ImCol32(255, 255, 255));
    dl->AddCircle(sp, 5.0f, ImCol32(20, 20, 20), 16, 1.5f);

    ImGui::InvisibleButton("##sai_wheel", ImVec2(size, size));
    bool changed = false;
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float dx = m.x - center.x;
        const float dy = m.y - center.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist >= inner - 2.0f && dist <= outer + 6.0f &&
            !(m.x >= sq0.x && m.x <= sq1.x && m.y >= sq0.y && m.y <= sq1.y && dist < inner)) {
            float ang = std::atan2(dy, dx);
            if (ang < 0.0f) {
                ang += 6.2831853f;
            }
            g_app.hsvH = ang / 6.2831853f;
            changed = true;
        } else if (m.x >= sq0.x - 4.0f && m.x <= sq1.x + 4.0f && m.y >= sq0.y - 4.0f &&
                   m.y <= sq1.y + 4.0f) {
            g_app.hsvS = std::clamp((m.x - sq0.x) / (sq1.x - sq0.x), 0.0f, 1.0f);
            g_app.hsvV = std::clamp(1.0f - (m.y - sq0.y) / (sq1.y - sq0.y), 0.0f, 1.0f);
            changed = true;
        }
    }
    return changed;
}

static bool SampleImageUv(const sgm::Image& img, float u, float v, float outRgb[3]) {
    if (img.empty()) {
        return false;
    }
    const int x = std::clamp(static_cast<int>(u * static_cast<float>(img.width)), 0, img.width - 1);
    const int y = std::clamp(static_cast<int>(v * static_cast<float>(img.height)), 0, img.height - 1);
    const std::uint8_t* px = img.pixel(x, y);
    outRgb[0] = px[0] / 255.0f;
    outRgb[1] = px[1] / 255.0f;
    outRgb[2] = px[2] / 255.0f;
    return true;
}

static bool SampleScreenPixel(POINT screen, float outRgb[3]) {
    HDC dc = GetDC(nullptr);
    if (!dc) {
        return false;
    }
    const COLORREF c = GetPixel(dc, screen.x, screen.y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) {
        return false;
    }
    outRgb[0] = GetRValue(c) / 255.0f;
    outRgb[1] = GetGValue(c) / 255.0f;
    outRgb[2] = GetBValue(c) / 255.0f;
    return true;
}

static bool SampleColorAtCursor(float outRgb[3]) {
    POINT screen{};
    GetCursorPos(&screen);
    POINT client = screen;
    ScreenToClient(g_hwnd, &client);
    const ImVec2 mouse(static_cast<float>(client.x), static_cast<float>(client.y));

    if (g_previewHit.valid && mouse.x >= g_previewHit.origin.x && mouse.x < g_previewHit.end.x &&
        mouse.y >= g_previewHit.origin.y && mouse.y < g_previewHit.end.y) {
        const float w = g_previewHit.end.x - g_previewHit.origin.x;
        const float h = g_previewHit.end.y - g_previewHit.origin.y;
        if (w > 1.0f && h > 1.0f) {
            const float u = (mouse.x - g_previewHit.origin.x) / w;
            const float v = (mouse.y - g_previewHit.origin.y) / h;
            const float cut = g_previewHit.origin.x + w * std::clamp(g_app.wipe, 0.0f, 1.0f);
            const sgm::Image& img = (mouse.x < cut) ? g_app.previewSource : g_app.previewMapped;
            if (SampleImageUv(img, u, v, outRgb)) {
                return true;
            }
        }
    }
    return SampleScreenPixel(screen, outRgb);
}

static void EndEyedropperCapture() {
    if (g_hwnd && GetCapture() == g_hwnd) {
        ReleaseCapture();
    }
}

static void CancelEyedropper() {
    if (g_app.pickingStop >= 0 &&
        g_app.pickingStop < static_cast<int>(g_app.gradient.stops.size())) {
        sgm::ColorStop& s = g_app.gradient.stops[static_cast<std::size_t>(g_app.pickingStop)];
        s.r = g_app.pickBackupR;
        s.g = g_app.pickBackupG;
        s.b = g_app.pickBackupB;
        g_app.dirty = true;
    }
    g_app.pickingStop = -1;
    g_app.pickWaitRelease = false;
    g_app.pickLmbDown = false;
    g_app.hsvStop = -1;
    EndEyedropperCapture();
    g_app.status = "Eyedropper cancelled";
}

static void ConfirmEyedropper() {
    g_app.pickingStop = -1;
    g_app.pickWaitRelease = false;
    g_app.pickLmbDown = false;
    g_app.presetIndex = -1;
    g_app.hsvStop = -1;
    g_app.dirty = true;
    EndEyedropperCapture();
    g_app.status = "Stop color sampled";
}

static void BeginEyedropper(int stop) {
    if (stop < 0 || stop >= static_cast<int>(g_app.gradient.stops.size())) {
        return;
    }
    if (g_app.pickingStop >= 0 && g_app.pickingStop != stop) {
        CancelEyedropper();
    }
    const sgm::ColorStop& s = g_app.gradient.stops[static_cast<std::size_t>(stop)];
    g_app.selectedStop = stop;
    g_app.pickingStop = stop;
    g_app.pickBackupR = s.r;
    g_app.pickBackupG = s.g;
    g_app.pickBackupB = s.b;
    g_app.pickWaitRelease = true;
    g_app.pickLmbDown = true;
    SetCapture(g_hwnd);
    g_app.status = "Eyedropper: click a color on the preview or in SAI  ·  Esc / RMB to cancel";
}

static void DrawEyedropperOverlay(float rgb[3]) {
    POINT screen{};
    GetCursorPos(&screen);
    POINT client = screen;
    ScreenToClient(g_hwnd, &client);
    const ImVec2 p(static_cast<float>(client.x) + 18.0f, static_cast<float>(client.y) + 18.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 fill = IM_COL32(
        static_cast<int>(rgb[0] * 255.0f + 0.5f),
        static_cast<int>(rgb[1] * 255.0f + 0.5f),
        static_cast<int>(rgb[2] * 255.0f + 0.5f),
        255);
    dl->AddRectFilled(p, ImVec2(p.x + 56.0f, p.y + 56.0f), fill, 4.0f);
    dl->AddRect(p, ImVec2(p.x + 56.0f, p.y + 56.0f), IM_COL32(255, 255, 255, 230), 4.0f, 0, 2.0f);
    char hex[16];
    std::snprintf(
        hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(rgb[0] * 255.0f + 0.5f),
        static_cast<int>(rgb[1] * 255.0f + 0.5f), static_cast<int>(rgb[2] * 255.0f + 0.5f));
    dl->AddRectFilled(ImVec2(p.x, p.y + 58.0f), ImVec2(p.x + 86.0f, p.y + 80.0f), IM_COL32(12, 12, 14, 220), 3.0f);
    dl->AddText(ImVec2(p.x + 6.0f, p.y + 61.0f), IM_COL32(240, 240, 245, 255), hex);
}

static void UpdateEyedropper() {
    if (g_app.pickingStop < 0) {
        return;
    }
    if (g_app.pickingStop >= static_cast<int>(g_app.gradient.stops.size())) {
        g_app.pickingStop = -1;
        EndEyedropperCapture();
        return;
    }

    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    SetCursor(LoadCursor(nullptr, IDC_CROSS));

    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || rmb) {
        CancelEyedropper();
        return;
    }

    if (g_app.pickWaitRelease) {
        if (!lmb && !rmb) {
            g_app.pickWaitRelease = false;
        }
        return;
    }

    float rgb[3] = {0, 0, 0};
    if (SampleColorAtCursor(rgb)) {
        sgm::ColorStop& s = g_app.gradient.stops[static_cast<std::size_t>(g_app.pickingStop)];
        if (s.r != rgb[0] || s.g != rgb[1] || s.b != rgb[2]) {
            s.r = rgb[0];
            s.g = rgb[1];
            s.b = rgb[2];
            g_app.dirty = true;
        }
        DrawEyedropperOverlay(rgb);
    }

    if (lmb && !g_app.pickLmbDown) {
        ConfirmEyedropper();
        return;
    }
    g_app.pickLmbDown = lmb;
}

static bool EyedropperButton(const char* id, bool active) {
    const ImVec2 size(24.0f, 24.0f);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.82f, 0.95f, 1.0f));
    }
    const bool pressed = ImGui::Button(id, size);
    if (active) {
        ImGui::PopStyleColor();
    }
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = active ? IM_COL32(20, 20, 20, 255) : IM_COL32(40, 40, 40, 255);
    dl->AddCircle(ImVec2(c.x - 2.5f, c.y + 3.0f), 4.2f, col, 10, 1.6f);
    dl->AddLine(ImVec2(c.x + 0.5f, c.y - 0.5f), ImVec2(c.x + 6.5f, c.y - 6.5f), col, 2.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Eyedropper: sample from the preview or SAI");
    }
    return pressed;
}

static bool GradientEditor(sgm::Gradient& g, int& selected) {
    g.ensureValid();
    bool changed = false;

    const float width = ImGui::GetContentRegionAvail().x;
    const float barH = 18.0f;
    const float markH = 16.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 barMin = pos;
    const ImVec2 barMax = ImVec2(pos.x + width, pos.y + barH);

    const int cols = std::max(2, static_cast<int>(width));
    for (int i = 0; i < cols; ++i) {
        float c[4];
        g.eval(static_cast<float>(i) / static_cast<float>(cols - 1), c);
        const ImU32 col = IM_COL32(
            static_cast<int>(c[0] * 255.0f + 0.5f),
            static_cast<int>(c[1] * 255.0f + 0.5f),
            static_cast<int>(c[2] * 255.0f + 0.5f),
            255);
        const float x0 = barMin.x + static_cast<float>(i);
        dl->AddRectFilled(ImVec2(x0, barMin.y), ImVec2(x0 + 1.0f, barMax.y), col);
    }
    dl->AddRect(barMin, barMax, IM_COL32(64, 64, 64, 255));

    ImGui::InvisibleButton("##gradient_bar", ImVec2(width, barH + markH + 4.0f));
    const bool picking = g_app.pickingStop >= 0;
    const bool hovered = !picking && ImGui::IsItemHovered();
    const bool active = !picking && ImGui::IsItemActive();

    auto stopX = [&](float t) { return barMin.x + t * width; };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float my = ImGui::GetIO().MousePos.y;
        int hit = -1;
        float best = 10.0f;
        for (int i = 0; i < static_cast<int>(g.stops.size()); ++i) {
            const float dx = std::fabs(mx - stopX(g.stops[static_cast<std::size_t>(i)].t));
            const float dy = my - barMax.y;
            if (dx < best && dy > -4.0f && dy < markH + 6.0f) {
                best = dx;
                hit = i;
            }
        }
        if (hit >= 0) {
            selected = hit;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                BeginEyedropper(hit);
            }
        } else if (my <= barMax.y + 2.0f) {
            const float tDisplay = std::clamp((mx - barMin.x) / width, 0.0f, 1.0f);
            const float gradT = g.reverse ? 1.0f - tDisplay : tDisplay;
            sgm::Gradient sample = g;
            sample.reverse = false;
            float c[4];
            sample.eval(gradT, c);
            sgm::ColorStop stop;
            stop.t = gradT;
            stop.r = c[0];
            stop.g = c[1];
            stop.b = c[2];
            stop.a = 1.0f;
            g.stops.push_back(stop);
            g.sortStops();
            for (int i = 0; i < static_cast<int>(g.stops.size()); ++i) {
                if (std::fabs(g.stops[static_cast<std::size_t>(i)].t - stop.t) < 1.0e-4f) {
                    selected = i;
                    break;
                }
            }
            changed = true;
        }
    }

    if (active && selected >= 0 && selected < static_cast<int>(g.stops.size()) &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float mx = ImGui::GetIO().MousePos.x;
        float t = std::clamp((mx - barMin.x) / width, 0.0f, 1.0f);
        if (g.reverse) {
            t = 1.0f - t;
        }
        g.stops[static_cast<std::size_t>(selected)].t = t;
        const sgm::ColorStop kept = g.stops[static_cast<std::size_t>(selected)];
        g.sortStops();
        for (int i = 0; i < static_cast<int>(g.stops.size()); ++i) {
            const auto& s = g.stops[static_cast<std::size_t>(i)];
            if (s.r == kept.r && s.g == kept.g && s.b == kept.b && s.t == kept.t) {
                selected = i;
                break;
            }
        }
        changed = true;
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        selected >= 0 && static_cast<int>(g.stops.size()) > 2) {
        g.stops.erase(g.stops.begin() + selected);
        selected = std::clamp(selected, 0, static_cast<int>(g.stops.size()) - 1);
        changed = true;
    }

    for (int i = 0; i < static_cast<int>(g.stops.size()); ++i) {
        const auto& s = g.stops[static_cast<std::size_t>(i)];
        const float displayT = g.reverse ? 1.0f - s.t : s.t;
        const float x = stopX(displayT);
        const float hw = (i == selected) ? 7.0f : 6.0f;
        const ImU32 fill = ColorU32(s);
        const ImU32 outline = (i == selected) ? IM_COL32(32, 96, 176, 255) : IM_COL32(40, 40, 40, 255);
        dl->AddTriangleFilled(
            ImVec2(x, barMax.y), ImVec2(x - hw, barMax.y + 7.0f), ImVec2(x + hw, barMax.y + 7.0f), fill);
        dl->AddRectFilled(ImVec2(x - hw, barMax.y + 7.0f), ImVec2(x + hw, barMax.y + markH), fill);
        dl->AddTriangle(
            ImVec2(x, barMax.y), ImVec2(x - hw, barMax.y + 7.0f), ImVec2(x + hw, barMax.y + 7.0f), outline, 1.0f);
        dl->AddRect(ImVec2(x - hw, barMax.y + 7.0f), ImVec2(x + hw, barMax.y + markH), outline);
    }

    return changed;
}

static void DrawChecker(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(156, 156, 156, 255));
    const int x0 = static_cast<int>(a.x / cell);
    const int y0 = static_cast<int>(a.y / cell);
    const int x1 = static_cast<int>(b.x / cell) + 1;
    const int y1 = static_cast<int>(b.y / cell) + 1;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (((x + y) & 1) == 0) {
                continue;
            }
            ImVec2 p0(x * cell, y * cell);
            ImVec2 p1((x + 1) * cell, (y + 1) * cell);
            p0.x = std::max(p0.x, a.x);
            p0.y = std::max(p0.y, a.y);
            p1.x = std::min(p1.x, b.x);
            p1.y = std::min(p1.y, b.y);
            if (p1.x > p0.x && p1.y > p0.y) {
                dl->AddRectFilled(p0, p1, IM_COL32(168, 168, 168, 255));
            }
        }
    }
}

static void DrawPreview() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(120.0f, avail.y - 4.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + avail.x, p.y + avail.y);
    DrawChecker(dl, p, q, 10.0f);

    if (!g_app.texOrig.srv || !g_app.texMapped.srv) {
        const char* hint = "Drop a PNG here or paste a layer from SAI";
        const ImVec2 sz = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(p.x + (avail.x - sz.x) * 0.5f, p.y + (avail.y - sz.y) * 0.5f),
                    IM_COL32(48, 48, 48, 255), hint);
        ImGui::Dummy(avail);
        g_previewHit.valid = false;
        return;
    }

    const float iw = static_cast<float>(g_app.texMapped.w);
    const float ih = static_cast<float>(g_app.texMapped.h);
    const float scale = std::min(avail.x / iw, avail.y / ih);
    const ImVec2 size(iw * scale, ih * scale);
    const ImVec2 origin(p.x + (avail.x - size.x) * 0.5f, p.y + (avail.y - size.y) * 0.5f);
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    g_previewHit.origin = origin;
    g_previewHit.end = end;
    g_previewHit.valid = true;

    dl->AddImage(reinterpret_cast<ImTextureID>(g_app.texMapped.srv), origin, end);
    const float cut = origin.x + size.x * std::clamp(g_app.wipe, 0.0f, 1.0f);
    dl->PushClipRect(origin, ImVec2(cut, end.y), true);
    dl->AddImage(reinterpret_cast<ImTextureID>(g_app.texOrig.srv), origin, end);
    dl->PopClipRect();
    dl->AddLine(ImVec2(cut, origin.y), ImVec2(cut, end.y), IM_COL32(32, 32, 32, 220), 1.5f);
    dl->AddText(ImVec2(origin.x + 8.0f, origin.y + 6.0f), IM_COL32(20, 20, 20, 230), "original");
    const ImVec2 rs = ImGui::CalcTextSize("mapped");
    dl->AddText(ImVec2(end.x - rs.x - 8.0f, origin.y + 6.0f), IM_COL32(20, 20, 20, 230), "mapped");

    ImGui::InvisibleButton("##preview", avail);
}

static void DrawUi() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(
        "##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open PNG…", "Ctrl+O")) {
                const std::wstring p = OpenImageDialog();
                if (!p.empty()) {
                    OpenFile(p);
                }
            }
            if (ImGui::MenuItem("Save result…", "Ctrl+S", false, !g_app.source.empty())) {
                SaveResult();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("From SAI clipboard", "Ctrl+V")) {
                LoadFromClipboard();
            }
            if (ImGui::MenuItem("Copy result", "Ctrl+C", false, !g_app.source.empty())) {
                CopyResult();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Always on top", nullptr, g_app.alwaysOnTop)) {
                g_app.alwaysOnTop = !g_app.alwaysOnTop;
                SetAlwaysOnTop(g_app.alwaysOnTop);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    SyncHsvFromSelected(g_app.pickingStop == g_app.selectedStop);

    ImGui::BeginChild("##sai_side", ImVec2(268.0f, 0.0f), ImGuiChildFlags_Border, ImGuiWindowFlags_None);

    SaiHeader("Color");
    if (DrawSaiColorWheel(248.0f)) {
        ApplyHsvToSelected();
    }

    if (g_app.selectedStop >= 0 &&
        g_app.selectedStop < static_cast<int>(g_app.gradient.stops.size())) {
        const sgm::ColorStop& cur = g_app.gradient.stops[static_cast<std::size_t>(g_app.selectedStop)];
        ImGui::ColorButton("##curcol", ImVec4(cur.r, cur.g, cur.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(36.0f, 22.0f));
        ImGui::SameLine();
        if (EyedropperButton("##pip_selected", g_app.pickingStop == g_app.selectedStop)) {
            if (g_app.pickingStop == g_app.selectedStop) {
                CancelEyedropper();
            } else {
                BeginEyedropper(g_app.selectedStop);
            }
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Stop %d", g_app.selectedStop + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete") && g_app.gradient.stops.size() > 2) {
            g_app.gradient.stops.erase(g_app.gradient.stops.begin() + g_app.selectedStop);
            g_app.selectedStop = std::clamp(
                g_app.selectedStop, 0, static_cast<int>(g_app.gradient.stops.size()) - 1);
            g_app.hsvStop = -1;
            g_app.presetIndex = -1;
            g_app.dirty = true;
        }
    }

    SaiHeader("Gradient");
    if (GradientEditor(g_app.gradient, g_app.selectedStop)) {
        g_app.presetIndex = -1;
        g_app.hsvStop = -1;
        g_app.dirty = true;
    }

    {
        const char* previewName =
            (g_app.presetIndex >= 0 && g_app.presetIndex < static_cast<int>(g_app.presets.size()))
                ? g_app.presets[static_cast<std::size_t>(g_app.presetIndex)].name
                : "Custom";
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preset");
        ImGui::SameLine(72.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##preset", previewName)) {
            for (int i = 0; i < static_cast<int>(g_app.presets.size()); ++i) {
                const bool sel = (i == g_app.presetIndex);
                if (ImGui::Selectable(g_app.presets[static_cast<std::size_t>(i)].name, sel)) {
                    ApplyPreset(i);
                }
                if (sel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    if (g_app.selectedStop >= 0 &&
        g_app.selectedStop < static_cast<int>(g_app.gradient.stops.size())) {
        sgm::ColorStop& s = g_app.gradient.stops[static_cast<std::size_t>(g_app.selectedStop)];
        float t = s.t;
        if (SaiSlider("Position", &t, 0.0f, 1.0f, "%.2f")) {
            s.t = t;
            const sgm::ColorStop kept = s;
            g_app.gradient.sortStops();
            for (int i = 0; i < static_cast<int>(g_app.gradient.stops.size()); ++i) {
                const auto& cand = g_app.gradient.stops[static_cast<std::size_t>(i)];
                if (cand.t == kept.t && cand.r == kept.r && cand.g == kept.g && cand.b == kept.b) {
                    g_app.selectedStop = i;
                    break;
                }
            }
            g_app.presetIndex = -1;
            g_app.dirty = true;
        }
    }

    if (ImGui::Checkbox("Reverse", &g_app.gradient.reverse)) {
        g_app.dirty = true;
    }

    SaiHeader("Map");
    if (SaiSlider("Mix", &g_app.settings.mix, 0.0f, 1.0f, "%.2f")) {
        g_app.dirty = true;
    }
    SaiSlider("Compare", &g_app.wipe, 0.0f, 1.0f, "%.2f");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Luma");
    ImGui::SameLine(72.0f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const char* lumaItems[] = {"Photoshop", "Rec.709"};
    int luma = (g_app.settings.luma == sgm::LumaMode::Photoshop) ? 0 : 1;
    if (ImGui::Combo("##luma", &luma, lumaItems, 2)) {
        g_app.settings.luma = luma == 0 ? sgm::LumaMode::Photoshop : sgm::LumaMode::Rec709;
        g_app.dirty = true;
    }

    if (ImGui::Checkbox("Keep alpha", &g_app.settings.preserveAlpha)) {
        g_app.dirty = true;
    }
    if (ImGui::Checkbox("Always on top", &g_app.alwaysOnTop)) {
        SetAlwaysOnTop(g_app.alwaysOnTop);
    }

    SaiHeader("Layer");
    if (SaiToolButton("fromclip", "Paste", false)) {
        LoadFromClipboard();
    }
    ImGui::SameLine();
    if (SaiToolButton("toclip", "Copy", false)) {
        CopyResult();
    }
    if (SaiToolButton("open", "Open", false)) {
        const std::wstring path = OpenImageDialog();
        if (!path.empty()) {
            OpenFile(path);
        }
    }
    ImGui::SameLine();
    if (SaiToolButton("save", "Save", false)) {
        SaveResult();
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextWrapped("%s", g_app.status.c_str());
    ImGui::TextDisabled("Ctrl+Alt+G grabs the clipboard. Eyedropper samples the canvas or SAI.");
    ImGui::PopTextWrapPos();

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##sai_canvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Border, ImGuiWindowFlags_None);
    DrawPreview();
    ImGui::EndChild();

    ImGui::End();

    if (!io.WantTextInput && g_app.pickingStop < 0) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            LoadFromClipboard();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopyResult();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            const std::wstring p = OpenImageDialog();
            if (!p.empty()) {
                OpenFile(p);
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            SaveResult();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && g_app.pickingStop < 0 &&
            g_app.gradient.stops.size() > 2) {
            g_app.gradient.stops.erase(g_app.gradient.stops.begin() + g_app.selectedStop);
            g_app.selectedStop = std::clamp(
                g_app.selectedStop, 0, static_cast<int>(g_app.gradient.stops.size()) - 1);
            g_app.presetIndex = -1;
            g_app.hsvStop = -1;
            g_app.dirty = true;
        }
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
    case WM_SETCURSOR:
        if (g_app.pickingStop >= 0) {
            SetCursor(LoadCursor(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) {
            g_droppedFile = path;
        }
        DragFinish(drop);
        return 0;
    }
    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            g_hotkeyPaste = true;
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            return 0;
        }
        g_ResizeWidth = LOWORD(lParam);
        g_ResizeHeight = HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SaiGradientMapWindow";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowW(
        wc.lpszClassName, L"SAI Gradient Map", WS_OVERLAPPEDWINDOW, 80, 80, 1280, 860,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    DragAcceptFiles(g_hwnd, TRUE);
    RegisterHotKey(g_hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT, 'G');

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    SetAlwaysOnTop(g_app.alwaysOnTop);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "imgui.ini";

    const char* fontCandidates[] = {
        "C:\\Windows\\Fonts\\tahoma.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
    };
    for (const char* fontPath : fontCandidates) {
        if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES) {
            io.Fonts->AddFontFromFileTTF(fontPath, 13.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
            break;
        }
    }

    ApplySaiStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ApplyPreset(0);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        if (!g_droppedFile.empty()) {
            OpenFile(g_droppedFile);
            g_droppedFile.clear();
        }
        if (g_hotkeyPaste) {
            g_hotkeyPaste = false;
            LoadFromClipboard();
        }
        if (g_app.dirty) {
            RebuildPreview();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        UpdateEyedropper();
        if (g_app.dirty) {
            RebuildPreview();
        }
        DrawUi();
        ImGui::Render();

        const float clear[4] = {0.831f, 0.831f, 0.831f, 1.0f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    g_app.texOrig.clear();
    g_app.texMapped.clear();
    UnregisterHotKey(g_hwnd, kHotkeyId);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
