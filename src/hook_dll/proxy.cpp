// NCMExclusive - 核心：COM 代理链 + 强制 WASAPI 独占模式
//
// 链路：CoCreateInstance(CLSID_MMDeviceEnumerator) 被 IAT 钩子拦截
//   -> EnumeratorProxy（包装 GetDefaultAudioEndpoint / GetDevice 返回的设备）
//   -> DeviceProxy::Activate(IAudioClient) -> ClientProxy
//   -> ClientProxy::Initialize：把宿主的共享模式请求改成独占模式
//
// 独占初始化要点（参考 AMExclusive / 微软 WASAPIRenderer 示例，MIT）：
//   1. IsFormatSupported(EXCLUSIVE) 预检，不支持则不强制（自动回退共享）
//   2. 独占必须带 AUDCLNT_STREAMFLAGS_EVENTCALLBACK；periodicity 必须非零
//   3. AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED 时按 GetBufferSize 换算周期，
//      并重新 Activate 一个全新客户端再重试（同一客户端只能 Initialize 一次）
//   4. 任何失败都静默回退到原始共享模式调用，保证播放不出问题
//
// 约定：COM 回调首参数一律用宿主视角的接口指针类型声明，函数体内
//       reinterpret_cast 回代理结构（布局兼容，首成员即 vtbl）。
#include "proxy.h"
#include "software_terminal.h"

#include "../common/wasapi_min.h"
#include "../common/iat_hook.h"
#include "../common/util.h"

#include <cstdint>
#include <cstdio>
#include <new>

namespace ncmex {
namespace {

constexpr reference_time_t kHnsPerSecond = 10000000;

// ---------------------------------------------------------------------------
// 代理对象
// ---------------------------------------------------------------------------

struct DeviceProxy;
struct ClientProxy;

struct EnumeratorProxy {
    const void* vtbl;
    IUnknown* inner;
    volatile LONG ref;
};

struct CollectionProxy {
    const void* vtbl;
    IUnknown* inner;
    int flow;
    volatile LONG ref;
};

struct DeviceProxy {
    const void* vtbl;
    IUnknown* inner;
    int flow;
    volatile LONG ref;
};

struct ClientProxy {
    const void* vtbl;
    IUnknown* inner;
    DeviceProxy* device;
    volatile LONG ref;
    int version;
    int mode;  // 0=共享直通 1=原格式独占 2=独占(pcm32)
    UINT32 channels;
    bool started;
    UINT32 blockAlign;
    HANDLE hostEvent;
    std::shared_ptr<SoftwareState> software;
};

struct RenderProxy {
    const void* vtbl;
    IUnknown* inner;
    volatile LONG ref;
    UINT32 channels;
    BYTE* lastData;
    int mode;
    LONG logRemaining;
    std::shared_ptr<SoftwareState> software;
};

extern const IMMDeviceEnumeratorVtbl_ kEnumVtbl;
extern const IMMDeviceCollectionVtbl_ kCollectionVtbl;
extern const IMMDeviceVtbl_ kDeviceVtbl;
extern const IAudioClientVtbl_ kClientVtbl;
extern const IAudioClient2Vtbl_ kClient2Vtbl;
extern const IAudioClient3Vtbl_ kClient3Vtbl;
extern const IAudioRenderClientVtbl_ kRenderVtbl;

HRESULT STDMETHODCALLTYPE Render_QI(IAudioRenderClient_* iface, REFIID riid, void** ppv) {
    auto* self = reinterpret_cast<RenderProxy*>(iface);
    if (riid == IID_IAudioRenderClient_ || riid == IID_IUnknown) {
        InterlockedIncrement(&self->ref);
        *ppv = self;
        return S_OK;
    }
    return self->inner ? self->inner->QueryInterface(riid, ppv) : E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE Render_AddRef(IAudioRenderClient_* iface) {
    auto* self = reinterpret_cast<RenderProxy*>(iface);
    return InterlockedIncrement(&self->ref);
}
ULONG STDMETHODCALLTYPE Render_Release(IAudioRenderClient_* iface) {
    auto* self = reinterpret_cast<RenderProxy*>(iface);
    const ULONG r = InterlockedDecrement(&self->ref);
    if (r == 0) {
        if (self->inner) self->inner->Release();
        self->software.reset();
        delete self;
    }
    return r;
}
HRESULT STDMETHODCALLTYPE Render_GetBuffer(IAudioRenderClient_* iface, UINT32 frames,
                                           BYTE** data) {
    auto* self = reinterpret_cast<RenderProxy*>(iface);
    HRESULT hr = E_UNEXPECTED;
    if (self->software) {
        hr = self->software->GetBuffer(frames, data);
    } else if (self->inner) {
        IAudioRenderClient_* inner = reinterpret_cast<IAudioRenderClient_*>(self->inner);
        hr = inner->lpVtbl->GetBuffer(inner, frames, data);
        if (SUCCEEDED(hr)) self->lastData = *data;
    }
    if (self->logRemaining > 0) {
        --self->logRemaining;
        Log(L"[NCMEx] Render_GetBuffer frames=%u hr=0x%08lX mode=%d", frames,
            static_cast<unsigned long>(hr), self->mode);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Render_ReleaseBuffer(IAudioRenderClient_* iface, UINT32 frames,
                                               DWORD flags) {
    auto* self = reinterpret_cast<RenderProxy*>(iface);
    HRESULT hr = E_UNEXPECTED;
    if (self->software) {
        hr = self->software->ReleaseBuffer(frames, flags);
    } else if (self->inner) {
        IAudioRenderClient_* inner = reinterpret_cast<IAudioRenderClient_*>(self->inner);
        if (self->mode == 2 && self->lastData && frames && !(flags & 0x1)) {
            float* f = reinterpret_cast<float*>(self->lastData);
            int32_t* i = reinterpret_cast<int32_t*>(self->lastData);
            const size_t count = static_cast<size_t>(frames) * self->channels;
            for (size_t k = 0; k < count; ++k) {
                const double v = f[k];
                if (v >= 1.0) {
                    i[k] = 0x7FFFFF00;
                } else if (v <= -1.0) {
                    i[k] = INT32_MIN;
                } else {
                    i[k] = static_cast<int32_t>(v * 2147483648.0) & ~0xFF;
                }
            }
        }
        self->lastData = nullptr;
        hr = inner->lpVtbl->ReleaseBuffer(inner, frames, flags);
    }
    if (self->logRemaining > 0) {
        --self->logRemaining;
        Log(L"[NCMEx] Render_ReleaseBuffer frames=%u flags=0x%lx hr=0x%08lX", frames, flags,
            static_cast<unsigned long>(hr));
    }
    return hr;
}

// ---------------------------------------------------------------------------
// ClientProxy
// ---------------------------------------------------------------------------

void TraceCallers(const wchar_t* tag) {
    void* frames[12] = {};
    const USHORT count = CaptureStackBackTrace(0, 12, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(frames[i]), &module))
            continue;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        if (_wcsicmp(name, L"cloudmusic.dll") != 0 && _wcsicmp(name, L"cloudmusic.exe") != 0 &&
            _wcsicmp(name, L"libcef.dll") != 0)
            continue;
        const auto offset = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module);
        Log(L"[NCMEx] stack %ls [%u] %ls+0x%llX", tag, i, name,
            static_cast<unsigned long long>(offset));
    }
}

IAudioClient_* AsClient(IUnknown* u) { return reinterpret_cast<IAudioClient_*>(u); }
IAudioClient2_* AsClient2(IUnknown* u) { return reinterpret_cast<IAudioClient2_*>(u); }
IAudioClient3_* AsClient3(IUnknown* u) { return reinterpret_cast<IAudioClient3_*>(u); }

const IID& ClientIid(const ClientProxy* self) {
    if (self->version >= 3) return IID_IAudioClient3_;
    if (self->version == 2) return IID_IAudioClient2_;
    return IID_IAudioClient_;
}

HRESULT ReactivateClient(ClientProxy* self) {
    void* p = nullptr;
    IMMDevice_* dev = reinterpret_cast<IMMDevice_*>(self->device->inner);
    const HRESULT hr = dev->lpVtbl->Activate(dev, ClientIid(self), CLSCTX_INPROC_SERVER,
                                             nullptr, &p);
    if (SUCCEEDED(hr)) {
        if (self->software) self->software->Stop();
        self->software.reset();
        if (self->inner) self->inner->Release();
        self->inner = static_cast<IUnknown*>(p);
        self->started = false;
    }
    return hr;
}

HRESULT TryInitializeExclusive(ClientProxy* self, DWORD flags, reference_time_t bufferDuration,
                               const WaveFormatEx_* fmt, const GUID* sessionGuid, bool pcm32) {
    if (!fmt || !fmt->nSamplesPerSec || !fmt->nChannels) return E_INVALIDARG;
    HRESULT hr = ReactivateClient(self);
    if (FAILED(hr)) return hr;
    IAudioClient_* client = AsClient(self->inner);

    reference_time_t defaultPeriod = 0, minPeriod = 0;
    if (SUCCEEDED(client->lpVtbl->GetDevicePeriod(client, &defaultPeriod, &minPeriod))) {
        if (bufferDuration <= 0) bufferDuration = defaultPeriod;
        if (minPeriod > 0 && bufferDuration < minPeriod) bufferDuration = minPeriod;
    }
    if (bufferDuration <= 0) bufferDuration = 200000;

    const WaveFormatEx_* useFmt = fmt;
    WaveFormatExtensible_ pcmFmt{};
    if (pcm32) {
        const UINT32 ch = fmt->nChannels;
        pcmFmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE_;
        pcmFmt.Format.nChannels = static_cast<WORD>(ch);
        pcmFmt.Format.nSamplesPerSec = fmt->nSamplesPerSec;
        pcmFmt.Format.nBlockAlign = static_cast<WORD>(ch * 4);
        pcmFmt.Format.nAvgBytesPerSec = fmt->nSamplesPerSec * ch * 4;
        pcmFmt.Format.wBitsPerSample = 32;
        pcmFmt.Format.cbSize =
            static_cast<WORD>(sizeof(WaveFormatExtensible_) - sizeof(WaveFormatEx_));
        pcmFmt.Samples.wValidBitsPerSample = 24;
        DWORD mask = 0;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE_ && fmt->cbSize >= 22) {
            mask = reinterpret_cast<const WaveFormatExtensible_*>(fmt)->dwChannelMask;
        } else if (ch == 1) {
            mask = 0x4;
        } else if (ch == 2) {
            mask = 0x3;
        }
        pcmFmt.dwChannelMask = mask;
        pcmFmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM_;
        useFmt = &pcmFmt.Format;
    }

