// idhmfis_camera.cpp — IDHMFISCapture.dll
//
// IMFMediaSource implementation for MFCreateVirtualCamera (Windows 11+).
// Reads BGR24 frames from IDHMFIS named shared memory (Local\IDHMFISCameraFrame)
// written by virtual_camera.cpp in IDHMFIS.exe and delivers them to consumers
// such as Capture via the Windows Media Foundation pipeline.
//
// COM entry points (exported via IDHMFISCapture.def):
//   DllGetClassObject — creates IMFMediaSource for CLSID_IdhmfisCapture
//   DllCanUnloadNow   — always S_FALSE

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>

#include "camera_shm.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Fixed CLSID — must match kIdhmfisCaptureCLSID in virtual_camera.cpp.
//  {CA1D0001-DAC0-1DA0-BEEF-DEADBEEF0001}
// ─────────────────────────────────────────────────────────────────────────────
static const CLSID CLSID_IdhmfisCapture = {
    0xCA1D0001u, 0xDAC0u, 0x1DA0u,
    { 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ShmReader — opens and reads BGR24 frames from the IDHMFIS SHM
// ─────────────────────────────────────────────────────────────────────────────
struct ShmReader {
    HANDLE           map_handle   { nullptr };
    HANDLE           mutex_handle { nullptr };
    const CameraShm* view         { nullptr };

    bool open() {
        if (view) return true;
        map_handle = OpenFileMappingW(FILE_MAP_READ, FALSE, kCamShmName);
        if (!map_handle) return false;
        view = static_cast<const CameraShm*>(
            MapViewOfFile(map_handle, FILE_MAP_READ, 0, 0, sizeof(CameraShm)));
        if (!view) {
            CloseHandle(map_handle); map_handle = nullptr;
            return false;
        }
        mutex_handle = OpenMutexW(SYNCHRONIZE, FALSE, kCamMutexName);
        return true;
    }

    void close() {
        if (view)         { UnmapViewOfFile(view); view = nullptr; }
        if (map_handle)   { CloseHandle(map_handle); map_handle = nullptr; }
        if (mutex_handle) { CloseHandle(mutex_handle); mutex_handle = nullptr; }
    }

    void read_frame(uint8_t* dst) const {
        if (!view) { std::memset(dst, 0, static_cast<size_t>(kCamFrameBytes)); return; }
        if (mutex_handle) WaitForSingleObject(mutex_handle, 100);
        std::memcpy(dst, view->data, static_cast<size_t>(kCamFrameBytes));
        if (mutex_handle) ReleaseMutex(mutex_handle);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
struct CamSource;

// ─────────────────────────────────────────────────────────────────────────────
//  CamStream — IMFMediaStream: delivers BGR24 samples from SHM on demand
// ─────────────────────────────────────────────────────────────────────────────
struct CamStream final : IMFMediaStream {
    volatile LONG        refs_        { 1 };
    std::mutex           cs_;
    IMFMediaEventQueue*  evq_         { nullptr };
    IMFStreamDescriptor* sd_          { nullptr };
    CamSource*           source_      { nullptr }; // weak ref — source owns us
    ShmReader            shm_;
    std::vector<uint8_t> frame_buf_;
    MFTIME               sample_time_ { 0 };
    bool                 shutdown_    { false };

    static constexpr MFTIME kFrameDuration = 10'000'000LL / kCamFPS;

    static HRESULT Create(CamSource* pSrc, IMFStreamDescriptor* pSD, CamStream** ppOut) {
        if (!ppOut) return E_POINTER;
        auto* s = new (std::nothrow) CamStream();
        if (!s) return E_OUTOFMEMORY;
        HRESULT hr = MFCreateEventQueue(&s->evq_);
        if (FAILED(hr)) { delete s; return hr; }
        s->source_    = pSrc; // weak ref
        s->sd_        = pSD;  pSD->AddRef();
        s->frame_buf_.resize(static_cast<size_t>(kCamFrameBytes), 0u);
        *ppOut = s;
        return S_OK;
    }

    // IUnknown
    ULONG   STDMETHODCALLTYPE AddRef() override  { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG   STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IMFMediaEventGenerator) ||
            IsEqualIID(riid, IID_IMFMediaStream)) {
            *ppv = static_cast<IMFMediaStream*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    // IMFMediaEventGenerator — delegate to event queue
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** pp) override
        { return evq_->GetEvent(flags, pp); }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override
        { return evq_->BeginGetEvent(cb, state); }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** pp) override
        { return evq_->EndGetEvent(result, pp); }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID guid, HRESULT status,
                                          const PROPVARIANT* pv) override
        { return evq_->QueueEventParamVar(met, guid, status, pv); }

    // IMFMediaStream
    HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** ppSrc) override;

    HRESULT STDMETHODCALLTYPE GetStreamDescriptor(IMFStreamDescriptor** ppSD) override {
        if (!ppSD) return E_POINTER;
        *ppSD = sd_; sd_->AddRef(); return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RequestSample(IUnknown* pToken) override {
        std::lock_guard<std::mutex> lk(cs_);
        if (shutdown_) return MF_E_SHUTDOWN;

        // Lazy-open SHM — IDHMFIS.exe might start after Capture
        if (!shm_.view) shm_.open();
        shm_.read_frame(frame_buf_.data());

        // Build IMFSample
        IMFSample* pSample = nullptr;
        HRESULT hr = MFCreateSample(&pSample);
        if (FAILED(hr)) return hr;

        IMFMediaBuffer* pBuf = nullptr;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(kCamFrameBytes), &pBuf);
        if (FAILED(hr)) { pSample->Release(); return hr; }

        BYTE* pData = nullptr;
        if (SUCCEEDED(pBuf->Lock(&pData, nullptr, nullptr))) {
            std::memcpy(pData, frame_buf_.data(), static_cast<size_t>(kCamFrameBytes));
            pBuf->Unlock();
        }
        pBuf->SetCurrentLength(static_cast<DWORD>(kCamFrameBytes));

        hr = pSample->AddBuffer(pBuf);
        pBuf->Release();

        if (SUCCEEDED(hr)) {
            pSample->SetSampleTime(sample_time_);
            pSample->SetSampleDuration(kFrameDuration);
            sample_time_ += kFrameDuration;

            if (pToken)
                pSample->SetUnknown(MFSampleExtension_Token, pToken);

            hr = evq_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, pSample);
        }

        pSample->Release();
        return hr;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  CamSource — IMFMediaSource wrapping a single CamStream
// ─────────────────────────────────────────────────────────────────────────────
struct CamSource final : IMFMediaSource {
    volatile LONG               refs_     { 1 };
    std::mutex                  cs_;
    IMFMediaEventQueue*         evq_      { nullptr };
    IMFPresentationDescriptor*  pd_       { nullptr };
    CamStream*                  stream_   { nullptr };
    bool                        shutdown_ { false };
    bool                        started_  { false };

    static HRESULT Create(CamSource** ppOut) {
        if (!ppOut) return E_POINTER;

        // 1. Media type: RGB24, 512×512, 30fps, progressive
        IMFMediaType* pType = nullptr;
        HRESULT hr = MFCreateMediaType(&pType);
        if (FAILED(hr)) return hr;

        struct Guard { IMFMediaType* p; ~Guard() { p->Release(); } } g{pType};

        auto try_set = [&](auto fn) { if (SUCCEEDED(hr)) hr = fn; };
        try_set(pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        try_set(pType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB24));
        try_set(MFSetAttributeSize(pType, MF_MT_FRAME_SIZE,
                                   static_cast<UINT32>(kCamWidth),
                                   static_cast<UINT32>(kCamHeight)));
        try_set(MFSetAttributeRatio(pType, MF_MT_FRAME_RATE,
                                    static_cast<UINT32>(kCamFPS), 1u));
        try_set(MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1u, 1u));
        try_set(pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        try_set(pType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        // Positive stride = bottom-up BGR; Capture normalises internally.
        try_set(pType->SetUINT32(MF_MT_DEFAULT_STRIDE,
                                  static_cast<UINT32>(kCamWidth * kCamBytesPerPx)));
        try_set(pType->SetUINT32(MF_MT_SAMPLE_SIZE,
                                  static_cast<UINT32>(kCamFrameBytes)));
        if (FAILED(hr)) return hr;

        // 2. Stream descriptor → presentation descriptor
        IMFStreamDescriptor* pSD = nullptr;
        hr = MFCreateStreamDescriptor(0, 1, &pType, &pSD);
        if (FAILED(hr)) return hr;

        IMFPresentationDescriptor* pPD = nullptr;
        hr = MFCreatePresentationDescriptor(1, &pSD, &pPD);
        if (SUCCEEDED(hr)) pPD->SelectStream(0);

        // 3. Allocate CamSource
        auto* src = new (std::nothrow) CamSource();
        if (!src) { if (pPD) pPD->Release(); pSD->Release(); return E_OUTOFMEMORY; }

        hr = MFCreateEventQueue(&src->evq_);
        if (SUCCEEDED(hr)) hr = CamStream::Create(src, pSD, &src->stream_);
        pSD->Release();

        if (SUCCEEDED(hr)) {
            src->pd_ = pPD;
        } else {
            if (pPD) pPD->Release();
            src->Release();
            return hr;
        }

        *ppOut = src;
        return S_OK;
    }

    ~CamSource() {
        if (stream_) { stream_->Release(); stream_ = nullptr; }
        if (pd_)     { pd_->Release();     pd_     = nullptr; }
        if (evq_)    { evq_->Release();    evq_    = nullptr; }
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IMFMediaEventGenerator) ||
            IsEqualIID(riid, IID_IMFMediaSource)) {
            *ppv = static_cast<IMFMediaSource*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    // IMFMediaEventGenerator
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** pp) override
        { return evq_->GetEvent(flags, pp); }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override
        { return evq_->BeginGetEvent(cb, state); }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** pp) override
        { return evq_->EndGetEvent(result, pp); }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID guid, HRESULT status,
                                          const PROPVARIANT* pv) override
        { return evq_->QueueEventParamVar(met, guid, status, pv); }

    // IMFMediaSource
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* pdw) override {
        if (!pdw) return E_POINTER;
        *pdw = MFMEDIASOURCE_CAN_PAUSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD) override {
        if (!ppPD) return E_POINTER;
        std::lock_guard<std::mutex> lk(cs_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return pd_->Clone(ppPD);
    }

    HRESULT STDMETHODCALLTYPE Start(IMFPresentationDescriptor* pPD,
                                     const GUID* /*pguidTimeFormat*/,
                                     const PROPVARIANT* /*pvarStartPos*/) override {
        if (!pPD) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(cs_);
        if (shutdown_) return MF_E_SHUTDOWN;

        MediaEventType source_evt = started_ ? MESourceSeeked : MESourceStarted;
        MediaEventType stream_evt = started_ ? MEStreamSeeked : MEStreamStarted;
        started_ = true;

        // Fire MENewStream only on first start
        if (source_evt == MESourceStarted) {
            evq_->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK,
                                     static_cast<IMFMediaStream*>(stream_));
        }

        PROPVARIANT var{};
        evq_->QueueEventParamVar(source_evt, GUID_NULL, S_OK, &var);
        if (stream_) {
            std::lock_guard<std::mutex> slk(stream_->cs_);
            stream_->shutdown_ = false;
            stream_->evq_->QueueEventParamVar(stream_evt, GUID_NULL, S_OK, &var);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop() override {
        std::lock_guard<std::mutex> lk(cs_);
        if (shutdown_) return MF_E_SHUTDOWN;

        if (stream_) {
            std::lock_guard<std::mutex> slk(stream_->cs_);
            stream_->shm_.close();
            PROPVARIANT var{};
            stream_->evq_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, &var);
        }
        PROPVARIANT var{};
        evq_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &var);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Pause() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Shutdown() override {
        std::lock_guard<std::mutex> lk(cs_);
        if (shutdown_) return MF_E_SHUTDOWN;
        shutdown_ = true;

        if (stream_) {
            std::lock_guard<std::mutex> slk(stream_->cs_);
            stream_->shutdown_ = true;
            stream_->shm_.close();
            if (stream_->evq_) stream_->evq_->Shutdown();
        }
        if (evq_) evq_->Shutdown();
        return S_OK;
    }
};

// CamStream::GetMediaSource — needs full CamSource definition
HRESULT STDMETHODCALLTYPE CamStream::GetMediaSource(IMFMediaSource** ppSrc) {
    if (!ppSrc) return E_POINTER;
    std::lock_guard<std::mutex> lk(cs_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *ppSrc = static_cast<IMFMediaSource*>(source_);
    source_->AddRef();
    return S_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CamClassFactory
// ─────────────────────────────────────────────────────────────────────────────
struct CamClassFactory final : IClassFactory {
    volatile LONG refs_{ 1 };

    ULONG   STDMETHODCALLTYPE AddRef()  override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG   STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (pOuter) return CLASS_E_NOAGGREGATION;

        CamSource* pSrc = nullptr;
        HRESULT hr = CamSource::Create(&pSrc);
        if (FAILED(hr)) return hr;

        hr = pSrc->QueryInterface(riid, ppv);
        pSrc->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DLL entry points
// ─────────────────────────────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    if (!IsEqualCLSID(rclsid, CLSID_IdhmfisCapture))
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* cf = new (std::nothrow) CamClassFactory();
    if (!cf) return E_OUTOFMEMORY;
    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

HRESULT STDAPICALLTYPE DllCanUnloadNow() { return S_FALSE; }
