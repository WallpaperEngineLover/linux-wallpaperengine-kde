// sharedvideo.exe <video> [seconds]
// sharedvideo.exe --open <shared handle> [seconds]: reads another process's shared texture (a live WE's video texture,
// handle from gdb_video_texdesc.py) every 250 ms under its keyed mutex
// Repeats WE 2.8.42's video texture path under Wine to find out where the frames get lost: the render device makes a
// BGRA8 texture with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX (sub_1400EB570), the player opens it on its own
// VIDEO_SUPPORT device (sub_1400F1FA0/sub_1400F2510), the media engine's TransferVideoFrame writes into it under
// AcquireSync(0)/ReleaseSync(0) (sub_1400F3480) and the render device reads it under the same key (sub_1400EE8B0).
// Each step reads the pixels back so the output says which device saw what.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfmediaengine.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <vector>

static const UINT W = 640, H = 360;
// SV_FLUSH=1: Flush () the player context before every ReleaseSync, SV_PLAYERREAD=1: also read the textures back on
// the player device each printed frame (that Map flushes the player context too)
// SV_WE=1: load like WE (byte stream, source reader first, media engine extension), SV_URL default: SetSource (path)
// SV_THREAD=1: WE's split (sub_1400F3930 / sub_1400F3480), a player thread ticks at twice the frame rate under a lock and
// flags new frames, this thread transfers them under the same lock before each read
static bool g_flush, g_playerRead, g_we, g_thread;
static CRITICAL_SECTION g_lock;
static volatile LONG g_newFrame, g_run = 1;
static IMFMediaEngine* g_engine;

static DWORD WINAPI tickThread (void*)
{
    while (g_run) {
        EnterCriticalSection (&g_lock);
        LONGLONG pts = 0;
        if (g_engine->OnVideoStreamTick (&pts) == S_OK)
            g_newFrame = 1;
        LeaveCriticalSection (&g_lock);
        Sleep (16);
    }
    return 0;
}

static void check (const char* what, HRESULT hr)
{
    printf ("%-44s hr=0x%08lx%s\n", what, (unsigned long) hr, FAILED (hr) ? "  FAILED" : "");
    fflush (stdout);
}

// frame number drawn by tests/videotex/make.py: 7 blocks at y=80, x=20+b*88+36, white=1 black=0
static int decodeFrame (const std::vector<uint8_t>& px)
{
    int bits = 0;
    for (int b = 0; b < 7; b++) {
        const uint8_t* p = &px [(80 * W + (20 + b * 88 + 36)) * 4];
        int v = (p [0] + p [1] + p [2]) / 3;
        if (v > 60 && v < 190)
            return -1;
        bits = bits * 2 + (v >= 190);
    }
    return bits;
}

static std::vector<uint8_t> readBack (ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex)
{
    std::vector<uint8_t> out (W * H * 4, 0);
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc (&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED (dev->CreateTexture2D (&desc, nullptr, &staging)))
        return out;
    ctx->CopyResource (staging, tex);
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED (ctx->Map (staging, 0, D3D11_MAP_READ, 0, &map))) {
        for (UINT y = 0; y < H; y++)
            memcpy (&out [y * W * 4], (uint8_t*) map.pData + y * map.RowPitch, W * 4);
        ctx->Unmap (staging, 0);
    }
    staging->Release ();
    return out;
}

static void describe (const char* who, const std::vector<uint8_t>& px)
{
    uint64_t sum = 0;
    for (uint8_t v : px)
        sum += v;
    const uint8_t* c = &px [(H / 2 * W + W / 2) * 4];
    printf ("  %-22s center BGRA %3d %3d %3d %3d  mean %6.2f  frame %d\n", who, c [0], c [1], c [2], c [3],
            (double) sum / px.size (), decodeFrame (px));
}

// reads the shared texture on the render device the way WE's texture bind does (key 0 in, key 0 out)
static std::vector<uint8_t> readOnRender (ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                                          IDXGIKeyedMutex* mutex)
{
    HRESULT hr = mutex->AcquireSync (0, 1000);
    if (hr != S_OK)
        printf ("  render AcquireSync hr=0x%08lx\n", (unsigned long) hr);
    std::vector<uint8_t> px = readBack (dev, ctx, tex);
    mutex->ReleaseSync (0);
    return px;
}

