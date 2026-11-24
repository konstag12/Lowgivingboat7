// pcport_testscene.exe - Windows 10/11 x64 test scene.
//   Win32 window + Direct3D 11 (feature level 11_0+), flip-model swap chain,
//   keyboard + XInput gamepad, threaded fixed-step simulation, switchable
//   frame limiter (F1 = 30, F2 = 60, F3 = unlimited), frame-time capture.
//
// Command line:
//   --fps 30|60|unlimited     initial limiter mode (default 60)
//   --bench SECONDS           scripted route for SECONDS of sim, then exit
//   --csv PATH                per-frame CSV (frame, cpu_frame_ms, gpu_ms, build_ms, present_ms)
//   --summary PATH            append a one-line summary
//   --record PATH             record per-tick input of a live session
//   --replay PATH             replay a recorded session (exits at its end)
//   --width W --height H      client size (default 1280x720)
//   --load N                  N extra translucent full-screen quads (GPU load)
//   --vsync                   present with sync interval 1 (default off)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../core/loop.hpp"

using namespace pcport;

namespace {

template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Vtx { float x, y, r, g, b, a; };

struct Color { float r, g, b, a; };

class Batch {
public:
    std::vector<Vtx> v;
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
        v.push_back({x0, y0, c.r, c.g, c.b, c.a});
        v.push_back({x1, y1, c.r, c.g, c.b, c.a});
        v.push_back({x2, y2, c.r, c.g, c.b, c.a});
    }
    void rect(float x0, float y0, float x1, float y1, Color c) {
        tri(x0, y0, x1, y0, x1, y1, c);
        tri(x0, y0, x1, y1, x0, y1, c);
    }
    void box(float cx, float cy, float hw, float hh, float ang, Color c) {
        const float cs = std::cos(ang), sn = std::sin(ang);
        float px[4], py[4];
        const float lx[4] = {-hw, hw, hw, -hw}, ly[4] = {-hh, -hh, hh, hh};
        for (int i = 0; i < 4; i++) { px[i] = cx + lx[i] * cs - ly[i] * sn; py[i] = cy + lx[i] * sn + ly[i] * cs; }
        tri(px[0], py[0], px[1], py[1], px[2], py[2], c);
        tri(px[0], py[0], px[2], py[2], px[3], py[3], c);
    }
    void circle(float cx, float cy, float r, Color c, int seg = 16) {
        for (int i = 0; i < seg; i++) {
            const float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
            tri(cx, cy, cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1),
                cy + r * std::sin(a1), c);
        }
    }
};

const char* kShader = R"(
cbuffer CB : register(b0) { float4 xf; };
struct VSI { float2 p : POSITION; float4 c : COLOR; };
struct PSI { float4 p : SV_Position; float4 c : COLOR; };
PSI vs_main(VSI i) { PSI o; o.p = float4(i.p * xf.xy + xf.zw, 0, 1); o.c = i.c; return o; }
float4 ps_main(PSI i) : SV_Target { return i.c; }
)";

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                        ID3DBlob**);
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);

struct Options {
    FpsMode fps = FpsMode::Fps60;
    double benchSeconds = 0;
    std::string csv, summary, record, replay;
    int width = 1280, height = 720;
    int load = 0;
    bool vsync = false;
};

void fatal(const char* what, HRESULT hr) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lx)", what, (unsigned long)hr);
    std::fprintf(stderr, "FATAL: %s\n", buf);
    MessageBoxA(nullptr, buf, "pcport_testscene", MB_ICONERROR);
    ExitProcess(1);
}

class App final : public Renderer {
public:
    explicit App(const Options& o) : opt_(o) {}
    ~App() override { shutdown(); }

    void init();
    void shutdown();
    bool beginFrame(FrameLimiter& lim) override;
    void draw(const SimState& s, const SimState& latest) override;

    LiveInput live;
    bool benchMode = false;
    std::vector<double> gpuMs, buildMs, presentMs;
    double shaderCompileMs = 0;
    std::string adapterName;

    LRESULT wndProc(HWND, UINT, WPARAM, LPARAM);

private:
    void createTargets();
    void pollGpuTimers();
    void updateTitle(const SimState& s);