    hr = client->lpVtbl->IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE_, useFmt, nullptr);
    if (hr == S_FALSE) hr = AUDCLNT_E_UNSUPPORTED_FORMAT_;
    if (FAILED(hr)) return hr;

    const DWORD exFlags =
        (flags & (AUDCLNT_STREAMFLAGS_EVENTCALLBACK_ | AUDCLNT_STREAMFLAGS_NOPERSIST_)) |
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK_;

    hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE_, exFlags, bufferDuration,
                                    bufferDuration, useFmt, sessionGuid);
    if (SUCCEEDED(hr)) {
        self->mode = pcm32 ? 2 : 1;
        self->channels = useFmt->nChannels;
        self->blockAlign = useFmt->nBlockAlign;
        return S_OK;
    }
    if (hr != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED_) return hr;

    UINT32 frames = 0;
    const HRESULT sizeHr = client->lpVtbl->GetBufferSize(client, &frames);
    if (FAILED(sizeHr) || frames == 0) return FAILED(sizeHr) ? sizeHr : E_FAIL;
    const reference_time_t period = static_cast<reference_time_t>(
        (static_cast<uint64_t>(frames) * kHnsPerSecond + useFmt->nSamplesPerSec / 2) /
        useFmt->nSamplesPerSec);
    hr = ReactivateClient(self);
    if (FAILED(hr)) return hr;
    client = AsClient(self->inner);
    hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE_, exFlags, period,
                                    period, useFmt, sessionGuid);
    if (FAILED(hr)) return hr;
    self->mode = pcm32 ? 2 : 1;
    self->channels = useFmt->nChannels;
    self->blockAlign = useFmt->nBlockAlign;
    return S_OK;
}

void BalloonNotify();
ULONG STDMETHODCALLTYPE Device_Release(IMMDevice_* iface);

