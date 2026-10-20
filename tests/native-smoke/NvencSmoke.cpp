// Native smoke harness for NvencNative.dll (no YMM4 / .NET needed).
// Loads the DLL dynamically so the same exe can drive the baseline or a new build.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <thread>

typedef void* (*PFN_Create)(ID3D11Device*, int, int, int, int, int, int, int, int, int, int, int, int, const wchar_t*);
typedef void* (*PFN_CreatePool)(ID3D11Device*, int, int, int, int, int, int, int, int, int, int, int, int, const wchar_t*, int);
typedef int (*PFN_Encode)(void*, ID3D11Texture2D*);
typedef int (*PFN_Audio)(void*, const float*, int, int, int);
typedef int (*PFN_Finalize)(void*);
typedef void (*PFN_Destroy)(void*);
typedef const wchar_t* (*PFN_Err)(void*);
typedef int (*PFN_EnableOrdered)(void*);
typedef void* (*PFN_CreateMember)(void*, int);
typedef int (*PFN_EncodeEx)(void*, ID3D11Texture2D*, unsigned long long, int);

// Stall watchdog: if no progress for 10 s, print where the harness is stuck.
static std::atomic<int> g_frame{ -1 };
static std::atomic<int> g_stage{ 0 };  // 0 create, 1 render, 2 encode, 3 audio, 4 finalize, 5 destroy, 9 done
static const char* kStageNames[] = { "create", "render", "encode", "audio", "finalize", "destroy" };
// Poor man's stack dump of every other thread: RIP plus stack words that point into loaded modules.
static void DescribeAddr(ULONG_PTR a)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m)
    {
        wchar_t name[MAX_PATH] = L"?";
        GetModuleFileNameW(m, name, MAX_PATH);
        const wchar_t* base = wcsrchr(name, L'\\'); base = base ? base + 1 : name;
        printf("    %ls+0x%llx\n", base, (unsigned long long)(a - (ULONG_PTR)m));
    }
}
static void DumpThreads()
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1)
        {
            CONTEXT c = {}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(t, &c))
            {
                printf("  thread %lu rip:\n", te.th32ThreadID);
                DescribeAddr((ULONG_PTR)c.Rip);
                ULONG_PTR* sp = (ULONG_PTR*)c.Rsp; int shown = 0;
                for (int i = 0; i < 400 && shown < 24; ++i)
                {
                    ULONG_PTR v; SIZE_T rd = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), &rd) || rd != sizeof(v)) break;
                    HMODULE m = nullptr;
                    if (v > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m) && m) { DescribeAddr(v); ++shown; }
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    fflush(stdout);
}
static void Watchdog()
{
    int lastFrame = -2, lastStage = -2, idle = 0;
    for (;;)
    {
        Sleep(1000);
        if (g_stage == 9) return;
        if (g_frame == lastFrame && g_stage == lastStage) { if (++idle == 10) { printf("STALL stage=%s frame=%d (no progress for 10 s)\n", kStageNames[g_stage.load()], g_frame.load()); fflush(stdout); DumpThreads(); } }
        else { idle = 0; lastFrame = g_frame; lastStage = g_stage; }
    }
}

static const int kMarkerBits = 16;   // frame index encoded in a 16-block strip
static const int kMarkerBlock = 32;  // each block is 32x32 px (survives lossy coding)

struct Opts
{
    std::wstring dll = L"NvencNative.dll", out = L"smoke.mp4";
    std::string codec = "h264";
    int frames = 600, width = 1920, height = 1080, fps = 60, pool = 6, hevcAsync = 0;
    int sessions = 1, chunk = 0;
    int pregen = 0, mt = 0, sync = 0, quality = 1, audio = 1, cancelAt = -1, bitrate = 0, rc = 0, debugLog = 0, srgb = 0;
};