    Options opt_;
    HWND hwnd_ = nullptr;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* vb_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11RasterizerState* rast_ = nullptr;
    static constexpr UINT kMaxVerts = 1 << 18;
    bool tearing_ = false, flip_ = true;
    int w_ = 0, h_ = 0;
    bool resizePending_ = false, quit_ = false;
    std::atomic<int> requestMode_{-1};
    bool requestToggle_ = false;
    PFN_XInputGetState xinputGetState_ = nullptr;
    WORD lastPadButtons_ = 0;
    // GPU timestamp ring
    struct GpuQ { ID3D11Query *dis = nullptr, *t0 = nullptr, *t1 = nullptr; bool pending = false; };
    GpuQ gq_[4];
    int gqi_ = 0;
    // title stats
    double titleT_ = 0, lastFrameT_ = 0;
    int titleFrames_ = 0;
    FpsMode titleMode_ = FpsMode::Fps60;
    Batch batch_;
};

App* g_app = nullptr;

LRESULT CALLBACK WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app) return g_app->wndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT App::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE: quit_ = true; return 0;
        case WM_SIZE:
            if (LOWORD(l) && HIWORD(l)) { w_ = LOWORD(l); h_ = HIWORD(l); resizePending_ = true; }
            return 0;
        case WM_KEYDOWN:
            if (l & (1 << 30)) return 0;  // ignore auto-repeat
            if (w == VK_ESCAPE) quit_ = true;
            if (!benchMode) {
                if (w == VK_F1) requestMode_ = 30;
                if (w == VK_F2) requestMode_ = 60;
                if (w == VK_F3) requestMode_ = 0;
                if (w == VK_TAB || w == 'E') requestToggle_ = true;
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void App::init() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PcPortTestScene";
    RegisterClassExW(&wc);
    RECT r = {0, 0, opt_.width, opt_.height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"PC-Port test scene", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                            nullptr, nullptr, inst, nullptr);
    if (!hwnd_) fatal("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
    ShowWindow(hwnd_, SW_SHOW);
    RECT cr;
    GetClientRect(hwnd_, &cr);
    w_ = std::max<LONG>(1, cr.right);
    h_ = std::max<LONG>(1, cr.bottom);

    // ---- device
    const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 2,
                                   D3D11_SDK_VERSION, &dev_, &got, &ctx_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls + 1, 1,
                               D3D11_SDK_VERSION, &dev_, &got, &ctx_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "hardware device failed (0x%08lx), trying WARP\n", (unsigned long)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fls + 1, 1,
                               D3D11_SDK_VERSION, &dev_, &got, &ctx_);
    }
    if (FAILED(hr)) fatal("D3D11CreateDevice", hr);

    IDXGIDevice1* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* fac = nullptr;
    dev_->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev);
    dxgiDev->SetMaximumFrameLatency(1);  // lower input latency
    dxgiDev->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad;
    if (SUCCEEDED(adapter->GetDesc(&ad))) {
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
        adapterName = name;
    }
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
    if (FAILED(hr)) fatal("IDXGIFactory2 (needs Windows 8+)", hr);
    IDXGIFactory5* fac5 = nullptr;
    if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory5), (void**)&fac5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            tearing_ = allow == TRUE;
        fac5->Release();
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w_;
    sd.Height = h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = fac->CreateSwapChainForHwnd(dev_, hwnd_, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "flip-model swap chain failed (0x%08lx), using blt model\n", (unsigned long)hr);
        flip_ = false;
        tearing_ = false;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        sd.Flags = 0;
        hr = fac->CreateSwapChainForHwnd(dev_, hwnd_, &sd, nullptr, nullptr, &swap_);
    }
    if (FAILED(hr)) fatal("CreateSwapChainForHwnd", hr);
    fac->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    fac->Release();
    adapter->Release();
    dxgiDev->Release();
    createTargets();

    // ---- shaders (runtime compile, timed: this is the "shader compilation" cost)
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dc) fatal("LoadLibrary(d3dcompiler_47.dll)", HRESULT_FROM_WIN32(GetLastError()));
    auto compile = (PFN_D3DCompile)(void*)GetProcAddress(dc, "D3DCompile");
    if (!compile) fatal("GetProcAddress(D3DCompile)", E_NOINTERFACE);
    RealClock& clk = RealClock::instance();
    const double tc0 = clk.now();
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    hr = compile(kShader, std::strlen(kShader), "scene.hlsl", nullptr, nullptr, "vs_main", "vs_4_0",
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
    if (FAILED(hr)) { if (err) std::fprintf(stderr, "%s\n", (char*)err->GetBufferPointer()); fatal("D3DCompile vs", hr); }
    hr = compile(kShader, std::strlen(kShader), "scene.hlsl", nullptr, nullptr, "ps_main", "ps_4_0",
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (FAILED(hr)) { if (err) std::fprintf(stderr, "%s\n", (char*)err->GetBufferPointer()); fatal("D3DCompile ps", hr); }
    shaderCompileMs = (clk.now() - tc0) * 1000.0;
    dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    hr = dev_->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);
    if (FAILED(hr)) fatal("CreateInputLayout", hr);
    vsb->Release();
    psb->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = kMaxVerts * sizeof(Vtx);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(hr = dev_->CreateBuffer(&bd, nullptr, &vb_))) fatal("CreateBuffer vb", hr);
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(hr = dev_->CreateBuffer(&bd, nullptr, &cb_))) fatal("CreateBuffer cb", hr);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&bl, &blend_);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &rast_);

    for (auto& q : gq_) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev_->CreateQuery(&qd, &q.dis);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        dev_->CreateQuery(&qd, &q.t0);
        dev_->CreateQuery(&qd, &q.t1);
    }

    HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
    if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
    if (xi) xinputGetState_ = (PFN_XInputGetState)(void*)GetProcAddress(xi, "XInputGetState");

    std::printf("adapter: %s | feature level 0x%x | swap: %s%s | shader compile %.2f ms | xinput: %s\n",
                adapterName.c_str(), (unsigned)got, flip_ ? "flip-discard" : "blt-discard",
                tearing_ ? " +tearing" : "", shaderCompileMs, xinputGetState_ ? "yes" : "no");
    std::fflush(stdout);
    titleT_ = lastFrameT_ = clk.now();
}