HRESULT STDMETHODCALLTYPE Client_QI(IAudioClient_* iface, REFIID riid, void** ppv) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    Log(L"[NCMEx] Client_QI enter riid=%08lX-%04X-%04X version=%d", riid.Data1, riid.Data2,
        riid.Data3, self->version);
    if (riid == IID_IAudioClient_ || riid == IID_IUnknown) {
        InterlockedIncrement(&self->ref);
        *ppv = self;
        return S_OK;
    }

    const int requested = riid == IID_IAudioClient3_ ? 3 : (riid == IID_IAudioClient2_ ? 2 : 1);
    if (requested > 1) {
        if (self->version >= requested) {
            InterlockedIncrement(&self->ref);
            *ppv = self;
            return S_OK;
        }
        void* upgraded = nullptr;
        const HRESULT hr = self->inner->QueryInterface(riid, &upgraded);
        if (FAILED(hr)) {
            Log(L"[NCMEx] Client_QI upgrade exit hr=0x%08lX", static_cast<unsigned long>(hr));
            return hr;
        }
        self->inner->Release();
        self->inner = static_cast<IUnknown*>(upgraded);
        self->version = requested;
        self->vtbl = requested == 3 ? static_cast<const void*>(&kClient3Vtbl)
                                    : static_cast<const void*>(&kClient2Vtbl);
        InterlockedIncrement(&self->ref);
        *ppv = self;
        Log(L"[NCMEx] Client_QI upgraded to IAudioClient%d", requested);
        return S_OK;
    }

    const HRESULT hr = self->inner->QueryInterface(riid, ppv);
    Log(L"[NCMEx] Client_QI other exit hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}
ULONG STDMETHODCALLTYPE Client_AddRef(IAudioClient_* iface) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    return InterlockedIncrement(&self->ref);
}
ULONG STDMETHODCALLTYPE Client_Release(IAudioClient_* iface) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    const ULONG r = InterlockedDecrement(&self->ref);
    Log(L"[NCMEx] Client_Release remaining=%u mode=%d", r, self->mode);
    if (r == 0) {
        if (self->software) self->software->Stop();
        self->software.reset();
        self->inner->Release();
        Device_Release(reinterpret_cast<IMMDevice_*>(self->device));
        delete self;
    }
    return r;
}