// the render device sampling the shared texture in a draw, the way WE's material passes see it: SRV made once up front,
// AcquireSync(0), draw into a render target, ReleaseSync(0), then the render target is read back
struct Sampler {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    Sampler (ID3D11Device* d, ID3D11DeviceContext* c, ID3D11Texture2D* shared) : dev (d), ctx (c)
    {
        static const char hlsl [] =
            "Texture2D t : register(t0); SamplerState s : register(s0);\n"
            "void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
            "  uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
            "float4 ps_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }\n";
        ID3DBlob* vsb = nullptr;
        ID3DBlob* psb = nullptr;
        ID3DBlob* err = nullptr;
        check ("D3DCompile vs", D3DCompile (hlsl, sizeof (hlsl) - 1, nullptr, nullptr, nullptr, "vs_main", "vs_4_0", 0, 0,
                                           &vsb, &err));
        check ("D3DCompile ps", D3DCompile (hlsl, sizeof (hlsl) - 1, nullptr, nullptr, nullptr, "ps_main", "ps_4_0", 0, 0,
                                           &psb, &err));
        dev->CreateVertexShader (vsb->GetBufferPointer (), vsb->GetBufferSize (), nullptr, &vs);
        dev->CreatePixelShader (psb->GetBufferPointer (), psb->GetBufferSize (), nullptr, &ps);
        check ("render CreateShaderResourceView", dev->CreateShaderResourceView (shared, nullptr, &srv));
        D3D11_TEXTURE2D_DESC desc;
        shared->GetDesc (&desc);
        desc.MiscFlags = 0;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        dev->CreateTexture2D (&desc, nullptr, &target);
        dev->CreateRenderTargetView (target, nullptr, &rtv);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState (&sd, &sampler);
    }