static std::wstring W(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

static bool Parse(int argc, char** argv, Opts& o)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--dll") o.dll = W(next());
        else if (a == "--out") o.out = W(next());
        else if (a == "--codec") o.codec = next();
        else if (a == "--frames") o.frames = atoi(next());
        else if (a == "--width") o.width = atoi(next());
        else if (a == "--height") o.height = atoi(next());
        else if (a == "--fps") o.fps = atoi(next());
        else if (a == "--pool") o.pool = atoi(next());
        else if (a == "--hevc-async") o.hevcAsync = atoi(next());
        else if (a == "--quality") o.quality = atoi(next());
        else if (a == "--audio") o.audio = atoi(next());
        else if (a == "--cancel-at") o.cancelAt = atoi(next());
        else if (a == "--bitrate") o.bitrate = atoi(next());
        else if (a == "--rc") o.rc = atoi(next());
        else if (a == "--debug-log") o.debugLog = atoi(next());
        else if (a == "--mt") o.mt = atoi(next()); // 1: harness enables ID3D10Multithread protection on the device
        else if (a == "--sync") o.sync = atoi(next()); // 0 none, 1 Flush, 2 wait on event query after rendering
        else if (a == "--pregen") o.pregen = atoi(next()); // K>0: pre-render K distinct frames, cycle them (no per-frame CPU upload)
        else if (a == "--sessions") o.sessions = atoi(next()); // N>1: N NVENC sessions merged into one mp4 (NvencEnableOrderedMux)
        else if (a == "--chunk") o.chunk = atoi(next());       // frames per chunk handed to a session (0 = fps*2, the GOP length)
        else if (a == "--srgb") o.srgb = atoi(next());
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return false; }
    }
    return true;
}

#define CHECK_HR(x) do { HRESULT _hr = (x); if (FAILED(_hr)) { fprintf(stderr, "FAIL %s hr=0x%08lx\n", #x, (unsigned long)_hr); return 3; } } while (0)