HRESULT STDMETHODCALLTYPE Client_Initialize(IAudioClient_* iface, int shareMode, DWORD streamFlags,
                                            reference_time_t bufferDuration,
                                            reference_time_t periodicity,
                                            const WaveFormatEx_* format,
                                            const GUID* sessionGuid) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_Initialize enter pid=%lu shareMode=%d tag=0x%x bits=%u ch=%u rate=%u",
        GetCurrentProcessId(), shareMode, format ? format->wFormatTag : 0,
        format ? format->wBitsPerSample : 0, format ? format->nChannels : 0,
        format ? format->nSamplesPerSec : 0);
    const Config cfg = LoadConfig();
    const bool isFloat =
        format &&
        ((format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT_) ||
         (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE_ && format->cbSize >= 22 &&
          reinterpret_cast<const WaveFormatExtensible_*>(format)->SubFormat ==
              KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_));

    if (!cfg.enable || shareMode != AUDCLNT_SHAREMODE_SHARED_ || !format) {
        self->mode = 0;
        const HRESULT hr = AsClient(self->inner)->lpVtbl->Initialize(
            AsClient(self->inner), shareMode, streamFlags, bufferDuration, periodicity, format,
            sessionGuid);
        Log(L"[NCMEx] Initialize 直通 exit hr=0x%08lX enable=%d", static_cast<unsigned long>(hr),
            cfg.enable);
        return hr;
    }

    HRESULT hr = TryInitializeExclusive(self, streamFlags, bufferDuration, format, sessionGuid,
                                        false);
    if (FAILED(hr) && cfg.pcm_fallback && isFloat) {
        Log(L"[NCMEx] 原格式独占失败 hr=0x%08lX，尝试 PCM32", static_cast<unsigned long>(hr));
        hr = TryInitializeExclusive(self, streamFlags, bufferDuration, format, sessionGuid, true);
    }
    if (SUCCEEDED(hr)) {
        Log(L"[NCMEx] 独占客户端初始化成功：%uHz %uch %dbit(%ls) 请求缓冲%lldms pid=%lu",
            format->nSamplesPerSec, format->nChannels,
            self->mode == 2 ? 24 : format->wBitsPerSample,
            self->mode == 2 ? L"PCM24/32" : (isFloat ? L"Float" : L"PCM"),
            bufferDuration / 10000, GetCurrentProcessId());
        return S_OK;
    }

    const HRESULT exclusiveHr = hr;
    self->mode = 0;
    hr = ReactivateClient(self);
    if (FAILED(hr)) {
        Log(L"[NCMEx] 独占不可用(0x%08lX)，共享客户端重建失败(0x%08lX)",
            static_cast<unsigned long>(exclusiveHr), static_cast<unsigned long>(hr));
        return hr;
    }
    hr = AsClient(self->inner)->lpVtbl->Initialize(
        AsClient(self->inner), shareMode, streamFlags, bufferDuration, periodicity, format,
        sessionGuid);
    Log(L"[NCMEx] 独占不可用(0x%08lX)，共享回退 exit hr=0x%08lX",
        static_cast<unsigned long>(exclusiveHr), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Client_GetService(IAudioClient_* iface, REFIID riid, void** service) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    if (!service) return E_POINTER;
    Log(L"[NCMEx] Client_GetService enter riid=%08lX mode=%d", riid.Data1, self->mode);
    const HRESULT hr = AsClient(self->inner)->lpVtbl->GetService(AsClient(self->inner), riid,
                                                                 service);
    if (FAILED(hr) || riid != IID_IAudioRenderClient_ || self->mode == 0) {
        Log(L"[NCMEx] Client_GetService exit hr=0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    if (!self->software) {
        auto* realRender = static_cast<IAudioRenderClient_*>(*service);
        UINT32 frames = 0;
        AsClient(self->inner)->lpVtbl->GetBufferSize(AsClient(self->inner), &frames);
        self->software = SoftwareState::Create(AsClient(self->inner), realRender, frames,
                                               self->blockAlign, self->channels, self->mode,
                                               self->hostEvent);
        if (!self->software) return E_FAIL;
    }
    auto* proxy = new (std::nothrow) RenderProxy{&kRenderVtbl, nullptr, 1, self->channels, nullptr,
                                                 self->mode, 30, self->software};
    if (!proxy) return E_OUTOFMEMORY;
    *service = proxy;
    Log(L"[NCMEx] Client_GetService exit hr=0x00000000 wrapped=software-mode%d", self->mode);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Client_GetBufferSize(IAudioClient_* iface, UINT32* size) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_GetBufferSize enter");
    const HRESULT hr = AsClient(self->inner)->lpVtbl->GetBufferSize(AsClient(self->inner), size);
    Log(L"[NCMEx] Client_GetBufferSize exit hr=0x%08lX frames=%u", static_cast<unsigned long>(hr),
        size ? *size : 0);
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_GetStreamLatency(IAudioClient_* iface, reference_time_t* latency) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_GetStreamLatency enter");
    const HRESULT hr = AsClient(self->inner)->lpVtbl->GetStreamLatency(AsClient(self->inner),
                                                                       latency);
    Log(L"[NCMEx] Client_GetStreamLatency exit hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_GetCurrentPadding(IAudioClient_* iface, UINT32* padding) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    if (!padding) return E_POINTER;
    if (self->software) {
        *padding = self->software->Padding();
        return S_OK;
    }
    if (self->mode != 0) {
        *padding = 0;
        return S_OK;
    }
    return AsClient(self->inner)->lpVtbl->GetCurrentPadding(AsClient(self->inner), padding);
}
HRESULT STDMETHODCALLTYPE Client_IsFormatSupported(IAudioClient_* iface, int shareMode,
                                                   const WaveFormatEx_* format,
                                                   WaveFormatEx_** closest) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_IsFormatSupported enter mode=%d", shareMode);
    const HRESULT hr = AsClient(self->inner)->lpVtbl->IsFormatSupported(AsClient(self->inner),
                                                                        shareMode, format, closest);
    Log(L"[NCMEx] Client_IsFormatSupported exit hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_GetMixFormat(IAudioClient_* iface, WaveFormatEx_** format) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_GetMixFormat enter");
    const HRESULT hr = AsClient(self->inner)->lpVtbl->GetMixFormat(AsClient(self->inner), format);
    Log(L"[NCMEx] Client_GetMixFormat exit hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_GetDevicePeriod(IAudioClient_* iface, reference_time_t* defPeriod,
                                                 reference_time_t* minPeriod) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_GetDevicePeriod enter inner=%p", self->inner);
    const HRESULT hr = AsClient(self->inner)->lpVtbl->GetDevicePeriod(AsClient(self->inner),
                                                                      defPeriod, minPeriod);
    Log(L"[NCMEx] Client_GetDevicePeriod exit hr=0x%08lX default=%lld min=%lld",
        static_cast<unsigned long>(hr), defPeriod ? *defPeriod : 0, minPeriod ? *minPeriod : 0);
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_Start(IAudioClient_* iface) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    static LONG traces = 0;
    if (InterlockedIncrement(&traces) <= 8) TraceCallers(L"Start");
    Log(L"[NCMEx] Client_Start enter mode=%d", self->mode);
    const HRESULT hr = self->software ? self->software->Start()
                                      : AsClient(self->inner)->lpVtbl->Start(AsClient(self->inner));
    Log(L"[NCMEx] Client_Start exit hr=0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) self->started = true;
    if (SUCCEEDED(hr) && self->software && self->mode != 0) {
        Log(L"[NCMEx] 独占模式已生效（播放已启动）pid=%lu", GetCurrentProcessId());
        BalloonNotify();
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_Stop(IAudioClient_* iface) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    static LONG traces = 0;
    if (InterlockedIncrement(&traces) <= 8) TraceCallers(L"Stop");
    Log(L"[NCMEx] Client_Stop enter");
    const HRESULT hr = self->software ? self->software->Stop()
                                      : AsClient(self->inner)->lpVtbl->Stop(AsClient(self->inner));
    self->started = false;
    Log(L"[NCMEx] Client_Stop exit hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_Reset(IAudioClient_* iface) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_Reset enter");
    const HRESULT hr = self->software ? self->software->Reset()
                                      : AsClient(self->inner)->lpVtbl->Reset(AsClient(self->inner));
    self->started = false;
    Log(L"[NCMEx] Client_Reset exit hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE Client_SetEventHandle(IAudioClient_* iface, HANDLE eventHandle) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client_SetEventHandle enter");
    if (self->mode != 0) {
        self->hostEvent = eventHandle;
        if (self->software) self->software->SetHostEvent(eventHandle);
        Log(L"[NCMEx] Client_SetEventHandle exit hr=0x00000000 software-event");
        return S_OK;
    }
    const HRESULT hr = AsClient(self->inner)->lpVtbl->SetEventHandle(AsClient(self->inner),
                                                                     eventHandle);
    Log(L"[NCMEx] Client_SetEventHandle exit hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Client2_IsOffloadCapable(IAudioClient2_* iface, int category,
                                                   BOOL* offloadCapable) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    IAudioClient2_* inner = AsClient2(self->inner);
    return inner->lpVtbl->IsOffloadCapable(inner, category, offloadCapable);
}
HRESULT STDMETHODCALLTYPE Client2_SetClientProperties(IAudioClient2_* iface,
                                                      const void* properties) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    IAudioClient2_* inner = AsClient2(self->inner);
    return inner->lpVtbl->SetClientProperties(inner, properties);
}
HRESULT STDMETHODCALLTYPE Client2_GetBufferSizeLimits(IAudioClient2_* iface,
                                                      const WaveFormatEx_* format, BOOL eventDriven,
                                                      reference_time_t* minBufferDuration,
                                                      reference_time_t* maxBufferDuration) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    IAudioClient2_* inner = AsClient2(self->inner);
    return inner->lpVtbl->GetBufferSizeLimits(inner, format, eventDriven, minBufferDuration,
                                              maxBufferDuration);
}
HRESULT STDMETHODCALLTYPE Client3_GetSharedModeEnginePeriod(
    IAudioClient3_* iface, const WaveFormatEx_* format, UINT32* defaultPeriodFrames,
    UINT32* fundamentalPeriodFrames, UINT32* minPeriodFrames, UINT32* maxPeriodFrames) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    IAudioClient3_* inner = AsClient3(self->inner);
    return inner->lpVtbl->GetSharedModeEnginePeriod(inner, format, defaultPeriodFrames,
                                                    fundamentalPeriodFrames, minPeriodFrames,
                                                    maxPeriodFrames);
}
HRESULT STDMETHODCALLTYPE Client3_GetCurrentSharedModeEnginePeriod(
    IAudioClient3_* iface, WaveFormatEx_** format, UINT32* currentPeriodFrames) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    IAudioClient3_* inner = AsClient3(self->inner);
    return inner->lpVtbl->GetCurrentSharedModeEnginePeriod(inner, format, currentPeriodFrames);
}
HRESULT STDMETHODCALLTYPE Client3_InitializeSharedAudioStream(
    IAudioClient3_* iface, DWORD streamFlags, UINT32 periodFrames, const WaveFormatEx_* format,
    const GUID* sessionGuid) {
    auto* self = reinterpret_cast<ClientProxy*>(iface);
    Log(L"[NCMEx] Client3_InitializeSharedAudioStream enter frames=%u", periodFrames);
    IAudioClient3_* inner = AsClient3(self->inner);
    if (!LoadConfig().enable || !format || !format->nSamplesPerSec || !periodFrames) {
        return inner->lpVtbl->InitializeSharedAudioStream(inner, streamFlags, periodFrames, format,
                                                          sessionGuid);
    }
    const reference_time_t duration = static_cast<reference_time_t>(
        (static_cast<uint64_t>(periodFrames) * kHnsPerSecond + format->nSamplesPerSec / 2) /
        format->nSamplesPerSec);
    return Client_Initialize(reinterpret_cast<IAudioClient_*>(iface), AUDCLNT_SHAREMODE_SHARED_,
                             streamFlags, duration, 0, format, sessionGuid);
}