void App::createTargets() {
    ID3D11Texture2D* bb = nullptr;
    HRESULT hr = swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (FAILED(hr)) fatal("GetBuffer", hr);
    hr = dev_->CreateRenderTargetView(bb, nullptr, &rtv_);
    bb->Release();
    if (FAILED(hr)) fatal("CreateRenderTargetView", hr);
}

void App::shutdown() {
    for (auto& q : gq_) { safeRelease(q.dis); safeRelease(q.t0); safeRelease(q.t1); }
    safeRelease(rast_); safeRelease(blend_); safeRelease(cb_); safeRelease(vb_);
    safeRelease(layout_); safeRelease(ps_); safeRelease(vs_); safeRelease(rtv_);
    safeRelease(swap_); safeRelease(ctx_); safeRelease(dev_);
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool App::beginFrame(FrameLimiter& lim) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) quit_ = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (resizePending_ && swap_) {
        resizePending_ = false;
        safeRelease(rtv_);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        HRESULT hr = swap_->ResizeBuffers(0, w_, h_, DXGI_FORMAT_UNKNOWN,
                                          tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(hr)) fatal("ResizeBuffers", hr);
        createTargets();
    }
    const int rm = requestMode_.exchange(-1);
    if (rm >= 0) lim.setMode(rm == 30 ? FpsMode::Fps30 : rm == 60 ? FpsMode::Fps60 : FpsMode::Unlimited);
    titleMode_ = lim.mode();

    // ---- devices -> LiveInput (sampled by the sim thread once per tick)
    float mx = 0, my = 0, th = 0, st = 0;
    bool sprint = false;
    if (GetForegroundWindow() == hwnd_) {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down('W') || down(VK_UP)) { my += 1; th += 1; }
        if (down('S') || down(VK_DOWN)) { my -= 1; th -= 1; }
        if (down('D') || down(VK_RIGHT)) { mx += 1; st -= 1; }
        if (down('A') || down(VK_LEFT)) { mx -= 1; st += 1; }
        sprint = down(VK_SHIFT);
    }
    if (xinputGetState_) {
        XINPUT_STATE xs;
        if (xinputGetState_(0, &xs) == ERROR_SUCCESS) {
            auto axis = [](SHORT v) {
                float f = (float)v / 32767.0f;
                return std::fabs(f) < 0.24f ? 0.0f : std::max(-1.0f, std::min(1.0f, f));
            };
            const float lx = axis(xs.Gamepad.sThumbLX), ly = axis(xs.Gamepad.sThumbLY);
            mx += lx; my += ly; st -= lx;
            th += (xs.Gamepad.bRightTrigger - xs.Gamepad.bLeftTrigger) / 255.0f;
            sprint = sprint || (xs.Gamepad.wButtons & XINPUT_GAMEPAD_A);
            const WORD pressed = xs.Gamepad.wButtons & ~lastPadButtons_;
            if (pressed & XINPUT_GAMEPAD_Y) requestToggle_ = true;
            lastPadButtons_ = xs.Gamepad.wButtons;
        }
    }
    live.setAxes(std::max(-1.f, std::min(1.f, mx)), std::max(-1.f, std::min(1.f, my)),
                 std::max(-1.f, std::min(1.f, th)), std::max(-1.f, std::min(1.f, st)), sprint);
    if (requestToggle_) { live.pressToggle(); requestToggle_ = false; }
    return !quit_;
}

