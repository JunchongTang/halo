// halo.cpp — Windows 全屏边框发光工具
//
// 编译: 见同目录 build.bat（MSVC cl）
// 运行: halo.exe --duration 3 --style rainbow
//       halo.exe --duration 3 --style pulse --color "#FF3B30"
//
// 行为与 macOS 版完全一致（契约见 docs/cli-spec.md）：开一个透明、鼠标穿透、
// 置顶、覆盖主显示器的全屏窗口，沿屏幕边缘发光，duration 秒后淡出并自动退出。
//
// 技术栈：Win32 建窗口 + DirectComposition 做无重定向位图的透明合成 +
// Direct2D（D3D11/DXGI 后端）在 GPU 上绘制。动画由交换链 vsync 驱动，
// 与 macOS 的 Core Animation 同级别的丝滑。

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// ── 命令行配置 ───────────────────────────────────────────────

struct Config {
    double       duration = 3.0;          // 持续秒数（含淡出）
    std::wstring style    = L"rainbow";   // "rainbow"(彩色流光) | "pulse"(单色呼吸)
    std::wstring colorHex = L"#3B82F6";   // pulse 模式的颜色
    float        lineWidth = 16.0f;       // 边框光带宽度（96 dpi 下的逻辑像素，运行时按 DPI 缩放）
    float        corner    = 36.0f;       // 内圆角半径（同上，按 DPI 缩放）
};

static std::wstring toLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// 从 Codex 的 JSON 尾参里抠出 "type" 字段值。不引入 JSON 库，
// 只做最小扫描：找到 "type" → 冒号 → 紧随的引号字符串。
static std::wstring extractJsonType(const std::wstring& s) {
    size_t k = s.find(L"\"type\"");
    if (k == std::wstring::npos) return L"";
    size_t colon = s.find(L':', k);
    if (colon == std::wstring::npos) return L"";
    size_t q1 = s.find(L'"', colon);
    if (q1 == std::wstring::npos) return L"";
    size_t q2 = s.find(L'"', q1 + 1);
    if (q2 == std::wstring::npos) return L"";
    return s.substr(q1 + 1, q2 - q1 - 1);
}

// 解析参数。关键点：宽容对待不认识的参数（见 cli-spec.md 的容错要求）。
// 返回 false 表示"非完成事件，静默退出不发光"。
static bool parseArgs(Config& cfg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return true;  // 拿不到参数也照常发光（兜底）

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--duration") {
            if (i + 1 < argc) {
                try { cfg.duration = (std::max)(0.3, std::stod(argv[++i])); }
                catch (...) {}
            }
        } else if (a == L"--style") {
            if (i + 1 < argc) cfg.style = toLower(argv[++i]);
        } else if (a == L"--color") {
            if (i + 1 < argc) cfg.colorHex = argv[++i];
        } else if (!a.empty() && a[0] == L'{') {
            // Codex 的 JSON 尾参：type 不是 agent-turn-complete 就不发光
            std::wstring type = extractJsonType(a);
            if (!type.empty() && type != L"agent-turn-complete") {
                LocalFree(argv);
                return false;
            }
        }
        // 其它不认识的参数一律静默忽略
    }
    LocalFree(argv);

    // 无法识别的 style 回退到 rainbow
    if (cfg.style != L"rainbow" && cfg.style != L"pulse") cfg.style = L"rainbow";
    return true;
}

// ── 工具函数 ─────────────────────────────────────────────────

static D2D1_COLOR_F parseColor(const std::wstring& hex) {
    const D2D1_COLOR_F fallback = D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f); // 兜底蓝
    std::wstring s = hex;
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return fallback;
    try {
        unsigned long v = std::stoul(s, nullptr, 16);
        float r = ((v >> 16) & 0xFF) / 255.0f;
        float g = ((v >> 8)  & 0xFF) / 255.0f;
        float b = ( v        & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, 1.0f);
    } catch (...) {
        return fallback;
    }
}

static void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    switch (((int)i) % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

// 沿屏幕边缘的光带区域：外边界是整屏直角矩形，内边界是向内缩的圆角矩形，
// 用 EXCLUDE 组合（外减内）得到中间那一圈。
//
// 为什么外直角、内圆角：外接显示器是直角，直角外缘能严丝合缝贴满屏幕角；
// 笔记本屏是圆角，直角外缘的角像素正好落在物理圆角被裁掉的位置，会被硬件
// 自动裁成圆角。一份几何同时适配两种屏。
static ComPtr<ID2D1Geometry> makeBandGeometry(
        ID2D1Factory* factory, float w, float h, float lw, float corner) {
    ComPtr<ID2D1RectangleGeometry> outer;
    factory->CreateRectangleGeometry(D2D1::RectF(0, 0, w, h), &outer);

    ComPtr<ID2D1RoundedRectangleGeometry> inner;
    factory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(lw, lw, w - lw, h - lw), corner, corner), &inner);

    ComPtr<ID2D1PathGeometry> band;
    factory->CreatePathGeometry(&band);
    ComPtr<ID2D1GeometrySink> sink;
    band->Open(&sink);
    outer->CombineWithGeometry(inner.Get(), D2D1_COMBINE_MODE_EXCLUDE,
                               D2D1::Matrix3x2F::Identity(), sink.Get());
    sink->Close();
    return band;
}