// ---------------------------------------------------------------------------
// DeviceProxy / EnumeratorProxy
// ---------------------------------------------------------------------------

DeviceProxy* WrapDevice(IUnknown* real, int flow) {
    return new (std::nothrow) DeviceProxy{&kDeviceVtbl, real, flow, 1};
}

HRESULT STDMETHODCALLTYPE Device_QI(IMMDevice_* iface, REFIID riid, void** ppv) {
    auto* self = reinterpret_cast<DeviceProxy*>(iface);
    if (riid == IID_IMMDevice_ || riid == IID_IUnknown) {
        InterlockedIncrement(&self->ref);
        *ppv = self;
        return S_OK;
    }
    return self->inner->QueryInterface(riid, ppv);
}
ULONG STDMETHODCALLTYPE Device_AddRef(IMMDevice_* iface) {
    auto* self = reinterpret_cast<DeviceProxy*>(iface);
    return InterlockedIncrement(&self->ref);
}
ULONG STDMETHODCALLTYPE Device_Release(IMMDevice_* iface) {
    auto* self = reinterpret_cast<DeviceProxy*>(iface);
    const ULONG r = InterlockedDecrement(&self->ref);
    if (r == 0) {
        self->inner->Release();
        delete self;
    }
    return r;
}

HRESULT STDMETHODCALLTYPE Device_Activate(IMMDevice_* iface, REFIID riid, DWORD clsctx,
                                          PROPVARIANT* params, void** out) {
    auto* self = reinterpret_cast<DeviceProxy*>(iface);
    const int version = riid == IID_IAudioClient3_ ? 3
                        : riid == IID_IAudioClient2_ ? 2
                        : riid == IID_IAudioClient_  ? 1
                                                     : 0;
    if (version) Log(L"[NCMEx] Device_Activate enter flow=%d version=%d", self->flow, version);
    if (version && self->flow == eRender_) {
        void* real = nullptr;
        IMMDevice_* dev = reinterpret_cast<IMMDevice_*>(self->inner);
        const HRESULT hr = dev->lpVtbl->Activate(dev, riid, clsctx, params, &real);
        if (FAILED(hr)) {
            Log(L"[NCMEx] Device_Activate inner exit hr=0x%08lX", static_cast<unsigned long>(hr));
            return hr;
        }
        Device_AddRef(iface);
        const void* vtbl = version == 3 ? static_cast<const void*>(&kClient3Vtbl)
                           : version == 2 ? static_cast<const void*>(&kClient2Vtbl)
                                          : static_cast<const void*>(&kClientVtbl);
        auto* proxy = new (std::nothrow)
            ClientProxy{vtbl, static_cast<IUnknown*>(real), self, 1, version, 0, 0, false, 0,
                        nullptr, nullptr};
        if (!proxy) {
            Device_Release(iface);
            static_cast<IUnknown*>(real)->Release();
            return E_OUTOFMEMORY;
        }
        *out = proxy;
        Log(L"[NCMEx] Device_Activate exit wrapped=%p version=%d", proxy, version);
        return S_OK;
    }
    return reinterpret_cast<IMMDevice_*>(self->inner)->lpVtbl->Activate(
        reinterpret_cast<IMMDevice_*>(self->inner), riid, clsctx, params, out);
}

HRESULT STDMETHODCALLTYPE Device_OpenPropertyStore(IMMDevice_* iface, DWORD access, void** props) {
    auto* self = reinterpret_cast<DeviceProxy*>(iface);
    return reinterpret_cast<IMMDevice_*>(self->inner)->lpVtbl->OpenPropertyStore(
        reinterpret_cast<IMMDevice_*>(self->inner), access, props);
}
HRESULT STDMETHODCALLTYPE Device_GetId(IMMDevice_* iface, LPWSTR* id) {
    auto* self = reinterpret_cast<DeviceProxy*>(iface);
    return reinterpret_cast<IMMDevice_*>(self->inner)->lpVtbl->GetId(
        reinterpret_cast<IMMDevice_*>(self->inner), id);
}
HRESULT STDMETHODCALLTYPE Device_GetState(IMMDevice_* iface, DWORD* state) {
    auto* self = reinterpret_cast<DeviceProxy*>(iface);
    return reinterpret_cast<IMMDevice_*>(self->inner)->lpVtbl->GetState(
        reinterpret_cast<IMMDevice_*>(self->inner), state);
}

