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
    bool dirty = true;
    bool alwaysOnTop = true;
    float wipe = 0.5f;
    std::string status = "Скопируй слой в SAI (Ctrl+C), затем Ctrl+V или Ctrl+Alt+G";
    std::vector<sgm::Preset> presets = sgm::builtinPresets();
};

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
    ofn.lpstrFilter = L"Картинки (PNG, JPEG, BMP, TGA)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0PNG\0*.png\0Все файлы\0*.*\0\0";
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
    SetSourceImage(std::move(img), "Слой из буфера");
}

static sgm::Image MapFull() {
    g_app.gradient.ensureValid();
    return sgm::applyGradientMap(g_app.source, g_app.gradient, g_app.settings);
}

static void CopyResult() {
    if (g_app.source.empty()) {
        g_app.status = "Сначала вставь слой из SAI";
        return;
    }
    std::string err;
    sgm::Image mapped = MapFull();
    if (!sgm::setClipboardImage(g_hwnd, mapped, &err)) {
        g_app.status = err;
        return;
    }
    g_app.status = "Результат в буфере — в SAI нажми Ctrl+V (лучше на новый слой)";
}

static void SaveResult() {
    if (g_app.source.empty()) {
        g_app.status = "Сначала вставь слой из SAI";
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
    g_app.status = "Сохранено PNG";
}

static void OpenFile(const std::wstring& path) {
    std::string err;
    sgm::Image img;
    if (!sgm::loadImageFromFile(path, img, &err)) {
        g_app.status = err;
        return;
    }
    SetSourceImage(std::move(img), "Открыт файл");
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
    g_app.dirty = true;
}

static ImU32 ColorU32(const sgm::ColorStop& s) {
    return IM_COL32(
        static_cast<int>(s.r * 255.0f + 0.5f),
        static_cast<int>(s.g * 255.0f + 0.5f),
        static_cast<int>(s.b * 255.0f + 0.5f),
        255);
}

static bool GradientEditor(sgm::Gradient& g, int& selected) {
    g.ensureValid();
    bool changed = false;

    const float width = ImGui::GetContentRegionAvail().x;
    const float barH = 22.0f;
    const float markH = 14.0f;
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
    dl->AddRect(barMin, barMax, IM_COL32(0, 0, 0, 180));

    ImGui::InvisibleButton("##gradient_bar", ImVec2(width, barH + markH + 4.0f));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    auto stopX = [&](float t) { return barMin.x + t * width; };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float my = ImGui::GetIO().MousePos.y;
        int hit = -1;
        float best = 10.0f;
        for (int i = 0; i < static_cast<int>(g.stops.size()); ++i) {
            const float dx = std::fabs(mx - stopX(g.stops[static_cast<std::size_t>(i)].t));
            const float dy = my - barMax.y;
            if (dx < best && dy > -6.0f && dy < markH + 8.0f) {
                best = dx;
                hit = i;
            }
        }
        if (hit >= 0) {
            selected = hit;
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
        const ImVec2 tip(x, barMax.y);
        const float w = (i == selected) ? 8.0f : 6.5f;
        dl->AddTriangleFilled(
            tip, ImVec2(x - w, barMax.y + markH), ImVec2(x + w, barMax.y + markH), ColorU32(s));
        dl->AddTriangle(
            tip, ImVec2(x - w, barMax.y + markH), ImVec2(x + w, barMax.y + markH),
            i == selected ? IM_COL32(255, 210, 70, 255) : IM_COL32(0, 0, 0, 220), 1.6f);
    }

    return changed;
}

static void DrawChecker(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(36, 36, 40, 255));
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
                dl->AddRectFilled(p0, p1, IM_COL32(48, 48, 54, 255));
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
        const char* hint = "Перетащи PNG сюда или вставь слой из SAI";
        const ImVec2 sz = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(p.x + (avail.x - sz.x) * 0.5f, p.y + (avail.y - sz.y) * 0.5f),
                    IM_COL32(200, 200, 210, 255), hint);
        ImGui::Dummy(avail);
        return;
    }

    const float iw = static_cast<float>(g_app.texMapped.w);
    const float ih = static_cast<float>(g_app.texMapped.h);
    const float scale = std::min(avail.x / iw, avail.y / ih);
    const ImVec2 size(iw * scale, ih * scale);
    const ImVec2 origin(p.x + (avail.x - size.x) * 0.5f, p.y + (avail.y - size.y) * 0.5f);
    const ImVec2 end(origin.x + size.x, origin.y + size.y);

    dl->AddImage(reinterpret_cast<ImTextureID>(g_app.texMapped.srv), origin, end);
    const float cut = origin.x + size.x * std::clamp(g_app.wipe, 0.0f, 1.0f);
    dl->PushClipRect(origin, ImVec2(cut, end.y), true);
    dl->AddImage(reinterpret_cast<ImTextureID>(g_app.texOrig.srv), origin, end);
    dl->PopClipRect();
    dl->AddLine(ImVec2(cut, origin.y), ImVec2(cut, end.y), IM_COL32(255, 255, 255, 220), 2.0f);
    dl->AddText(ImVec2(origin.x + 8.0f, origin.y + 6.0f), IM_COL32(255, 255, 255, 200), "оригинал");
    const ImVec2 rs = ImGui::CalcTextSize("карта");
    dl->AddText(ImVec2(end.x - rs.x - 8.0f, origin.y + 6.0f), IM_COL32(255, 255, 255, 200), "карта");

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
        if (ImGui::BeginMenu("Файл")) {
            if (ImGui::MenuItem("Открыть PNG…", "Ctrl+O")) {
                const std::wstring p = OpenImageDialog();
                if (!p.empty()) {
                    OpenFile(p);
                }
            }
            if (ImGui::MenuItem("Сохранить результат…", "Ctrl+S", false, !g_app.source.empty())) {
                SaveResult();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Из буфера SAI", "Ctrl+V")) {
                LoadFromClipboard();
            }
            if (ImGui::MenuItem("Результат в буфер", "Ctrl+C", false, !g_app.source.empty())) {
                CopyResult();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Выход")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Вид")) {
            if (ImGui::MenuItem("Поверх SAI", nullptr, g_app.alwaysOnTop)) {
                g_app.alwaysOnTop = !g_app.alwaysOnTop;
                SetAlwaysOnTop(g_app.alwaysOnTop);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (ImGui::Button("Из буфера SAI")) {
        LoadFromClipboard();
    }
    ImGui::SameLine();
    if (ImGui::Button("Результат в буфер")) {
        CopyResult();
    }
    ImGui::SameLine();
    if (ImGui::Button("Открыть…")) {
        const std::wstring p = OpenImageDialog();
        if (!p.empty()) {
            OpenFile(p);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Сохранить PNG…")) {
        SaveResult();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Поверх SAI", &g_app.alwaysOnTop)) {
        SetAlwaysOnTop(g_app.alwaysOnTop);
    }

    ImGui::Separator();

    ImGui::BeginChild("##preview_child", ImVec2(0.0f, -(248.0f + ImGui::GetStyle().ItemSpacing.y)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_None);
    DrawPreview();
    ImGui::EndChild();

    ImGui::Separator();
    if (GradientEditor(g_app.gradient, g_app.selectedStop)) {
        g_app.presetIndex = -1;
        g_app.dirty = true;
    }

    ImGui::SetNextItemWidth(220.0f);
    const char* preview = (g_app.presetIndex >= 0 && g_app.presetIndex < static_cast<int>(g_app.presets.size()))
                              ? g_app.presets[static_cast<std::size_t>(g_app.presetIndex)].name
                              : "Свой градиент";
    if (ImGui::BeginCombo("Пресет", preview)) {
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
    ImGui::SameLine();
    if (ImGui::Checkbox("Reverse", &g_app.gradient.reverse)) {
        g_app.dirty = true;
    }

    if (g_app.selectedStop >= 0 && g_app.selectedStop < static_cast<int>(g_app.gradient.stops.size())) {
        sgm::ColorStop& s = g_app.gradient.stops[static_cast<std::size_t>(g_app.selectedStop)];
        float col[3] = {s.r, s.g, s.b};
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::ColorEdit3("Цвет стопа", col)) {
            s.r = col[0];
            s.g = col[1];
            s.b = col[2];
            g_app.presetIndex = -1;
            g_app.dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        float t = s.t;
        if (ImGui::SliderFloat("Позиция", &t, 0.0f, 1.0f, "%.3f")) {
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
        ImGui::SameLine();
        if (ImGui::Button("Удалить стоп") && g_app.gradient.stops.size() > 2) {
            g_app.gradient.stops.erase(g_app.gradient.stops.begin() + g_app.selectedStop);
            g_app.selectedStop = std::clamp(
                g_app.selectedStop, 0, static_cast<int>(g_app.gradient.stops.size()) - 1);
            g_app.presetIndex = -1;
            g_app.dirty = true;
        }
    }

    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("Смешение", &g_app.settings.mix, 0.0f, 1.0f, "%.2f")) {
        g_app.dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Сравнение", &g_app.wipe, 0.0f, 1.0f, "%.2f")) {
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Сохранить альфу", &g_app.settings.preserveAlpha)) {
        g_app.dirty = true;
    }

    const char* lumaItems[] = {"Яркость Photoshop", "Яркость Rec.709"};
    int luma = (g_app.settings.luma == sgm::LumaMode::Photoshop) ? 0 : 1;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("##luma", &luma, lumaItems, 2)) {
        g_app.settings.luma = luma == 0 ? sgm::LumaMode::Photoshop : sgm::LumaMode::Rec709;
        g_app.dirty = true;
    }

    ImGui::TextWrapped("%s", g_app.status.c_str());
    ImGui::TextDisabled("Хоткей глобально: Ctrl+Alt+G  ·  клик по полоске — стоп  ·  ПКМ — удалить");

    ImGui::End();

    if (!io.WantTextInput) {
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
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && g_app.gradient.stops.size() > 2) {
            g_app.gradient.stops.erase(g_app.gradient.stops.begin() + g_app.selectedStop);
            g_app.selectedStop = std::clamp(
                g_app.selectedStop, 0, static_cast<int>(g_app.gradient.stops.size()) - 1);
            g_app.presetIndex = -1;
            g_app.dirty = true;
        }
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
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
        wc.lpszClassName, L"SAI Gradient Map", WS_OVERLAPPEDWINDOW, 80, 80, 1180, 820,
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

    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF(fontPath, 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2(12, 10);
    style.ItemSpacing = ImVec2(10, 8);

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
        DrawUi();
        ImGui::Render();

        const float clear[4] = {0.08f, 0.08f, 0.09f, 1.0f};
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