// ── 全局渲染状态（供窗口过程外的渲染循环使用）────────────────

static HWND                       g_hwnd = nullptr;
static Config                     g_cfg;
static ComPtr<ID2D1DeviceContext> g_dc;
static ComPtr<IDXGISwapChain1>    g_swapChain;
static ComPtr<ID2D1Bitmap1>       g_targetBitmap;   // 交换链后台缓冲
static ComPtr<ID2D1Bitmap1>       g_offscreen;      // 模糊前的离屏中转
static ComPtr<ID2D1Bitmap>        g_conic;          // 彩虹锥形渐变位图（rainbow）
static ComPtr<ID2D1Effect>        g_blur;           // 高斯模糊
static ComPtr<ID2D1Geometry>      g_band;           // 光带几何
static ComPtr<ID2D1SolidColorBrush> g_brush;        // pulse 填充
static float                      g_w = 0, g_h = 0, g_diag = 0;
static D2D1_COLOR_F               g_pulseColor;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// 画一帧。t 为已运行秒数，globalAlpha 为整体淡出系数（1→0）。
static void renderFrame(double t, float globalAlpha) {
    const float cx = g_w / 2.0f, cy = g_h / 2.0f;

    // 第一步：把"光带内的内容"画到离屏位图
    g_dc->SetTarget(g_offscreen.Get());
    g_dc->BeginDraw();
    g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (g_cfg.style == L"pulse") {
        // 单色呼吸：透明度在 0.35 ↔ 1.0 间往返，整周期 1.8 秒
        float o = 0.35f + 0.65f * (0.5f - 0.5f * (float)std::cos(t * (2.0 * 3.14159265 / 1.8)));
        g_brush->SetColor(g_pulseColor);
        g_brush->SetOpacity(o);
        g_dc->FillGeometry(g_band.Get(), g_brush.Get());
    } else {
        // 彩色流光：把锥形彩虹位图绕屏幕中心旋转，再用光带几何裁出边缘那一圈。
        // 旋转一圈 3 秒，和 macOS 版一致。
        float angle = (float)((t / 3.0) * 360.0);
        g_dc->PushLayer(
            D2D1::LayerParameters1(D2D1::InfiniteRect(), g_band.Get()),
            nullptr);
        g_dc->SetTransform(D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(cx, cy)));
        // 放大到对角线尺寸并居中，保证旋转到任意角度都盖满整屏，边角不露空
        D2D1_RECT_F dst = D2D1::RectF(cx - g_diag / 2, cy - g_diag / 2,
                                      cx + g_diag / 2, cy + g_diag / 2);
        g_dc->DrawBitmap(g_conic.Get(), dst);
        g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
        g_dc->PopLayer();
    }
    g_dc->EndDraw();

    // 第二步：把离屏内容高斯模糊后合成到交换链，并施加整体淡出透明度
    g_dc->SetTarget(g_targetBitmap.Get());
    g_dc->BeginDraw();
    g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    g_blur->SetInput(0, g_offscreen.Get());
    g_dc->PushLayer(
        D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                               D2D1::Matrix3x2F::Identity(), globalAlpha),
        nullptr);
    g_dc->DrawImage(g_blur.Get());
    g_dc->PopLayer();
    g_dc->EndDraw();

    g_swapChain->Present(1, 0);  // 跟随 vsync，GPU 驱动的平滑动画
}

