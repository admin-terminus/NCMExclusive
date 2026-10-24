// NCMExclusive - 手写 WASAPI/COM 最小声明
// 目的：避免 audioclient.h / mmdeviceapi.h 在 mingw 与 MSVC 间的差异，
// 只声明本项目用到的接口。内存布局与官方完全一致（按 vtable 顺序）。
#pragma once

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <cstdint>

namespace ncmex {

using reference_time_t = LONGLONG;  // 100ns 单位

enum EDataFlow_ { eRender_ = 0, eCapture_ = 1, eAll_ = 2 };
enum ERole_ { eConsole_ = 0, eMultimedia_ = 1, eCommunications_ = 2 };

enum AudclntShareMode_ {
    AUDCLNT_SHAREMODE_SHARED_ = 0,
    AUDCLNT_SHAREMODE_EXCLUSIVE_ = 1,
};

// stream flags（与 audioclient.h 一致）
constexpr DWORD AUDCLNT_STREAMFLAGS_CROSSPROCESS_ = 0x00010000;
constexpr DWORD AUDCLNT_STREAMFLAGS_LOOPBACK_ = 0x00020000;
constexpr DWORD AUDCLNT_STREAMFLAGS_EVENTCALLBACK_ = 0x00040000;
constexpr DWORD AUDCLNT_STREAMFLAGS_NOPERSIST_ = 0x00080000;
constexpr DWORD AUDCLNT_STREAMFLAGS_RATEADJUST_ = 0x00100000;
constexpr DWORD AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY_ = 0x08000000;
constexpr DWORD AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM_ = 0x80000000;

// 常用 AUDCLNT 错误码
constexpr HRESULT AUDCLNT_E_NOT_INITIALIZED_ = 0x88890001;
constexpr HRESULT AUDCLNT_E_UNSUPPORTED_FORMAT_ = 0x88890008;
constexpr HRESULT AUDCLNT_E_DEVICE_IN_USE_ = 0x8889000A;
constexpr HRESULT AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED_ = 0x8889000E;
constexpr HRESULT AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED_ = 0x88890019;

// GUID（与官方相同字节序）
struct Guid_ : GUID {
    constexpr Guid_(DWORD a, WORD b, WORD c, BYTE d0, BYTE d1, BYTE d2, BYTE d3,
                    BYTE d4, BYTE d5, BYTE d6, BYTE d7)
        : GUID{a, b, c, {d0, d1, d2, d3, d4, d5, d6, d7}} {}
    constexpr bool operator==(const GUID& o) const {
        if (Data1 != o.Data1 || Data2 != o.Data2 || Data3 != o.Data3) return false;
        for (int i = 0; i < 8; ++i)
            if (Data4[i] != o.Data4[i]) return false;
        return true;
    }
    constexpr bool operator!=(const GUID& o) const { return !(*this == o); }
};

inline constexpr Guid_ CLSID_MMDeviceEnumerator_(
    0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
inline constexpr Guid_ IID_IMMDeviceEnumerator_(
    0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
inline constexpr Guid_ IID_IMMDevice_(
    0xD666063F, 0x1587, 0x4E43, 0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F);
inline constexpr Guid_ IID_IMMDeviceCollection_(
    0x0BD7A1BE, 0x7A1A, 0x44DB, 0x83, 0x97, 0xCC, 0x53, 0x92, 0x38, 0x7B, 0x5E);
inline constexpr Guid_ IID_IAudioClient_(
    0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
inline constexpr Guid_ IID_IAudioClient2_(
    0x726778CD, 0xF60A, 0x4EDA, 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA);
inline constexpr Guid_ IID_IAudioClient3_(
    0x7ED4EE07, 0x8E67, 0x4CD4, 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42);
inline constexpr Guid_ IID_IAudioRenderClient_(
    0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
inline constexpr Guid_ KSDATAFORMAT_SUBTYPE_PCM_(
    0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
inline constexpr Guid_ KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_(
    0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

// 与官方布局一致的 WAVEFORMATEX / WAVEFORMATEXTENSIBLE
#pragma pack(push, 1)
struct WaveFormatEx_ {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};

struct WaveFormatExtensible_ {
    WaveFormatEx_ Format;
    union {
        WORD wValidBitsPerSample;
        WORD wSamplesPerBlock;
        WORD wReserved;
    } Samples;
    DWORD dwChannelMask;
    GUID SubFormat;
};
#pragma pack(pop)

constexpr WORD WAVE_FORMAT_EXTENSIBLE_ = 0xFFFE;
constexpr WORD WAVE_FORMAT_PCM_ = 1;
constexpr WORD WAVE_FORMAT_IEEE_FLOAT_ = 3;

struct IMMDevice_;
struct IMMDeviceEnumerator_;
struct IMMDeviceCollection_;

struct IMMDeviceEnumeratorVtbl_ {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(IMMDeviceEnumerator_* self, REFIID riid,
                                               void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(IMMDeviceEnumerator_* self);
    ULONG(STDMETHODCALLTYPE* Release)(IMMDeviceEnumerator_* self);
    HRESULT(STDMETHODCALLTYPE* EnumAudioEndpoints)(IMMDeviceEnumerator_* self, int flow,
                                                   DWORD mask, void** collection);
    HRESULT(STDMETHODCALLTYPE* GetDefaultAudioEndpoint)(IMMDeviceEnumerator_* self, int flow,
                                                        int role, IMMDevice_** device);
    HRESULT(STDMETHODCALLTYPE* GetDevice)(IMMDeviceEnumerator_* self, LPCWSTR id,
                                          IMMDevice_** device);
    HRESULT(STDMETHODCALLTYPE* RegisterEndpointNotificationCallback)(IMMDeviceEnumerator_* self,
                                                                     void* client);
    HRESULT(STDMETHODCALLTYPE* UnregisterEndpointNotificationCallback)(
        IMMDeviceEnumerator_* self, void* client);
};

struct IMMDeviceEnumerator_ {
    const IMMDeviceEnumeratorVtbl_* lpVtbl;
};

struct IMMDeviceVtbl_ {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(IMMDevice_* self, REFIID riid, void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(IMMDevice_* self);
    ULONG(STDMETHODCALLTYPE* Release)(IMMDevice_* self);
    HRESULT(STDMETHODCALLTYPE* Activate)(IMMDevice_* self, REFIID riid, DWORD clsctx,
                                         PROPVARIANT* activationParams, void** interfaceOut);
    HRESULT(STDMETHODCALLTYPE* OpenPropertyStore)(IMMDevice_* self, DWORD access,
                                                  void** props);
    HRESULT(STDMETHODCALLTYPE* GetId)(IMMDevice_* self, LPWSTR* id);
    HRESULT(STDMETHODCALLTYPE* GetState)(IMMDevice_* self, DWORD* state);
};

struct IMMDevice_ {
    const IMMDeviceVtbl_* lpVtbl;
};

struct IMMDeviceCollectionVtbl_ {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(IMMDeviceCollection_* self, REFIID riid,
                                               void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(IMMDeviceCollection_* self);
    ULONG(STDMETHODCALLTYPE* Release)(IMMDeviceCollection_* self);
    HRESULT(STDMETHODCALLTYPE* GetCount)(IMMDeviceCollection_* self, UINT* count);
    HRESULT(STDMETHODCALLTYPE* Item)(IMMDeviceCollection_* self, UINT index, IMMDevice_** device);
};

struct IMMDeviceCollection_ {
    const IMMDeviceCollectionVtbl_* lpVtbl;
};

struct IAudioClient_;
struct IAudioClient2_;
struct IAudioClient3_;

struct IAudioClientVtbl_ {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(IAudioClient_* self, REFIID riid, void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(IAudioClient_* self);
    ULONG(STDMETHODCALLTYPE* Release)(IAudioClient_* self);
    HRESULT(STDMETHODCALLTYPE* Initialize)(IAudioClient_* self, int shareMode, DWORD streamFlags,
                                           reference_time_t bufferDuration,
                                           reference_time_t periodicity,
                                           const WaveFormatEx_* format,
                                           const GUID* sessionGuid);
    HRESULT(STDMETHODCALLTYPE* GetBufferSize)(IAudioClient_* self, UINT32* size);
    HRESULT(STDMETHODCALLTYPE* GetStreamLatency)(IAudioClient_* self, reference_time_t* latency);
    HRESULT(STDMETHODCALLTYPE* GetCurrentPadding)(IAudioClient_* self, UINT32* padding);
    HRESULT(STDMETHODCALLTYPE* IsFormatSupported)(IAudioClient_* self, int shareMode,
                                                  const WaveFormatEx_* format,
                                                  WaveFormatEx_** closest);
    HRESULT(STDMETHODCALLTYPE* GetMixFormat)(IAudioClient_* self, WaveFormatEx_** format);
    HRESULT(STDMETHODCALLTYPE* GetDevicePeriod)(IAudioClient_* self, reference_time_t* defPeriod,
                                                reference_time_t* minPeriod);
    HRESULT(STDMETHODCALLTYPE* Start)(IAudioClient_* self);
    HRESULT(STDMETHODCALLTYPE* Stop)(IAudioClient_* self);
    HRESULT(STDMETHODCALLTYPE* Reset)(IAudioClient_* self);
    HRESULT(STDMETHODCALLTYPE* SetEventHandle)(IAudioClient_* self, HANDLE eventHandle);
    HRESULT(STDMETHODCALLTYPE* GetService)(IAudioClient_* self, REFIID riid, void** service);
};

struct IAudioClient_ {
    const IAudioClientVtbl_* lpVtbl;
};

struct IAudioClient2Vtbl_ {
    IAudioClientVtbl_ base;
    HRESULT(STDMETHODCALLTYPE* IsOffloadCapable)(IAudioClient2_* self, int category,
                                                 BOOL* offloadCapable);
    HRESULT(STDMETHODCALLTYPE* SetClientProperties)(IAudioClient2_* self, const void* properties);
    HRESULT(STDMETHODCALLTYPE* GetBufferSizeLimits)(IAudioClient2_* self,
                                                    const WaveFormatEx_* format, BOOL eventDriven,
                                                    reference_time_t* minBufferDuration,
                                                    reference_time_t* maxBufferDuration);
};

struct IAudioClient2_ {
    const IAudioClient2Vtbl_* lpVtbl;
};

struct IAudioClient3Vtbl_ {
    IAudioClient2Vtbl_ base2;
    HRESULT(STDMETHODCALLTYPE* GetSharedModeEnginePeriod)(
        IAudioClient3_* self, const WaveFormatEx_* format, UINT32* defaultPeriodFrames,
        UINT32* fundamentalPeriodFrames, UINT32* minPeriodFrames, UINT32* maxPeriodFrames);
    HRESULT(STDMETHODCALLTYPE* GetCurrentSharedModeEnginePeriod)(
        IAudioClient3_* self, WaveFormatEx_** format, UINT32* currentPeriodFrames);
    HRESULT(STDMETHODCALLTYPE* InitializeSharedAudioStream)(
        IAudioClient3_* self, DWORD streamFlags, UINT32 periodFrames,
        const WaveFormatEx_* format, const GUID* sessionGuid);
};

struct IAudioClient3_ {
    const IAudioClient3Vtbl_* lpVtbl;
};

struct IAudioRenderClient_;

struct IAudioRenderClientVtbl_ {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(IAudioRenderClient_* self, REFIID riid,
                                               void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(IAudioRenderClient_* self);
    ULONG(STDMETHODCALLTYPE* Release)(IAudioRenderClient_* self);
    HRESULT(STDMETHODCALLTYPE* GetBuffer)(IAudioRenderClient_* self, UINT32 frames,
                                          BYTE** data);
    HRESULT(STDMETHODCALLTYPE* ReleaseBuffer)(IAudioRenderClient_* self, UINT32 frames,
                                              DWORD flags);
};

struct IAudioRenderClient_ {
    const IAudioRenderClientVtbl_* lpVtbl;
};

}  // namespace ncmex
