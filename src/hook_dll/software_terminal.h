#pragma once

#include "../common/wasapi_min.h"

#include <memory>

namespace ncmex {

struct SoftwareState {
    static std::shared_ptr<SoftwareState> Create(IAudioClient_* client,
                                                 IAudioRenderClient_* render,
                                                 UINT32 bufferFrames,
                                                 UINT32 blockAlign,
                                                 UINT32 channels,
                                                 int mode,
                                                 HANDLE hostEvent);

    SoftwareState();
    ~SoftwareState();

    HRESULT GetBuffer(UINT32 frames, BYTE** data);
    HRESULT ReleaseBuffer(UINT32 frames, DWORD flags);
    UINT32 Padding();
    UINT32 Frames() const;
    void SetHostEvent(HANDLE eventHandle);
    HRESULT Start();
    HRESULT Stop();
    HRESULT Reset();

public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

}  // namespace ncmex