    std::vector<uint8_t> draw (IDXGIKeyedMutex* mutex)
    {
        HRESULT hr = mutex->AcquireSync (0, 1000);
        if (hr != S_OK)
            printf ("  render AcquireSync hr=0x%08lx\n", (unsigned long) hr);
        D3D11_VIEWPORT vp = {0, 0, (float) W, (float) H, 0, 1};
        ctx->OMSetRenderTargets (1, &rtv, nullptr);
        ctx->RSSetViewports (1, &vp);
        ctx->IASetInputLayout (nullptr);
        ctx->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader (vs, nullptr, 0);
        ctx->PSSetShader (ps, nullptr, 0);
        ctx->PSSetShaderResources (0, 1, &srv);
        ctx->PSSetSamplers (0, 1, &sampler);
        ctx->Draw (3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources (0, 1, &none);
        mutex->ReleaseSync (0);
        return readBack (dev, ctx, target);
    }
};

// WE's media engine extension (vtable 0x140487280): CanPlayType says PROBABLY, BeginCreateObject resolves the byte
// stream with MF_RESOLUTION_MEDIASOURCE | CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE (17) and hands back a
// wrapper source (vtable 0x1404872F8) whose CreatePresentationDescriptor deselects the audio streams
struct SourceWrapper : IMFMediaSourceEx {
    LONG refs = 1;
    IMFMediaSourceEx* inner;
    explicit SourceWrapper (IMFMediaSourceEx* s) : inner (s) {}
    STDMETHODIMP QueryInterface (REFIID riid, void** out) override
    {
        if (riid == __uuidof (IUnknown) || riid == __uuidof (IMFMediaEventGenerator) ||
            riid == __uuidof (IMFMediaSource) || riid == __uuidof (IMFMediaSourceEx)) {
            *out = this;
            AddRef ();
            return S_OK;
        }
        return inner->QueryInterface (riid, out);
    }
    STDMETHODIMP_ (ULONG) AddRef () override { return InterlockedIncrement (&refs); }
    STDMETHODIMP_ (ULONG) Release () override
    {
        ULONG r = InterlockedDecrement (&refs);
        if (!r) {
            inner->Release ();
            delete this;
        }
        return r;
    }
    STDMETHODIMP GetEvent (DWORD f, IMFMediaEvent** e) override { return inner->GetEvent (f, e); }
    STDMETHODIMP BeginGetEvent (IMFAsyncCallback* c, IUnknown* st) override { return inner->BeginGetEvent (c, st); }
    STDMETHODIMP EndGetEvent (IMFAsyncResult* r, IMFMediaEvent** e) override { return inner->EndGetEvent (r, e); }
    STDMETHODIMP QueueEvent (MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) override
    {
        return inner->QueueEvent (t, g, hr, v);
    }
    STDMETHODIMP GetCharacteristics (DWORD* c) override { return inner->GetCharacteristics (c); }
    STDMETHODIMP CreatePresentationDescriptor (IMFPresentationDescriptor** pd) override
    {
        HRESULT hr = inner->CreatePresentationDescriptor (pd);
        if (FAILED (hr))
            return hr;
        DWORD count = 0;
        (*pd)->GetStreamDescriptorCount (&count);
        for (DWORD i = 0; i < count; i++) {
            BOOL selected = FALSE;
            IMFStreamDescriptor* sd = nullptr;
            (*pd)->GetStreamDescriptorByIndex (i, &selected, &sd);
            IMFMediaTypeHandler* handler = nullptr;
            GUID major = {};
            sd->GetMediaTypeHandler (&handler);
            handler->GetMajorType (&major);
            handler->Release ();
            sd->Release ();
            if (major == MFMediaType_Audio)
                (*pd)->DeselectStream (i);
        }
        return S_OK;
    }
    STDMETHODIMP Start (IMFPresentationDescriptor* pd, const GUID* f, const PROPVARIANT* p) override
    {
        return inner->Start (pd, f, p);
    }
    STDMETHODIMP Stop () override { return inner->Stop (); }
    STDMETHODIMP Pause () override { return inner->Pause (); }
    STDMETHODIMP Shutdown () override { return inner->Shutdown (); }
    STDMETHODIMP GetSourceAttributes (IMFAttributes** a) override { return inner->GetSourceAttributes (a); }
    STDMETHODIMP GetStreamAttributes (DWORD id, IMFAttributes** a) override { return inner->GetStreamAttributes (id, a); }
    STDMETHODIMP SetD3DManager (IUnknown* m) override { return inner->SetD3DManager (m); }
};

struct Extension : IMFMediaEngineExtension {
    LONG refs = 1;
    STDMETHODIMP QueryInterface (REFIID riid, void** out) override
    {
        if (riid == __uuidof (IUnknown) || riid == __uuidof (IMFMediaEngineExtension)) {
            *out = this;
            AddRef ();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_ (ULONG) AddRef () override { return InterlockedIncrement (&refs); }
    STDMETHODIMP_ (ULONG) Release () override { return InterlockedDecrement (&refs); }
    STDMETHODIMP CanPlayType (BOOL, BSTR, MF_MEDIA_ENGINE_CANPLAY* answer) override
    {
        *answer = MF_MEDIA_ENGINE_CANPLAY_PROBABLY;
        return S_OK;
    }
    STDMETHODIMP BeginCreateObject (BSTR url, IMFByteStream* stream, MF_OBJECT_TYPE, IUnknown** cancel,
                                    IMFAsyncCallback* callback, IUnknown* state) override
    {
        printf ("  extension BeginCreateObject url=%ls stream=%p\n", url ? url : L"(null)", (void*) stream);
        if (cancel)
            *cancel = nullptr;
        if (!stream)
            return E_INVALIDARG;
        IMFSourceResolver* resolver = nullptr;
        HRESULT hr = MFCreateSourceResolver (&resolver);
        IUnknown* object = nullptr;
        IUnknown* wrapper = nullptr;
        if (SUCCEEDED (hr)) {
            stream->SetCurrentPosition (0);
            MF_OBJECT_TYPE type;
            hr = resolver->CreateObjectFromByteStream (stream, url, 17, nullptr, &type, &object);
            check ("  extension CreateObjectFromByteStream", hr);
        }
        if (SUCCEEDED (hr)) {
            IMFMediaSourceEx* source = nullptr;
            hr = object->QueryInterface (__uuidof (IMFMediaSourceEx), (void**) &source);
            check ("  extension QI IMFMediaSourceEx", hr);
            if (SUCCEEDED (hr))
                wrapper = new SourceWrapper (source);
        }
        if (SUCCEEDED (hr)) {
            IMFAsyncResult* result = nullptr;
            hr = MFCreateAsyncResult (wrapper, callback, state, &result);
            if (SUCCEEDED (hr)) {
                hr = MFInvokeCallback (result);
                result->Release ();
            }
        }
        if (wrapper)
            wrapper->Release ();
        if (object)
            object->Release ();
        if (resolver)
            resolver->Release ();
        return hr;
    }
    STDMETHODIMP CancelObjectCreation (IUnknown*) override { return E_NOTIMPL; }
    STDMETHODIMP EndCreateObject (IMFAsyncResult* result, IUnknown** object) override
    {
        *object = nullptr;
        HRESULT hr = result->GetStatus ();
        if (SUCCEEDED (hr))
            hr = result->GetObject (object);
        return hr;
    }
};

struct Notify : IMFMediaEngineNotify {
    LONG refs = 1;
    HANDLE loaded = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    STDMETHODIMP QueryInterface (REFIID riid, void** out) override
    {
        if (riid == __uuidof (IUnknown) || riid == __uuidof (IMFMediaEngineNotify)) {
            *out = this;
            AddRef ();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_ (ULONG) AddRef () override { return InterlockedIncrement (&refs); }
    STDMETHODIMP_ (ULONG) Release () override { return InterlockedDecrement (&refs); }
    STDMETHODIMP EventNotify (DWORD event, DWORD_PTR p1, DWORD p2) override
    {
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
            printf ("  event ERROR %lu hr=0x%08lx\n", (unsigned long) p1, (unsigned long) p2);
        else if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || event == MF_MEDIA_ENGINE_EVENT_ENDED ||
                 event == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE || event == MF_MEDIA_ENGINE_EVENT_PLAYING)
            printf ("  event %lu\n", (unsigned long) event);
        if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || event == MF_MEDIA_ENGINE_EVENT_ERROR)
            SetEvent (loaded);
        fflush (stdout);
        return S_OK;
    }
};

static int watchShared (HANDLE handle, double seconds)
{
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    check ("D3D11CreateDevice", D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    if (!dev)
        return 1;
    ID3D11Texture2D* tex = nullptr;
    check ("OpenSharedResource", dev->OpenSharedResource (handle, __uuidof (ID3D11Texture2D), (void**) &tex));
    if (!tex)
        return 1;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc (&desc);
    printf ("  %ux%u format %d bind %#x misc %#x\n", desc.Width, desc.Height, desc.Format, desc.BindFlags,
            desc.MiscFlags);
    IDXGIKeyedMutex* mutex = nullptr;
    tex->QueryInterface (__uuidof (IDXGIKeyedMutex), (void**) &mutex);
    for (int i = 0; i < seconds * 4; i++) {
        HRESULT hr = mutex ? mutex->AcquireSync (0, 100) : S_OK;
        std::vector<uint8_t> px = readBack (dev, ctx, tex);
        if (mutex && hr == S_OK)
            mutex->ReleaseSync (0);
        printf ("t=%.2f acquire hr=0x%08lx", i * 0.25, (unsigned long) hr);
        describe ("", px);
        fflush (stdout);
        Sleep (250);
    }
    return 0;
}

int main (int argc, char** argv)
{
    if (argc > 2 && !strcmp (argv [1], "--open"))
        return watchShared ((HANDLE) (uintptr_t) strtoull (argv [2], nullptr, 0), argc > 3 ? atof (argv [3]) : 5.0);
    if (argc < 2) {
        printf ("usage: sharedvideo.exe <video> [seconds]\n");
        return 2;
    }
    double seconds = argc > 2 ? atof (argv [2]) : 2.0;
    g_flush = getenv ("SV_FLUSH") != nullptr;
    g_playerRead = getenv ("SV_PLAYERREAD") != nullptr;
    g_we = getenv ("SV_WE") != nullptr;
    g_thread = getenv ("SV_THREAD") != nullptr;
    InitializeCriticalSection (&g_lock);
    printf ("flush %d player read %d we load %d\n", g_flush, g_playerRead, g_we);
    D3D_FEATURE_LEVEL levels [] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0};

    // sub_14012AC60: WE's render device is VIDEO_SUPPORT (0x800) when the renderer flag at +22 is set, otherwise
    // SINGLETHREADED (1); SV_RFLAGS overrides, default is WE's SINGLETHREADED
    UINT renderFlags = getenv ("SV_RFLAGS") ? strtoul (getenv ("SV_RFLAGS"), nullptr, 0) : D3D11_CREATE_DEVICE_SINGLETHREADED;
    printf ("render device flags 0x%x\n", renderFlags);
    ID3D11Device* rdev = nullptr;
    ID3D11DeviceContext* rctx = nullptr;
    check ("render D3D11CreateDevice",
           D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, renderFlags, levels, 4,
                              D3D11_SDK_VERSION, &rdev, nullptr, &rctx));
    ID3D11Device* pdev = nullptr;
    ID3D11DeviceContext* pctx = nullptr;
    check ("player D3D11CreateDevice (VIDEO_SUPPORT)",
           D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 4,
                              D3D11_SDK_VERSION, &pdev, nullptr, &pctx));
    if (!rdev || !pdev)
        return 1;