static double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char** argv)
{
    Opts o;
    if (!Parse(argc, argv, o)) return 2;
    int codec = o.codec == "hevc" ? 1 : (o.codec == "av1" ? 2 : 0);
    if (o.width < kMarkerBits * kMarkerBlock || o.height < kMarkerBlock * 2) { fprintf(stderr, "frame too small\n"); return 2; }

    HMODULE mod = LoadLibraryW(o.dll.c_str());
    if (!mod) { fprintf(stderr, "LoadLibrary failed (%lu)\n", GetLastError()); return 2; }
    auto createPool = (PFN_CreatePool)GetProcAddress(mod, "NvencCreateWithPool");
    auto create = (PFN_Create)GetProcAddress(mod, "NvencCreate");
    auto encode = (PFN_Encode)GetProcAddress(mod, "NvencEncode");
    auto audio = (PFN_Audio)GetProcAddress(mod, "NvencWriteAudio");
    auto finalize = (PFN_Finalize)GetProcAddress(mod, "NvencFinalize");
    auto destroy = (PFN_Destroy)GetProcAddress(mod, "NvencDestroy");
    auto lastErr = (PFN_Err)GetProcAddress(mod, "NvencGetLastError");
    auto enableOrdered = (PFN_EnableOrdered)GetProcAddress(mod, "NvencEnableOrderedMux");
    auto createMember = (PFN_CreateMember)GetProcAddress(mod, "NvencCreateMember");
    auto encodeEx = (PFN_EncodeEx)GetProcAddress(mod, "NvencEncodeEx");
    if (!encode || !audio || !finalize || !destroy || !lastErr || (!create && !createPool)) { fprintf(stderr, "missing exports\n"); return 2; }

    // NVIDIA hardware adapter.
    IDXGIFactory1* factory = nullptr;
    CHECK_HR(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory));
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        wprintf(L"adapter %u: %ls vendor=0x%04x\n", i, d.Description, d.VendorId);
        if (!adapter && d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) adapter = a; else a->Release();
    }
    if (!adapter) { fprintf(stderr, "no NVIDIA adapter\n"); return 3; }

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    CHECK_HR(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx));

    if (o.mt)
    {
        ID3D10Multithread* mtp = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mtp))) { mtp->SetMultithreadProtected(TRUE); mtp->Release(); }
    }

    // Same properties YMM4 passes: BGRA, RT|SRV, single-sampled, default usage.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = o.width; td.Height = o.height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = o.srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    CHECK_HR(dev->CreateTexture2D(&td, nullptr, &tex));
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = td.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    CHECK_HR(dev->CreateRenderTargetView(tex, &rd, &rtv));

    int bitrate = o.bitrate > 0 ? o.bitrate : (o.width * o.height > 2000000 ? 50000 : 20000);
    const int bufferFormat = 0x01000000; // ARGB, what the managed plugin passes for B8G8R8A8
    DeleteFileW(o.out.c_str());

    std::thread(Watchdog).detach();
    auto t0 = std::chrono::steady_clock::now();
    void* h = createPool
        ? createPool(dev, o.width, o.height, o.fps, bitrate, codec, o.quality, 0, o.rc, bitrate, bufferFormat, o.hevcAsync, o.debugLog, o.out.c_str(), o.pool)
        : create(dev, o.width, o.height, o.fps, bitrate, codec, o.quality, 0, o.rc, bitrate, bufferFormat, o.hevcAsync, o.debugLog, o.out.c_str());
    auto tCreated = std::chrono::steady_clock::now();
    if (!h) { fprintf(stderr, "RESULT ok=0 stage=create\n"); return 4; }
    if (lastErr(h) && *lastErr(h)) { wprintf(L"create error: %ls\n", lastErr(h)); printf("RESULT ok=0 stage=create\n"); destroy(h); return 4; }

    // Marker strip pixel data (BGRA): white/black 32x32 blocks, 16 blocks.
    std::vector<uint32_t> strip(kMarkerBits * kMarkerBlock * kMarkerBlock);
    std::vector<uint32_t> rect(128 * 128, 0xFFFF8000u);
    const int sr = 48000, ch = 2;
    double audioCarry = 0, phase = 0;
    std::vector<float> abuf;
    int failedAt = -1;
    double encodeMs = 0;

    auto renderFrame = [&](int f, ID3D11Texture2D* t, ID3D11RenderTargetView* v)
    {
        float bg[4] = { (float)(f % 64) / 255.0f, 0.2f, 0.1f, 1.0f };
        ctx->ClearRenderTargetView(v, bg);
        for (int b = 0; b < kMarkerBits; ++b)
        {
            uint32_t c = ((f >> b) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
            for (int y = 0; y < kMarkerBlock; ++y)
                for (int x = 0; x < kMarkerBlock; ++x)
                    strip[y * kMarkerBits * kMarkerBlock + b * kMarkerBlock + x] = c;
        }
        D3D11_BOX box = { 0, 0, 0, (UINT)(kMarkerBits * kMarkerBlock), (UINT)kMarkerBlock, 1 };
        ctx->UpdateSubresource(t, 0, &box, strip.data(), kMarkerBits * kMarkerBlock * 4, 0);
        int rx = (f * 7) % (o.width - 128), ry = kMarkerBlock + 8 + (f * 3) % (o.height - 128 - kMarkerBlock - 8);
        D3D11_BOX rb = { (UINT)rx, (UINT)ry, 0, (UINT)rx + 128, (UINT)ry + 128, 1 };
        ctx->UpdateSubresource(t, 0, &rb, rect.data(), 128 * 4, 0);

        if (o.sync == 1) ctx->Flush();
        else if (o.sync == 2)
        {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
            ID3D11Query* q = nullptr;
            if (SUCCEEDED(dev->CreateQuery(&qd, &q)))
            {
                ctx->End(q);
                BOOL done = FALSE;
                while (ctx->GetData(q, &done, sizeof(done), 0) == S_FALSE) {}
                q->Release();
            }
        }
    };
    std::vector<ID3D11Texture2D*> ring;
    if (o.pregen > 0)
    {
        for (int k = 0; k < o.pregen; ++k)
        {
            ID3D11Texture2D* t = nullptr; ID3D11RenderTargetView* v = nullptr;
            CHECK_HR(dev->CreateTexture2D(&td, nullptr, &t));
            CHECK_HR(dev->CreateRenderTargetView(t, &rd, &v));
            renderFrame(k, t, v);
            v->Release();
            ring.push_back(t);
        }
        ctx->Flush();
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        ID3D11Query* q = nullptr;
        if (SUCCEEDED(dev->CreateQuery(&qd, &q))) { ctx->End(q); BOOL done = FALSE; while (ctx->GetData(q, &done, sizeof(done), 0) == S_FALSE) {} q->Release(); }
    }

    if (o.sessions > 1)
    {
        // N sessions on one device feeding one muxer. Chunk c (frames [c*chunk, (c+1)*chunk)) goes to session
        // c % N and starts with a forced IDR. Each session is fed from its own thread.
        if (!enableOrdered || !createMember || !encodeEx) { fprintf(stderr, "DLL has no ordered-mux exports\n"); return 2; }
        const int chunk = o.chunk > 0 ? o.chunk : o.fps * 2;
        const int N = o.sessions;
        if (!enableOrdered(h)) { printf("RESULT ok=0 stage=enable_ordered\n"); destroy(h); return 4; }
        std::vector<void*> hs{ h };
        for (int i = 1; i < N; ++i)
        {
            void* m = createMember(h, o.pool);
            if (!m || (lastErr(m) && *lastErr(m)))
            {
                wprintf(L"member %d error: %ls\n", i, m && lastErr(m) ? lastErr(m) : L"(null)");
                printf("RESULT ok=0 stage=create_member\n");
                if (m) destroy(m);
                for (size_t k = hs.size(); k-- > 1;) destroy(hs[k]);
                destroy(h);
                return 4;
            }
            hs.push_back(m);
        }
        printf("sessions=%d chunk=%d\n", N, chunk);
        // Frame sources: pregen ring (speed test; markers wrap) or one texture per thread rendered on the fly (markers = frame index).
        std::vector<ID3D11Texture2D*> ring2;
        if (o.pregen > 0)
        {
            for (int k = 0; k < o.pregen; ++k)
            {
                ID3D11Texture2D* t = nullptr; ID3D11RenderTargetView* v = nullptr;
                CHECK_HR(dev->CreateTexture2D(&td, nullptr, &t));
                CHECK_HR(dev->CreateRenderTargetView(t, &rd, &v));
                renderFrame(k, t, v);
                v->Release();
                ring2.push_back(t);
            }
            ctx->Flush();
        }
        std::atomic<int> failed{ -1 };
        std::atomic<int> done{ 0 };
        std::vector<std::thread> th;
        auto tl = std::chrono::steady_clock::now();
        g_stage = 2;
        for (int si = 0; si < N; ++si)
        {
            th.emplace_back([&, si]()
            {
                ID3D11Texture2D* mine = nullptr; ID3D11RenderTargetView* myv = nullptr;
                if (o.pregen <= 0)
                {
                    dev->CreateTexture2D(&td, nullptr, &mine);
                    dev->CreateRenderTargetView(mine, &rd, &myv);
                }
                std::vector<uint32_t> mystrip(kMarkerBits * kMarkerBlock * kMarkerBlock);
                std::vector<uint32_t> myrect(128 * 128, 0xFFFF8000u);
                for (int c = si; (long long)c * chunk < o.frames && failed < 0; c += N)
                {
                    int a = c * chunk, b = std::min(a + chunk, o.frames);
                    for (int f = a; f < b && failed < 0; ++f)
                    {
                        ID3D11Texture2D* cur;
                        if (o.pregen > 0) cur = ring2[f % o.pregen];
                        else
                        {
                            float bg[4] = { (float)(f % 64) / 255.0f, 0.2f, 0.1f, 1.0f };
                            ctx->ClearRenderTargetView(myv, bg);
                            for (int bit = 0; bit < kMarkerBits; ++bit)
                            {
                                uint32_t col = ((f >> bit) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
                                for (int y = 0; y < kMarkerBlock; ++y)
                                    for (int x = 0; x < kMarkerBlock; ++x)
                                        mystrip[y * kMarkerBits * kMarkerBlock + bit * kMarkerBlock + x] = col;
                            }
                            D3D11_BOX box = { 0, 0, 0, (UINT)(kMarkerBits * kMarkerBlock), (UINT)kMarkerBlock, 1 };
                            ctx->UpdateSubresource(mine, 0, &box, mystrip.data(), kMarkerBits * kMarkerBlock * 4, 0);
                            int rx = (f * 7) % (o.width - 128), ry = kMarkerBlock + 8 + (f * 3) % (o.height - 128 - kMarkerBlock - 8);
                            D3D11_BOX rb = { (UINT)rx, (UINT)ry, 0, (UINT)rx + 128, (UINT)ry + 128, 1 };
                            ctx->UpdateSubresource(mine, 0, &rb, myrect.data(), 128 * 4, 0);
                            cur = mine;
                        }
                        if (!encodeEx(hs[si], cur, (unsigned long long)f, f == a ? 1 : 0)) { int e = -1; failed.compare_exchange_strong(e, f); break; }
                        int d = ++done; g_frame = d;
                    }
                }
            });
        }
        std::thread aud;
        if (o.audio)
        {
            aud = std::thread([&]()
            {
                double carry = 0, ph = 0; std::vector<float> ab;
                for (int f = 0; f < o.frames && failed < 0; ++f)
                {
                    while (done.load() + 2 * chunk < f && failed < 0) Sleep(1);   // keep audio roughly level with video
                    carry += (double)sr / o.fps; int n = (int)carry; carry -= n;
                    ab.resize((size_t)n * ch);
                    for (int i = 0; i < n; ++i) { float v = 0.25f * sinf((float)ph); ph += 2.0 * 3.14159265358979 * 440.0 / sr; ab[i * 2] = v; ab[i * 2 + 1] = v; }
                    if (!audio(h, ab.data(), (int)ab.size(), sr, ch)) { int e = -1; failed.compare_exchange_strong(e, f); break; }
                }
            });
        }
        for (auto& t : th) t.join();
        if (aud.joinable()) aud.join();
        auto tf = std::chrono::steady_clock::now();
        if (failed >= 0)
        {
            for (size_t k = 0; k < hs.size(); ++k) if (lastErr(hs[k]) && *lastErr(hs[k])) wprintf(L"session %zu error: %ls\n", k, lastErr(hs[k]));
            printf("RESULT ok=0 stage=encode frame=%d\n", failed.load());
            for (size_t k = hs.size(); k-- > 1;) destroy(hs[k]);
            destroy(h);
            return 5;
        }
        g_stage = 4;
        int fin = 1;
        for (size_t k = 1; k < hs.size(); ++k) if (!finalize(hs[k])) { fin = 0; wprintf(L"member %zu finalize error: %ls\n", k, lastErr(hs[k])); }
        if (fin && !finalize(h)) { fin = 0; wprintf(L"primary finalize error: %ls\n", lastErr(h)); }
        auto tEnd = std::chrono::steady_clock::now();
        g_stage = 5;
        for (size_t k = hs.size(); k-- > 1;) destroy(hs[k]);
        destroy(h);
        g_stage = 9;
        double wall = Ms(tCreated, tEnd), loop = Ms(tl, tf);
        printf("RESULT ok=%d mode=full sessions=%d chunk=%d frames=%d create_ms=%.1f encode_loop_ms=%.1f finalize_ms=%.1f wall_ms=%.1f fps=%.1f\n",
            fin, N, chunk, o.frames, Ms(t0, tCreated), loop, Ms(tf, tEnd), wall, o.frames * 1000.0 / wall);
        return fin ? 0 : 6;
    }
    double renderMs = 0;
    g_stage = 1;
    for (int f = 0; f < o.frames; ++f)
    {
        g_frame = f; g_stage = 1;
        if (o.cancelAt >= 0 && f == o.cancelAt)
        {
            auto tc = std::chrono::steady_clock::now();
            destroy(h);
            printf("RESULT ok=1 mode=cancel frames=%d destroy_ms=%.1f\n", f, Ms(tc, std::chrono::steady_clock::now()));
            return 0;
        }
        ID3D11Texture2D* cur = tex;
        if (o.pregen > 0) cur = ring[f % o.pregen];
        else {
            auto tr = std::chrono::steady_clock::now();
            renderFrame(f, tex, rtv);
            renderMs += Ms(tr, std::chrono::steady_clock::now());
        }
        g_stage = 2;
        auto te = std::chrono::steady_clock::now();
        int r = encode(h, cur);
        encodeMs += Ms(te, std::chrono::steady_clock::now());
        if (!r) { failedAt = f; break; }

        g_stage = 3;
        if (o.audio)
        {
            audioCarry += (double)sr / o.fps;
            int n = (int)audioCarry; audioCarry -= n;
            abuf.resize((size_t)n * ch);
            for (int i = 0; i < n; ++i)
            {
                float v = 0.25f * sinf((float)phase); phase += 2.0 * 3.14159265358979 * 440.0 / sr;
                abuf[i * 2] = v; abuf[i * 2 + 1] = v;
            }
            if (!audio(h, abuf.data(), (int)abuf.size(), sr, ch)) { failedAt = f; break; }
        }
    }
    if (failedAt >= 0)
    {
        wprintf(L"error at frame %d: %ls\n", failedAt, lastErr(h) ? lastErr(h) : L"(null)");
        printf("RESULT ok=0 stage=encode frame=%d\n", failedAt);
        destroy(h);
        return 5;
    }
    auto tf = std::chrono::steady_clock::now();
    g_stage = 4;
    int fin = finalize(h);
    auto tEnd = std::chrono::steady_clock::now();
    const wchar_t* e = lastErr(h);
    if (e && *e) wprintf(L"last error: %ls\n", e);
    g_stage = 5;
    destroy(h);
    g_stage = 9;
    double wall = Ms(tCreated, tEnd);
    printf("RESULT ok=%d mode=full frames=%d create_ms=%.1f render_ms=%.1f encode_loop_ms=%.1f finalize_ms=%.1f wall_ms=%.1f fps=%.1f\n",
        fin ? 1 : 0, o.frames, Ms(t0, tCreated), renderMs, encodeMs, Ms(tf, tEnd), wall, o.frames * 1000.0 / wall);
    return fin ? 0 : 6;
}