HRESULT STDMETHODCALLTYPE Collection_QI(IMMDeviceCollection_* iface, REFIID riid, void** ppv) {
    auto* self = reinterpret_cast<CollectionProxy*>(iface);
    if (riid == IID_IMMDeviceCollection_ || riid == IID_IUnknown) {
        InterlockedIncrement(&self->ref);
        *ppv = self;
        return S_OK;
    }
    return self->inner->QueryInterface(riid, ppv);
}
ULONG STDMETHODCALLTYPE Collection_AddRef(IMMDeviceCollection_* iface) {
    auto* self = reinterpret_cast<CollectionProxy*>(iface);
    return InterlockedIncrement(&self->ref);
}
ULONG STDMETHODCALLTYPE Collection_Release(IMMDeviceCollection_* iface) {
    auto* self = reinterpret_cast<CollectionProxy*>(iface);
    const ULONG r = InterlockedDecrement(&self->ref);
    if (r == 0) {
        self->inner->Release();
        delete self;
    }
    return r;
}
HRESULT STDMETHODCALLTYPE Collection_GetCount(IMMDeviceCollection_* iface, UINT* count) {
    auto* self = reinterpret_cast<CollectionProxy*>(iface);
    return reinterpret_cast<IMMDeviceCollection_*>(self->inner)
        ->lpVtbl->GetCount(reinterpret_cast<IMMDeviceCollection_*>(self->inner), count);
}
HRESULT STDMETHODCALLTYPE Collection_Item(IMMDeviceCollection_* iface, UINT index,
                                          IMMDevice_** device) {
    auto* self = reinterpret_cast<CollectionProxy*>(iface);
    IMMDeviceCollection_* inner = reinterpret_cast<IMMDeviceCollection_*>(self->inner);
    const HRESULT hr = inner->lpVtbl->Item(inner, index, device);
    if (FAILED(hr) || !device || !*device || self->flow == eCapture_) return hr;
    IUnknown* real = reinterpret_cast<IUnknown*>(*device);
    auto* proxy = WrapDevice(real, eRender_);
    if (!proxy) {
        real->Release();
        return E_OUTOFMEMORY;
    }
    *device = reinterpret_cast<IMMDevice_*>(proxy);
    Log(L"[NCMEx] Collection_Item wrapped index=%u", index);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Enum_QI(IMMDeviceEnumerator_* iface, REFIID riid, void** ppv) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    if (riid == IID_IMMDeviceEnumerator_ || riid == IID_IUnknown) {
        InterlockedIncrement(&self->ref);
        *ppv = self;
        return S_OK;
    }
    return self->inner->QueryInterface(riid, ppv);
}
ULONG STDMETHODCALLTYPE Enum_AddRef(IMMDeviceEnumerator_* iface) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    return InterlockedIncrement(&self->ref);
}
ULONG STDMETHODCALLTYPE Enum_Release(IMMDeviceEnumerator_* iface) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    const ULONG r = InterlockedDecrement(&self->ref);
    if (r == 0) {
        self->inner->Release();
        delete self;
    }
    return r;
}

HRESULT STDMETHODCALLTYPE Enum_EnumAudioEndpoints(IMMDeviceEnumerator_* iface, int flow,
                                                  DWORD mask, void** collection) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    const HRESULT hr = reinterpret_cast<IMMDeviceEnumerator_*>(self->inner)
                           ->lpVtbl->EnumAudioEndpoints(
                               reinterpret_cast<IMMDeviceEnumerator_*>(self->inner), flow, mask,
                               collection);
    Log(L"[NCMEx] EnumAudioEndpoints flow=%d mask=0x%lx hr=0x%08lX", flow, mask,
        static_cast<unsigned long>(hr));
    if (FAILED(hr) || !collection || !*collection || flow == eCapture_) return hr;
    IUnknown* real = static_cast<IUnknown*>(*collection);
    auto* proxy = new (std::nothrow) CollectionProxy{&kCollectionVtbl, real, flow, 1};
    if (!proxy) {
        real->Release();
        return E_OUTOFMEMORY;
    }
    *collection = proxy;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Enum_GetDefaultAudioEndpoint(IMMDeviceEnumerator_* iface, int flow,
                                                       int role, IMMDevice_** device) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    const HRESULT hr = reinterpret_cast<IMMDeviceEnumerator_*>(self->inner)
                           ->lpVtbl->GetDefaultAudioEndpoint(
                               reinterpret_cast<IMMDeviceEnumerator_*>(self->inner), flow, role,
                               device);
    Log(L"[NCMEx] GetDefaultAudioEndpoint flow=%d role=%d hr=0x%08lX", flow, role,
        static_cast<unsigned long>(hr));
    if (FAILED(hr) || flow != eRender_ || !device || !*device) return hr;
    IUnknown* real = reinterpret_cast<IUnknown*>(*device);
    auto* proxy = WrapDevice(real, flow);
    if (!proxy) {
        real->Release();
        return E_OUTOFMEMORY;
    }
    *device = reinterpret_cast<IMMDevice_*>(proxy);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Enum_GetDevice(IMMDeviceEnumerator_* iface, LPCWSTR id,
                                         IMMDevice_** device) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    const HRESULT hr = reinterpret_cast<IMMDeviceEnumerator_*>(self->inner)
                           ->lpVtbl->GetDevice(reinterpret_cast<IMMDeviceEnumerator_*>(self->inner),
                                               id, device);
    Log(L"[NCMEx] GetDevice hr=0x%08lX id=%ls", static_cast<unsigned long>(hr),
        id ? id : L"");
    if (FAILED(hr) || !device || !*device) return hr;
    IUnknown* real = reinterpret_cast<IUnknown*>(*device);
    auto* proxy = WrapDevice(real, eRender_);
    if (!proxy) {
        real->Release();
        return E_OUTOFMEMORY;
    }
    *device = reinterpret_cast<IMMDevice_*>(proxy);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Enum_RegisterNotification(IMMDeviceEnumerator_* iface, void* client) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    return reinterpret_cast<IMMDeviceEnumerator_*>(self->inner)
        ->lpVtbl->RegisterEndpointNotificationCallback(
            reinterpret_cast<IMMDeviceEnumerator_*>(self->inner), client);
}
HRESULT STDMETHODCALLTYPE Enum_UnregisterNotification(IMMDeviceEnumerator_* iface, void* client) {
    auto* self = reinterpret_cast<EnumeratorProxy*>(iface);
    return reinterpret_cast<IMMDeviceEnumerator_*>(self->inner)
        ->lpVtbl->UnregisterEndpointNotificationCallback(
            reinterpret_cast<IMMDeviceEnumerator_*>(self->inner), client);
}