    // render side texture: WE fills it with zeros, bind flags 0x28, misc 0x100
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = W;
    desc.Height = H;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // live WE (gdb_video_texdesc.py, sub_1400EC220): R8G8B8A8_UNORM, while the media engine outputs B8G8R8A8;
    // SV_FORMAT overrides
    desc.Format = getenv ("SV_FORMAT") ? (DXGI_FORMAT) atoi (getenv ("SV_FORMAT")) : DXGI_FORMAT_R8G8B8A8_UNORM;
    printf ("texture format %d\n", desc.Format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    std::vector<uint8_t> zeros (W * H * 4, 0);
    D3D11_SUBRESOURCE_DATA init = {zeros.data (), W * 4, 0};
    ID3D11Texture2D* rtex = nullptr;
    check ("render CreateTexture2D (KEYEDMUTEX)", rdev->CreateTexture2D (&desc, &init, &rtex));
    if (!rtex)
        return 1;
    IDXGIKeyedMutex* rmutex = nullptr;
    check ("render QI IDXGIKeyedMutex", rtex->QueryInterface (__uuidof (IDXGIKeyedMutex), (void**) &rmutex));
    IDXGIResource* res = nullptr;
    HANDLE shared = nullptr;
    rtex->QueryInterface (__uuidof (IDXGIResource), (void**) &res);
    check ("render GetSharedHandle", res->GetSharedHandle (&shared));
    printf ("  handle %p\n", shared);

    ID3D11Texture2D* ptex = nullptr;
    check ("player OpenSharedResource", pdev->OpenSharedResource (shared, __uuidof (ID3D11Texture2D), (void**) &ptex));
    if (!ptex || !rmutex)
        return 1;
    IDXGIKeyedMutex* pmutex = nullptr;
    check ("player QI IDXGIKeyedMutex", ptex->QueryInterface (__uuidof (IDXGIKeyedMutex), (void**) &pmutex));

    Sampler sampler (rdev, rctx, rtex);

    printf ("\n[A] plain D3D11 writes on the player device\n");
    describe ("render initial", readOnRender (rdev, rctx, rtex, rmutex));
    {
        ID3D11RenderTargetView* rtv = nullptr;
        check ("player CreateRenderTargetView", pdev->CreateRenderTargetView (ptex, nullptr, &rtv));
        check ("player AcquireSync(0)", pmutex->AcquireSync (0, 1000));
        const float red [4] = {1, 0, 0, 1};
        pctx->ClearRenderTargetView (rtv, red);
        if (g_flush)
            pctx->Flush ();
        check ("player ReleaseSync(0)", pmutex->ReleaseSync (0));
        rtv->Release ();
        describe ("render after clear red", readOnRender (rdev, rctx, rtex, rmutex));
    }
    {
        std::vector<uint8_t> green (W * H * 4);
        for (size_t i = 0; i < green.size (); i += 4)
            green [i] = 0, green [i + 1] = 255, green [i + 2] = 0, green [i + 3] = 255;
        check ("player AcquireSync(0)", pmutex->AcquireSync (0, 1000));
        pctx->UpdateSubresource (ptex, 0, nullptr, green.data (), W * 4, 0);
        if (g_flush)
            pctx->Flush ();
        check ("player ReleaseSync(0)", pmutex->ReleaseSync (0));
        describe ("render after upload green", readOnRender (rdev, rctx, rtex, rmutex));
        describe ("render sampled green", sampler.draw (rmutex));
    }
    {
        check ("player AcquireSync(0)", pmutex->AcquireSync (0, 1000));
        describe ("player own view", readBack (pdev, pctx, ptex));
        pmutex->ReleaseSync (0);
    }

    printf ("\n[B] media engine on the player device\n");
    check ("MFStartup", MFStartup (MF_VERSION, MFSTARTUP_LITE));
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED (pdev->QueryInterface (__uuidof (ID3D10Multithread), (void**) &mt))) {
        mt->SetMultithreadProtected (TRUE);
        mt->Release ();
    }
    UINT token = 0;
    IMFDXGIDeviceManager* manager = nullptr;
    check ("MFCreateDXGIDeviceManager", MFCreateDXGIDeviceManager (&token, &manager));
    check ("ResetDevice", manager->ResetDevice (pdev, token));
    IMFMediaEngineClassFactory* factory = nullptr;
    check ("CoInitializeEx", CoInitializeEx (nullptr, COINIT_MULTITHREADED));
    check ("CoCreateInstance MediaEngineClassFactory",
           CoCreateInstance (CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_ALL, IID_PPV_ARGS (&factory)));
    if (!factory)
        return 1;
    Notify* notify = new Notify;
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes (&attrs, 3);
    attrs->SetUnknown (MF_MEDIA_ENGINE_CALLBACK, notify);
    attrs->SetUnknown (MF_MEDIA_ENGINE_DXGI_MANAGER, manager);
    attrs->SetUINT32 (MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (g_we)
        attrs->SetUnknown (MF_MEDIA_ENGINE_EXTENSION, new Extension);
    IMFMediaEngine* engine = nullptr;
    // WE: (4 * !audio) | 0x12 = FORCEMUTE | WAITFORSTABLE_STATE | DISABLE_LOCAL_PLUGINS
    check ("CreateInstance (0x16)", factory->CreateInstance (0x16, attrs, &engine));
    if (!engine)
        return 1;
    IMFMediaEngineEx* engineEx = nullptr;
    check ("QI IMFMediaEngineEx", engine->QueryInterface (__uuidof (IMFMediaEngineEx), (void**) &engineEx));
    engine->SetAutoPlay (FALSE);
    engine->SetLoop (TRUE);

    wchar_t wpath [MAX_PATH];
    MultiByteToWideChar (CP_UTF8, 0, argv [1], -1, wpath, MAX_PATH);
    wchar_t full [MAX_PATH];
    GetFullPathNameW (wpath, MAX_PATH, full, nullptr);
    if (g_we) {
        // sub_1400F28E0: source reader on the byte stream for size/fps first, back to 0, then the engine gets the same
        // stream under the name "video"
        IMFByteStream* stream = nullptr;
        check ("MFCreateFile",
               MFCreateFile (MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, full, &stream));
        IMFSourceReader* reader = nullptr;
        check ("MFCreateSourceReaderFromByteStream", MFCreateSourceReaderFromByteStream (stream, nullptr, &reader));
        if (reader) {
            IMFMediaType* type = nullptr;
            reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
            check ("reader GetCurrentMediaType", reader->GetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type));
            if (type) {
                UINT32 fw = 0, fh = 0, num = 0, den = 0;
                MFGetAttributeSize (type, MF_MT_FRAME_SIZE, &fw, &fh);
                MFGetAttributeRatio (type, MF_MT_FRAME_RATE, &num, &den);
                printf ("  reader %ux%u fps %.3f\n", fw, fh, den ? (double) num / den : 0.0);
                type->Release ();
            }
            reader->Release ();
        }
        stream->SetCurrentPosition (0);
        BSTR name = SysAllocString (L"video");
        check ("SetSourceFromByteStream", engineEx->SetSourceFromByteStream (stream, name));
    } else {
        BSTR url = SysAllocString (full);
        check ("SetSource", engine->SetSource (url));
    }
    WaitForSingleObject (notify->loaded, 5000);
    DWORD vw = 0, vh = 0;
    engine->GetNativeVideoSize (&vw, &vh);
    printf ("  native size %lux%lu duration %.3f\n", (unsigned long) vw, (unsigned long) vh, engine->GetDuration ());
    check ("Play", engine->Play ());

