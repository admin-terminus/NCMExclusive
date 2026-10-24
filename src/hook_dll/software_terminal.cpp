#include "software_terminal.h"

#include "../common/util.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

namespace ncmex {
namespace {

constexpr HRESULT kBufferTooLarge = 0x88890006;
constexpr HRESULT kOutOfOrder = 0x88890007;

}  // namespace

struct SoftwareState::Impl {
    IAudioClient_* client = nullptr;
    IAudioRenderClient_* render = nullptr;
    UINT32 bufferFrames = 0;
    UINT32 blockAlign = 0;
    UINT32 channels = 0;
    int mode = 0;

    HANDLE hostEvent = nullptr;
    HANDLE realEvent = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE pumpThread = nullptr;

    std::mutex controlMutex;
    std::mutex mutex;
    std::deque<std::vector<BYTE>> blocks;
    std::vector<BYTE> staging;
    std::vector<BYTE> pumpBuffer;
    UINT32 padding = 0;
    UINT32 acquiredFrames = 0;
    bool acquired = false;
    bool running = false;

    ~Impl() {
        if (pumpThread) CloseHandle(pumpThread);
        if (realEvent) CloseHandle(realEvent);
        if (stopEvent) CloseHandle(stopEvent);
        if (render) render->lpVtbl->Release(render);
        if (client) client->lpVtbl->Release(client);
    }
};

namespace {

HRESULT PumpOnce(SoftwareState::Impl* state) {
    if (!state || !state->render || !state->bufferFrames) return E_UNEXPECTED;
    BYTE* destination = nullptr;
    HRESULT hr = state->render->lpVtbl->GetBuffer(state->render, state->bufferFrames,
                                                  &destination);
    if (FAILED(hr)) {
        static LONG errors = 0;
        if (InterlockedIncrement(&errors) <= 8) {
            Log(L"[NCMEx] SoftwarePump GetBuffer failed hr=0x%08lX",
                static_cast<unsigned long>(hr));
        }
        return hr;
    }

    size_t produced = 0;
    const size_t wanted = static_cast<size_t>(state->bufferFrames) * state->blockAlign;
    {
        std::lock_guard lock(state->mutex);
        std::memset(destination, 0, wanted);
        while (produced < wanted && !state->blocks.empty()) {
            auto& block = state->blocks.front();
            const size_t count = std::min(wanted - produced, block.size());
            std::memcpy(destination + produced, block.data(), count);
            produced += count;
            if (count == block.size()) {
                state->blocks.pop_front();
            } else {
                block.erase(block.begin(), block.begin() + static_cast<ptrdiff_t>(count));
            }
        }
        const UINT32 consumed = static_cast<UINT32>(produced / state->blockAlign);
        state->padding = consumed <= state->padding ? state->padding - consumed : 0;
    }

    hr = state->render->lpVtbl->ReleaseBuffer(state->render, state->bufferFrames, 0);
    if (FAILED(hr)) {
        static LONG errors = 0;
        if (InterlockedIncrement(&errors) <= 8) {
            Log(L"[NCMEx] SoftwarePump ReleaseBuffer failed hr=0x%08lX produced=%zu",
                static_cast<unsigned long>(hr), produced);
        }
    }
    if (SUCCEEDED(hr) && state->hostEvent) SetEvent(state->hostEvent);
    return hr;
}

DWORD WINAPI PumpThread(void* context) {
    auto* state = static_cast<SoftwareState::Impl*>(context);
    HANDLE handles[] = {state->stopEvent, state->realEvent};
    LONG errors = 0;
    for (;;) {
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) break;
        const HRESULT hr = PumpOnce(state);
        if (FAILED(hr) && InterlockedIncrement(&errors) <= 8) {
            Log(L"[NCMEx] SoftwarePump error hr=0x%08lX", static_cast<unsigned long>(hr));
        }
    }
    return 0;
}

}  // namespace

SoftwareState::SoftwareState() = default;
SoftwareState::~SoftwareState() {
    if (impl) Stop();
}

std::shared_ptr<SoftwareState> SoftwareState::Create(IAudioClient_* client,
                                                     IAudioRenderClient_* render,
                                                     UINT32 bufferFrames,
                                                     UINT32 blockAlign,
                                                     UINT32 channels,
                                                     int mode,
                                                     HANDLE hostEvent) {
    if (!client || !render || !bufferFrames || !blockAlign || !channels) {
        if (render) render->lpVtbl->Release(render);
        return nullptr;
    }
    auto state = std::shared_ptr<SoftwareState>(new (std::nothrow) SoftwareState());
    if (!state) {
        render->lpVtbl->Release(render);
        return nullptr;
    }
    state->impl = std::unique_ptr<Impl>(new (std::nothrow) Impl());
    if (!state->impl) {
        render->lpVtbl->Release(render);
        return nullptr;
    }

    auto* impl = state->impl.get();
    impl->client = client;
    impl->client->lpVtbl->AddRef(impl->client);
    impl->render = render;
    impl->bufferFrames = bufferFrames;
    impl->blockAlign = blockAlign;
    impl->channels = channels;
    impl->mode = mode;
    impl->hostEvent = hostEvent;
    impl->realEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl->realEvent || !impl->stopEvent) return nullptr;

    try {
        impl->staging.resize(static_cast<size_t>(bufferFrames) * blockAlign);
        impl->pumpBuffer.resize(impl->staging.size());
    } catch (...) {
        return nullptr;
    }

    const HRESULT hr = impl->client->lpVtbl->SetEventHandle(impl->client, impl->realEvent);
    if (FAILED(hr)) return nullptr;
    Log(L"[NCMEx] SoftwareTerminal created frames=%u align=%u mode=%d", bufferFrames, blockAlign,
        mode);
    return state;
}