void App::pollGpuTimers() {
    for (auto& q : gq_) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (ctx_->GetData(q.dis, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx_->GetData(q.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx_->GetData(q.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency) gpuMs.push_back((double)(b - a) * 1000.0 / (double)dj.Frequency);
    }
}

static float tileHash(int x, int y) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (float)(h & 0xffff) / 65535.0f;
}

void App::draw(const SimState& s, const SimState& latest) {
    RealClock& clk = RealClock::instance();
    const double tb0 = clk.now();
    pollGpuTimers();
    GpuQ& q = gq_[gqi_];
    const bool useQ = !q.pending;
    if (useQ) { ctx_->Begin(q.dis); ctx_->End(q.t0); }

    // ---- build geometry (world units = metres)
    const float viewH = 40.0f, viewW = viewH * (float)w_ / (float)h_;
    const bool inCar = s.control == Control::Vehicle;
    const float camX = (float)(inCar ? s.cx : s.px), camY = (float)(inCar ? s.cy : s.py);
    Batch& b = batch_;
    b.v.clear();
    const float T = 5.0f;
    const int x0 = (int)std::floor((camX - viewW / 2) / T) - 1, x1 = (int)std::floor((camX + viewW / 2) / T) + 1;
    const int y0 = (int)std::floor((camY - viewH / 2) / T) - 1, y1 = (int)std::floor((camY + viewH / 2) / T) + 1;
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++) {
            const bool road = (tx % 8 == 0) || (ty % 8 == 0);
            const float hsh = tileHash(tx, ty);
            Color c = road ? Color{0.22f, 0.22f, 0.25f, 1} :
                      ((tx + ty) & 1) ? Color{0.30f, 0.50f, 0.32f, 1} : Color{0.27f, 0.46f, 0.29f, 1};
            b.rect(tx * T, ty * T, tx * T + T, ty * T + T, c);
            if (!road && hsh > 0.82f && (std::abs(tx) > 2 || std::abs(ty) > 2))
                b.rect(tx * T + 0.6f, ty * T + 0.6f, tx * T + T - 0.6f, ty * T + T - 0.6f,
                       {0.55f + 0.2f * hsh, 0.52f, 0.48f, 1});
        }
    // checkpoint ring marks at the origin path
    for (int i = 0; i < 12; i++) b.circle(0, i * 10.0f, 0.35f, {1, 0.85f, 0.2f, 0.8f}, 8);

    // vehicle
    const float hd = (float)s.heading;
    b.box((float)s.cx, (float)s.cy, 2.3f, 1.05f, hd, inCar ? Color{0.9f, 0.25f, 0.2f, 1} : Color{0.6f, 0.2f, 0.18f, 1});
    b.box((float)s.cx + std::cos(hd) * 0.6f, (float)s.cy + std::sin(hd) * 0.6f, 0.7f, 0.9f, hd, {0.15f, 0.2f, 0.3f, 1});
    for (int i = 0; i < 4; i++) {
        const float lx = (i & 1) ? 1.4f : -1.4f, ly = (i & 2) ? 1.1f : -1.1f;
        const float wx = (float)s.cx + lx * std::cos(hd) - ly * std::sin(hd);
        const float wy = (float)s.cy + lx * std::sin(hd) + ly * std::cos(hd);
        const float spin = 0.5f + 0.5f * std::sin((float)latest.wheelAngle);
        b.box(wx, wy, 0.4f, 0.18f, hd, {0.08f + 0.1f * spin, 0.08f, 0.08f, 1});
    }
    // character: body + legs swinging with the walk-cycle phase
    if (!inCar) {
        const float swing = std::sin((float)latest.animPhase * 6.2831853f) * 0.35f;
        const float face = std::atan2((float)s.vy, (float)s.vx + 1e-6f);
        const float fx = std::cos(face), fy = std::sin(face);
        b.circle((float)s.px - fy * 0.18f + fx * swing, (float)s.py + fx * 0.18f + fy * swing, 0.15f, {0.1f, 0.1f, 0.4f, 1}, 8);
        b.circle((float)s.px + fy * 0.18f - fx * swing, (float)s.py - fx * 0.18f - fy * swing, 0.15f, {0.1f, 0.1f, 0.4f, 1}, 8);
        b.circle((float)s.px, (float)s.py, 0.42f, {0.95f, 0.75f, 0.55f, 1}, 20);
        b.circle((float)s.px + fx * 0.25f, (float)s.py + fy * 0.25f, 0.1f, {0.1f, 0.1f, 0.1f, 1}, 8);
    }
    // HUD in world space at the top of the view: mission timer bar + checkpoint pips
    const float hx = camX - viewW / 2 + 1, hy = camY + viewH / 2 - 1.6f;
    b.rect(hx, hy, hx + 12, hy + 0.6f, {0, 0, 0, 0.5f});
    b.rect(hx, hy, hx + 12 * (float)(s.missionTimer / 300.0), hy + 0.6f, {0.95f, 0.8f, 0.2f, 0.9f});
    for (uint32_t i = 0; i < std::min<uint32_t>(latest.checkpoints, 40); i++)
        b.rect(hx + i * 0.32f, hy - 0.6f, hx + i * 0.32f + 0.22f, hy - 0.3f, {0.3f, 0.9f, 1, 0.9f});
    // optional synthetic GPU load
    for (int i = 0; i < opt_.load; i++)
        b.rect(camX - viewW / 2, camY - viewH / 2, camX + viewW / 2, camY + viewH / 2, {0.5f, 0.5f, 0.5f, 0.004f});

    const UINT n = (UINT)std::min<size_t>(b.v.size(), kMaxVerts);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, b.v.data(), n * sizeof(Vtx));
        ctx_->Unmap(vb_, 0);
    }
    const float xf[4] = {2.0f / viewW, 2.0f / viewH, -camX * 2.0f / viewW, -camY * 2.0f / viewH};
    ctx_->UpdateSubresource(cb_, 0, nullptr, xf, 0, 0);

    const float clear[4] = {0.05f, 0.06f, 0.08f, 1};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear);
    D3D11_VIEWPORT vp = {0, 0, (float)w_, (float)h_, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(rast_);
    const float bf[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(blend_, bf, 0xffffffff);
    ctx_->IASetInputLayout(layout_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(Vtx), off = 0;
    ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &off);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->Draw(n, 0);
    if (useQ) { ctx_->End(q.t1); ctx_->End(q.dis); q.pending = true; gqi_ = (gqi_ + 1) & 3; }
    const double tb1 = clk.now();

    const UINT flags = (!opt_.vsync && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(opt_.vsync ? 1 : 0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        fatal("Present (device removed)", dev_->GetDeviceRemovedReason());
    const double tb2 = clk.now();
    buildMs.push_back((tb1 - tb0) * 1000.0);
    presentMs.push_back((tb2 - tb1) * 1000.0);
    updateTitle(latest);
}

void App::updateTitle(const SimState& s) {
    RealClock& clk = RealClock::instance();
    const double now = clk.now();
    titleFrames_++;
    if (now - titleT_ < 0.5) return;
    const double fps = titleFrames_ / (now - titleT_);
    titleFrames_ = 0;
    titleT_ = now;
    wchar_t buf[256];
    const int tm = (int)s.missionTimer;
    swprintf(buf, 256, L"PC-Port test scene | limit %hs | %.1f FPS (%.2f ms) | sim %d Hz t=%.1fs | %hs | timer %d:%02d | dist %.0f m%hs",
             fpsModeName(titleMode_), fps, 1000.0 / fps, kSimHz, s.simSeconds(),
             s.control == Control::Vehicle ? "VEHICLE" : "ON FOOT", tm / 60, tm % 60,
             s.charDistance + s.carDistance, benchMode ? " | BENCH" : "");
    SetWindowTextW(hwnd_, buf);
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        auto next = [&](const char*& v) { if (i + 1 >= argc) return false; v = argv[++i]; return true; };
        const char* v = nullptr;
        if (!std::strcmp(argv[i], "--fps") && next(v)) { if (!parseFpsMode(v, o.fps)) return false; }
        else if (!std::strcmp(argv[i], "--bench") && next(v)) o.benchSeconds = std::atof(v);
        else if (!std::strcmp(argv[i], "--csv") && next(v)) o.csv = v;
        else if (!std::strcmp(argv[i], "--summary") && next(v)) o.summary = v;
        else if (!std::strcmp(argv[i], "--record") && next(v)) o.record = v;
        else if (!std::strcmp(argv[i], "--replay") && next(v)) o.replay = v;
        else if (!std::strcmp(argv[i], "--width") && next(v)) o.width = std::atoi(v);
        else if (!std::strcmp(argv[i], "--height") && next(v)) o.height = std::atoi(v);
        else if (!std::strcmp(argv[i], "--load") && next(v)) o.load = std::atoi(v);
        else if (!std::strcmp(argv[i], "--vsync")) o.vsync = true;
        else return false;
    }
    return true;
}

double meanOf(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double s = 0;
    for (double x : v) s += x;
    return s / (double)v.size();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        std::fprintf(stderr,
                     "usage: pcport_testscene [--fps 30|60|unlimited] [--bench SEC] [--csv PATH] "
                     "[--summary PATH] [--record PATH] [--replay PATH] [--width W --height H] "
                     "[--load N] [--vsync]\n");
        return 2;
    }
    SetProcessDPIAware();
    App app(opt);
    g_app = &app;
    app.init();

    ScriptedRoute route;
    std::vector<InputFrame> replayFrames;
    std::unique_ptr<ReplaySource> replay;
    InputSource* src = &app.live;
    RunConfig cfg;
    cfg.fps = opt.fps;
    if (opt.benchSeconds > 0) {
        app.benchMode = true;
        src = &route;
        cfg.ticks = (uint64_t)(opt.benchSeconds * kSimHz);
    } else if (!opt.replay.empty()) {
        if (!ReplaySource::load(opt.replay, replayFrames)) { std::fprintf(stderr, "cannot load %s\n", opt.replay.c_str()); return 1; }
        replay.reset(new ReplaySource(replayFrames));
        src = replay.get();
        cfg.ticks = replay->length();
    }
    std::unique_ptr<RecordingSource> rec;
    if (!opt.record.empty()) { rec.reset(new RecordingSource(*src)); src = rec.get(); }

    RunResult res = runThreaded(cfg, *src, app, RealClock::instance());

    if (rec && !rec->save(opt.record)) std::fprintf(stderr, "cannot write %s\n", opt.record.c_str());
    std::vector<double> gpu = app.gpuMs;
    std::sort(gpu.begin(), gpu.end());
    const double gpuP99 = gpu.empty() ? 0 : gpu[std::min(gpu.size() - 1, (size_t)std::ceil(0.99 * gpu.size()) - 1)];
    std::printf("limit=%s %s | gpu_avg_ms=%.3f gpu_p99_ms=%.3f build_avg_ms=%.3f present_avg_ms=%.3f "
                "shader_compile_ms=%.2f | sim/wall=%.5f late_ticks=%llu\nfinal: %s\n",
                fpsModeName(cfg.fps), formatSummary(res.summary).c_str(), meanOf(app.gpuMs), gpuP99,
                meanOf(app.buildMs), meanOf(app.presentMs), app.shaderCompileMs, res.simToWall,
                (unsigned long long)res.lateTicks, describe(res.final).c_str());
    if (!opt.csv.empty()) {
        FILE* f = std::fopen(opt.csv.c_str(), "w");
        if (f) {
            std::fprintf(f, "frame,cpu_frame_ms,build_ms,present_ms\n");
            const auto& ft = res.frames.samplesMs();
            for (size_t i = 0; i < ft.size(); i++)
                std::fprintf(f, "%zu,%.4f,%.4f,%.4f\n", i, ft[i],
                             i + 1 < app.buildMs.size() ? app.buildMs[i + 1] : 0.0,
                             i + 1 < app.presentMs.size() ? app.presentMs[i + 1] : 0.0);
            std::fclose(f);
        }
    }
    if (!opt.summary.empty()) {
        FILE* f = std::fopen(opt.summary.c_str(), "a");
        if (f) {
            std::fprintf(f, "adapter=\"%s\" %dx%d load=%d limit=%s %s gpu_avg_ms=%.3f hash=%016llx\n",
                         app.adapterName.c_str(), opt.width, opt.height, opt.load, fpsModeName(cfg.fps),
                         formatSummary(res.summary).c_str(), meanOf(app.gpuMs),
                         (unsigned long long)stateHash(res.final));
            std::fclose(f);
        }
    }
    std::fflush(stdout);
    g_app = nullptr;
    return 0;
}