// ---------------------------------------------------------------------------
// vtables
// ---------------------------------------------------------------------------
const IMMDeviceEnumeratorVtbl_ kEnumVtbl = {
    Enum_QI,
    Enum_AddRef,
    Enum_Release,
    Enum_EnumAudioEndpoints,
    Enum_GetDefaultAudioEndpoint,
    Enum_GetDevice,
    Enum_RegisterNotification,
    Enum_UnregisterNotification};

const IMMDeviceCollectionVtbl_ kCollectionVtbl = {Collection_QI, Collection_AddRef,
                                                  Collection_Release, Collection_GetCount,
                                                  Collection_Item};

const IMMDeviceVtbl_ kDeviceVtbl = {Device_QI,         Device_AddRef,     Device_Release,
                                    Device_Activate,   Device_OpenPropertyStore,
                                    Device_GetId,      Device_GetState};

const IAudioClientVtbl_ kClientVtbl = {
    Client_QI,
    Client_AddRef,
    Client_Release,
    Client_Initialize,
    Client_GetBufferSize,
    Client_GetStreamLatency,
    Client_GetCurrentPadding,
    Client_IsFormatSupported,
    Client_GetMixFormat,
    Client_GetDevicePeriod,
    Client_Start,
    Client_Stop,
    Client_Reset,
    Client_SetEventHandle,
    Client_GetService};

const IAudioClient2Vtbl_ kClient2Vtbl = {kClientVtbl, Client2_IsOffloadCapable,
                                         Client2_SetClientProperties, Client2_GetBufferSizeLimits};
const IAudioClient3Vtbl_ kClient3Vtbl = {kClient2Vtbl, Client3_GetSharedModeEnginePeriod,
                                         Client3_GetCurrentSharedModeEnginePeriod,
                                         Client3_InitializeSharedAudioStream};

const IAudioRenderClientVtbl_ kRenderVtbl = {Render_QI, Render_AddRef, Render_Release,
                                             Render_GetBuffer, Render_ReleaseBuffer};

static_assert(sizeof(IMMDeviceEnumeratorVtbl_) == 8 * sizeof(void*), "IMMDeviceEnumerator vtable");
static_assert(sizeof(IMMDeviceCollectionVtbl_) == 5 * sizeof(void*), "IMMDeviceCollection vtable");
static_assert(sizeof(IMMDeviceVtbl_) == 7 * sizeof(void*), "IMMDevice vtable");
static_assert(sizeof(IAudioClientVtbl_) == 15 * sizeof(void*), "IAudioClient vtable");
static_assert(sizeof(IAudioClient2Vtbl_) == 18 * sizeof(void*), "IAudioClient2 vtable");
static_assert(sizeof(IAudioClient3Vtbl_) == 21 * sizeof(void*), "IAudioClient3 vtable");
static_assert(sizeof(IAudioRenderClientVtbl_) == 5 * sizeof(void*), "IAudioRenderClient vtable");

// ---------------------------------------------------------------------------
// CoCreateInstance / CoCreateInstanceEx 钩子 + 托盘气泡
// ---------------------------------------------------------------------------

using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
CoCreateInstanceFn g_origCoCreateInstance = nullptr;
using CoCreateInstanceExFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, COSERVERINFO*, DWORD,
                                              MULTI_QI*);
CoCreateInstanceExFn g_origCoCreateInstanceEx = nullptr;