HRESULT SoftwareState::GetBuffer(UINT32 frames, BYTE** data) {
    if (!data) return E_POINTER;
    *data = nullptr;
    auto* state = impl.get();
    std::lock_guard lock(state->mutex);
    if (state->acquired) return kOutOfOrder;
    if (frames > state->bufferFrames || state->padding + frames > state->bufferFrames) {
        return kBufferTooLarge;
    }
    if (frames) {
        state->acquired = true;
        state->acquiredFrames = frames;
        std::memset(state->staging.data(), 0,
                    static_cast<size_t>(frames) * state->blockAlign);
    }
    *data = state->staging.data();
    return S_OK;
}

HRESULT SoftwareState::ReleaseBuffer(UINT32 frames, DWORD flags) {
    auto* state = impl.get();
    std::lock_guard lock(state->mutex);
    if (!state->acquired) return frames == 0 ? S_OK : kOutOfOrder;
    if (frames > state->acquiredFrames || state->padding + frames > state->bufferFrames) {
        return kBufferTooLarge;
    }

    const size_t bytes = static_cast<size_t>(frames) * state->blockAlign;
    if (flags & 0x1) {
        std::memset(state->staging.data(), 0, bytes);
    } else if (state->mode == 2 && frames) {
        float* input = reinterpret_cast<float*>(state->staging.data());
        int32_t* output = reinterpret_cast<int32_t*>(state->staging.data());
        const size_t count = static_cast<size_t>(frames) * state->channels;
        for (size_t i = 0; i < count; ++i) {
            const double value = input[i];
            if (value >= 1.0) {
                output[i] = 0x7FFFFF00;
            } else if (value <= -1.0) {
                output[i] = INT32_MIN;
            } else {
                output[i] = static_cast<int32_t>(value * 2147483648.0) & ~0xFF;
            }
        }
    }

    try {
        state->blocks.emplace_back(state->staging.begin(), state->staging.begin() + bytes);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    state->padding += frames;
    state->acquired = false;
    state->acquiredFrames = 0;
    return S_OK;
}

UINT32 SoftwareState::Padding() {
    auto* state = impl.get();
    std::lock_guard lock(state->mutex);
    return state->padding;
}

UINT32 SoftwareState::Frames() const {
    return impl ? impl->bufferFrames : 0;
}

void SoftwareState::SetHostEvent(HANDLE eventHandle) {
    auto* state = impl.get();
    std::lock_guard lock(state->mutex);
    state->hostEvent = eventHandle;
}

HRESULT SoftwareState::Start() {
    auto* state = impl.get();
    std::lock_guard control(state->controlMutex);
    if (state->running) return S_OK;
    if (!state->realEvent) return E_UNEXPECTED;

    HRESULT hr = PumpOnce(state);
    if (FAILED(hr)) return hr;
    hr = state->client->lpVtbl->Start(state->client);
    if (FAILED(hr)) return hr;

    state->running = true;
    state->pumpThread = CreateThread(nullptr, 0, PumpThread, state, 0, nullptr);
    if (!state->pumpThread) {
        state->running = false;
        state->client->lpVtbl->Stop(state->client);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    Log(L"[NCMEx] SoftwarePump started");
    return S_OK;
}

HRESULT SoftwareState::Stop() {
    auto* state = impl.get();
    std::lock_guard control(state->controlMutex);
    if (!state->running) return S_OK;
    state->running = false;
    SetEvent(state->stopEvent);
    SetEvent(state->realEvent);
    if (state->pumpThread) {
        WaitForSingleObject(state->pumpThread, 5000);
        CloseHandle(state->pumpThread);
        state->pumpThread = nullptr;
    }
    const HRESULT hr = state->client->lpVtbl->Stop(state->client);
    ResetEvent(state->stopEvent);
    Log(L"[NCMEx] SoftwarePump stopped hr=0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT SoftwareState::Reset() {
    HRESULT hr = Stop();
    auto* state = impl.get();
    {
        std::lock_guard lock(state->mutex);
        state->blocks.clear();
        state->padding = 0;
        state->acquired = false;
        state->acquiredFrames = 0;
    }
    const HRESULT resetHr = state->client->lpVtbl->Reset(state->client);
    if (FAILED(resetHr)) hr = resetHr;
    return hr;
}

}  // namespace ncmex