// ── 入口 ─────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    if (!parseArgs(g_cfg)) return 0;  // 非完成事件 → 静默退出，退出码 0

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 主显示器矩形（物理像素）。当前实现仅覆盖主屏，和 macOS 版一致。
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(mon, &mi);
    int x = mi.rcMonitor.left;
    int y = mi.rcMonitor.top;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // 窗口：无边框、透明、穿透、置顶、不进任务栏、不抢焦点
    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"HaloWindow";
    RegisterClassEx(&wc);

    g_hwnd = CreateWindowEx(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"HaloWindow", L"Halo", WS_POPUP,
        x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    float dpiScale = GetDpiForWindow(g_hwnd) / 96.0f;
    float lw     = g_cfg.lineWidth * dpiScale;
    float corner = g_cfg.corner * dpiScale;

    // ── D3D11 / Direct2D / DXGI / DirectComposition 初始化 ──
    ComPtr<ID3D11Device> d3d;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr))) return 1;

    ComPtr<IDXGIDevice> dxgiDev;
    d3d.As(&dxgiDev);

    D2D1_FACTORY_OPTIONS fo = {};
    ComPtr<ID2D1Factory1> d2dFactory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &fo, (void**)d2dFactory.GetAddressOf()))) return 1;

    ComPtr<ID2D1Device> d2dDevice;
    d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice);
    d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc);

    // 用于合成的交换链（带预乘 alpha）
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> dxgiFactory;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)dxgiFactory.GetAddressOf());

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width            = w;
    scd.Height           = h;
    scd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount      = 2;
    scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgiFactory->CreateSwapChainForComposition(
            d3d.Get(), &scd, nullptr, &g_swapChain))) return 1;

    // DirectComposition：把交换链作为可视内容挂到窗口上
    ComPtr<IDCompositionDevice> dcomp;
    if (FAILED(DCompositionCreateDevice(dxgiDev.Get(),
            __uuidof(IDCompositionDevice), (void**)dcomp.GetAddressOf()))) return 1;
    ComPtr<IDCompositionTarget> target;
    dcomp->CreateTargetForHwnd(g_hwnd, TRUE, &target);
    ComPtr<IDCompositionVisual> visual;
    dcomp->CreateVisual(&visual);
    visual->SetContent(g_swapChain.Get());
    target->SetRoot(visual.Get());
    dcomp->Commit();

    // 把交换链后台缓冲包成 D2D 目标位图
    ComPtr<IDXGISurface> surface;
    g_swapChain->GetBuffer(0, __uuidof(IDXGISurface), (void**)surface.GetAddressOf());
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(g_dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &g_targetBitmap)))
        return 1;

    // 离屏中转位图（模糊的输入）
    D2D1_BITMAP_PROPERTIES1 op = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(g_dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &op, &g_offscreen)))
        return 1;

    // 几何、画刷、模糊滤镜
    g_w = (float)w; g_h = (float)h;
    g_diag = std::sqrt(g_w * g_w + g_h * g_h);
    g_band = makeBandGeometry(d2dFactory.Get(), g_w, g_h, lw, corner);
    g_pulseColor = parseColor(g_cfg.colorHex);
    g_dc->CreateSolidColorBrush(g_pulseColor, &g_brush);

    g_dc->CreateEffect(CLSID_D2D1GaussianBlur, &g_blur);
    // pulse 用大模糊做外发光，rainbow 用小模糊只柔化边缘
    float sigma = (g_cfg.style == L"pulse" ? 14.0f : 6.0f) * dpiScale;
    g_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);

    // rainbow 模式：预生成一张锥形彩虹位图（CPU 填一次，之后全是 GPU 变换）
    if (g_cfg.style == L"rainbow") {
        const int N = 1024;
        std::vector<uint32_t> px((size_t)N * N);
        const float c = N / 2.0f;
        for (int yy = 0; yy < N; ++yy) {
            for (int xx = 0; xx < N; ++xx) {
                float ang = std::atan2((float)yy - c, (float)xx - c); // [-pi, pi]
                float hue = (ang + 3.14159265f) / (2.0f * 3.14159265f);
                float r, g, b;
                hsvToRgb(hue, 0.9f, 1.0f, r, g, b);
                uint32_t R = (uint32_t)(r * 255), G = (uint32_t)(g * 255), B = (uint32_t)(b * 255);
                // BGRA8 预乘，alpha=255 时预乘=直值
                px[(size_t)yy * N + xx] = (255u << 24) | (R << 16) | (G << 8) | B;
            }
        }
        D2D1_BITMAP_PROPERTIES cbp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        g_dc->CreateBitmap(D2D1::SizeU(N, N), px.data(), N * 4, &cbp, &g_conic);
    }

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    // ── 渲染 / 计时循环 ──
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    double fade = (std::min)(0.5, g_cfg.duration * 0.3);  // 结束前淡出时长

    MSG msg;
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (t >= g_cfg.duration) break;

        float globalAlpha = 1.0f;
        if (t > g_cfg.duration - fade)
            globalAlpha = (float)((g_cfg.duration - t) / fade);
        if (globalAlpha < 0) globalAlpha = 0;

        renderFrame(t, globalAlpha);
    }

    return 0;  // 生命周期结束：启动 → 发光 → 淡出 → 退出，无残留进程
}