HRESULT WINAPI Detour_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                       REFIID riid, LPVOID* ppv) {
    if (!pUnkOuter && rclsid == CLSID_MMDeviceEnumerator_ &&
        (riid == IID_IMMDeviceEnumerator_ || riid == IID_IUnknown)) {
        if (LoadConfig().verbose) {
            Log(L"[NCMEx] CoCreateInstance(MMDeviceEnumerator) pid=%lu", GetCurrentProcessId());
        }
        void* real = nullptr;
        const HRESULT hr =
            g_origCoCreateInstance(rclsid, nullptr, dwClsContext, IID_IMMDeviceEnumerator_, &real);
        if (FAILED(hr)) return hr;
        auto* proxy =
            new (std::nothrow) EnumeratorProxy{&kEnumVtbl, static_cast<IUnknown*>(real), 1};
        if (!proxy) {
            static_cast<IUnknown*>(real)->Release();
            return E_OUTOFMEMORY;
        }
        *ppv = proxy;
        return S_OK;
    }
    return g_origCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

HRESULT WINAPI Detour_CoCreateInstanceEx(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                         COSERVERINFO* serverInfo, DWORD count,
                                         MULTI_QI* results) {
    if (!pUnkOuter && rclsid == CLSID_MMDeviceEnumerator_ && count == 1 && results &&
        results[0].pIID &&
        *results[0].pIID == static_cast<const IID&>(IID_IMMDeviceEnumerator_)) {
        MULTI_QI one{};
        one.pIID = const_cast<IID*>(static_cast<const IID*>(&IID_IMMDeviceEnumerator_));
        const HRESULT hr = g_origCoCreateInstanceEx(rclsid, nullptr, dwClsContext, serverInfo, 1,
                                                    &one);
        if (FAILED(hr)) return hr;
        results[0].hr = one.hr;
        if (FAILED(one.hr) || !one.pItf) return FAILED(one.hr) ? one.hr : E_NOINTERFACE;
        auto* proxy =
            new (std::nothrow) EnumeratorProxy{&kEnumVtbl, static_cast<IUnknown*>(one.pItf), 1};
        if (!proxy) {
            static_cast<IUnknown*>(one.pItf)->Release();
            return E_OUTOFMEMORY;
        }
        results[0].pItf = reinterpret_cast<IUnknown*>(proxy);
        results[0].hr = S_OK;
        return S_OK;
    }
    return g_origCoCreateInstanceEx(rclsid, pUnkOuter, dwClsContext, serverInfo, count, results);
}

using WaveOutOpenFn = DWORD(WINAPI*)(void*, UINT, const void*, DWORD_PTR, DWORD_PTR, DWORD);
WaveOutOpenFn g_origWaveOutOpen = nullptr;

DWORD WINAPI Detour_waveOutOpen(void* out, UINT device, const void* format, DWORD_PTR callback,
                                DWORD_PTR instance, DWORD flags) {
    const auto* fmt = static_cast<const WaveFormatEx_*>(format);
    Log(L"[NCMEx] waveOutOpen device=%u tag=0x%x bits=%u ch=%u rate=%u flags=0x%lx", device,
        fmt ? fmt->wFormatTag : 0, fmt ? fmt->wBitsPerSample : 0, fmt ? fmt->nChannels : 0,
        fmt ? fmt->nSamplesPerSec : 0, flags);
    return g_origWaveOutOpen(out, device, format, callback, instance, flags);
}

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_origGetProcAddress = nullptr;

FARPROC WINAPI Detour_GetProcAddress(HMODULE module, LPCSTR name) {
    if (!name || !(reinterpret_cast<uintptr_t>(name) >> 16)) {
        return g_origGetProcAddress(module, name);
    }
    if (std::strcmp(name, "CoCreateInstance") == 0 && g_origCoCreateInstance) {
        Log(L"[NCMEx] GetProcAddress 重定向 CoCreateInstance");
        return reinterpret_cast<FARPROC>(&Detour_CoCreateInstance);
    }
    if (std::strcmp(name, "CoCreateInstanceEx") == 0 && g_origCoCreateInstanceEx) {
        Log(L"[NCMEx] GetProcAddress 重定向 CoCreateInstanceEx");
        return reinterpret_cast<FARPROC>(&Detour_CoCreateInstanceEx);
    }
    if (std::strcmp(name, "waveOutOpen") == 0 && g_origWaveOutOpen) {
        Log(L"[NCMEx] GetProcAddress 重定向 waveOutOpen");
        return reinterpret_cast<FARPROC>(&Detour_waveOutOpen);
    }

    const bool interesting = std::strcmp(name, "DirectSoundCreate") == 0 ||
                             std::strcmp(name, "DirectSoundEnumerateW") == 0 ||
                             std::strcmp(name, "MFCreateAudioRendererActivate") == 0 ||
                             std::strcmp(name, "CoCreateInstance") == 0 ||
                             std::strcmp(name, "waveOutOpen") == 0;
    FARPROC result = g_origGetProcAddress(module, name);
    if (interesting) Log(L"[NCMEx] 音频动态解析 name=%hs result=%p", name, result);
    return result;
}

DWORD WINAPI BalloonThread(void*) {
    const Config cfg = LoadConfig();
    if (!cfg.balloon) return 0;

    const wchar_t* cls = L"NCMExHidden";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = cls;
    if (!RegisterClassW(&wc)) return 0;
    HWND hwnd = CreateWindowExW(0, cls, L"NCMEx", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr,
                                nullptr);
    if (!hwnd) return 0;

    // 32x32 纯绿色方块图标
    constexpr int kIcon = 32;
    BYTE mask[(kIcon * kIcon) / 8] = {};
    BYTE color[kIcon * kIcon * 4] = {};
    for (size_t i = 0; i < sizeof(color); i += 4) {
        color[i] = 0x30;      // B
        color[i + 1] = 0xC0;  // G
        color[i + 2] = 0x30;  // R
        color[i + 3] = 0xFF;  // A
    }
    HICON icon = CreateIcon(nullptr, kIcon, kIcon, 1, 32, mask, color);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 0x4E434D;
    nid.uFlags = NIF_ICON | NIF_INFO;
    nid.hIcon = icon;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 4000;
    wcscpy(nid.szInfoTitle, L"NCMExclusive");
    wcscpy(nid.szInfo, L"WASAPI 独占模式已生效");
    Shell_NotifyIconW(NIM_ADD, &nid);
    Sleep(4000);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (icon) DestroyIcon(icon);
    DestroyWindow(hwnd);
    UnregisterClassW(cls, nullptr);
    return 0;
}

void BalloonNotify() {
    static LONG fired = 0;
    if (InterlockedCompareExchange(&fired, 1, 0) != 0) return;
    if (HANDLE t = CreateThread(nullptr, 0, BalloonThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace

void InstallHooks() {
    const auto shouldHook = [](HMODULE module) {
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        return _wcsicmp(name, L"cloudmusic.exe") == 0 ||
               _wcsicmp(name, L"cloudmusic.dll") == 0 || _wcsicmp(name, L"libcef.dll") == 0 ||
               _wcsicmp(name, L"哔哩哔哩.exe") == 0;
    };
    const auto hookModule = [&](HMODULE module) {
        int count = 0;
        count += IatHook(module, nullptr, "CoCreateInstance",
                         reinterpret_cast<void*>(&Detour_CoCreateInstance),
                         reinterpret_cast<void**>(&g_origCoCreateInstance)) ? 1 : 0;
        count += IatHook(module, nullptr, "CoCreateInstanceEx",
                         reinterpret_cast<void*>(&Detour_CoCreateInstanceEx),
                         reinterpret_cast<void**>(&g_origCoCreateInstanceEx)) ? 1 : 0;
        count += IatHook(module, nullptr, "waveOutOpen",
                         reinterpret_cast<void*>(&Detour_waveOutOpen),
                         reinterpret_cast<void**>(&g_origWaveOutOpen)) ? 1 : 0;
        count += IatHook(module, nullptr, "GetProcAddress",
                         reinterpret_cast<void*>(&Detour_GetProcAddress),
                         reinterpret_cast<void**>(&g_origGetProcAddress)) ? 1 : 0;
        return count;
    };

    int patched = 0;
    HMODULE exe = GetModuleHandleW(nullptr);
    if (shouldHook(exe)) patched += hookModule(exe);

#ifndef NCMEX_NO_PSAPI
    HMODULE mods[1024] = {};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD bytes = needed < sizeof(mods) ? needed : sizeof(mods);
        const size_t count = bytes / sizeof(HMODULE);
        for (size_t i = 0; i < count; ++i) {
            if (mods[i] != exe && shouldHook(mods[i])) patched += hookModule(mods[i]);
        }
    }
#endif

    static int lastLogged = -1;
    if (patched != lastLogged) {
        lastLogged = patched;
        Log(L"[NCMEx] 钩子已安装 pid=%lu patched=%d", GetCurrentProcessId(), patched);
        if (patched == 0) Log(L"[NCMEx] 警告：目标模块尚未导入 CoCreateInstance/GetProcAddress");
    }
}

}  // namespace ncmex