    // a texture that only the player device knows, to tell a media engine that writes nothing apart from a share
    // that drops the write
    D3D11_TEXTURE2D_DESC ldesc = desc;
    ldesc.MiscFlags = 0;
    ID3D11Texture2D* ltex = nullptr;
    check ("player local CreateTexture2D", pdev->CreateTexture2D (&ldesc, nullptr, &ltex));

    MFVideoNormalizedRect src = {0, 0, 1, 1};
    RECT dst = {0, 0, (LONG) W, (LONG) H};
    MFARGB border = {0, 0, 0, 255};
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency (&freq);
    QueryPerformanceCounter (&t0);
    int ticks = 0, shown = 0;
    g_engine = engine;
    if (g_thread) {
        CreateThread (nullptr, 0, tickThread, nullptr, 0, nullptr);
        for (;;) {
            QueryPerformanceCounter (&now);
            double t = (double) (now.QuadPart - t0.QuadPart) / freq.QuadPart;
            if (t > seconds)
                break;
            if (g_newFrame) {
                EnterCriticalSection (&g_lock);
                HRESULT ahr = pmutex->AcquireSync (0, 1000);
                HRESULT shr = engine->TransferVideoFrame (ptex, &src, &dst, &border);
                pmutex->ReleaseSync (0);
                g_newFrame = 0;
                LeaveCriticalSection (&g_lock);
                ticks++;
                if (shown < 6 || ticks % 15 == 0) {
                    shown++;
                    printf ("t=%.3f cur=%.3f acquire hr=0x%08lx shared hr=0x%08lx\n", t, engine->GetCurrentTime (),
                            (unsigned long) ahr, (unsigned long) shr);
                    describe ("render sampled", sampler.draw (rmutex));
                }
            }
            Sleep (16);
        }
        g_run = 0;
        Sleep (50);
    }
    for (; !g_thread;) {
        QueryPerformanceCounter (&now);
        double t = (double) (now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (t > seconds)
            break;
        LONGLONG pts = 0;
        HRESULT hr = engine->OnVideoStreamTick (&pts);
        if (hr == S_OK) {
            ticks++;
            bool print = shown < 6 || ticks % 15 == 0;
            HRESULT lhr = engine->TransferVideoFrame (ltex, &src, &dst, &border);
            HRESULT ahr = pmutex->AcquireSync (0, 1000);
            HRESULT shr = engine->TransferVideoFrame (ptex, &src, &dst, &border);
            if (g_flush)
                pctx->Flush ();
            pmutex->ReleaseSync (0);
            if (print) {
                shown++;
                printf ("t=%.3f pts=%.3f cur=%.3f local hr=0x%08lx acquire hr=0x%08lx shared hr=0x%08lx\n", t,
                        pts / 1e7, engine->GetCurrentTime (), (unsigned long) lhr, (unsigned long) ahr,
                        (unsigned long) shr);
                if (g_playerRead) {
                    describe ("player local", readBack (pdev, pctx, ltex));
                    pmutex->AcquireSync (0, 1000);
                    describe ("player shared", readBack (pdev, pctx, ptex));
                    pmutex->ReleaseSync (0);
                }
                describe ("render shared", readOnRender (rdev, rctx, rtex, rmutex));
                describe ("render sampled", sampler.draw (rmutex));
                fflush (stdout);
            }
        }
        Sleep (16);
    }
    IMFMediaError* error = nullptr;
    engine->GetError (&error);
    printf ("\n%d new frames in %.1f s, ended %d, error %d\n", ticks, seconds, engine->IsEnded (),
            error ? error->GetErrorCode () : 0);
    engine->Shutdown ();
    return 0;
}
