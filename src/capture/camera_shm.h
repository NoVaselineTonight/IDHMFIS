#pragma once
// camera_shm.h — Named shared memory layout between IDHMFIS.exe (producer)
// and IDHMFISCapture.dll (consumer / DirectShow push-source).
//
// IDHMFIS.exe creates the mapping with CreateFileMapping.
// IDHMFISCapture.dll opens it with OpenFileMapping in DllGetClassObject.
//
// Thread safety: the producer writes data[] then increments sequence.
// The consumer polls sequence; when it changes, it copies data[] and delivers.

#pragma once
#include <cstdint>

static constexpr int kCamWidth      = 512;
static constexpr int kCamHeight     = 512;
static constexpr int kCamBytesPerPx = 3;   // BGR24
static constexpr int kCamStride     = kCamWidth * kCamBytesPerPx;
static constexpr int kCamFrameBytes = kCamHeight * kCamStride;
static constexpr int kCamFPS        = 30;

static constexpr const wchar_t* kCamShmName    = L"Local\\IDHMFISCameraFrame";
static constexpr const wchar_t* kCamEventName  = L"Local\\IDHMFISCameraEvent";
static constexpr const wchar_t* kCamMutexName  = L"Local\\IDHMFISCameraMutex";
static constexpr const char*    kCamFriendlyName = "IDHMFIS Laser Preview";

#pragma pack(push, 4)
struct CameraShm {
    volatile uint32_t sequence;         // incremented by producer after writing data
    uint8_t           data[kCamFrameBytes]; // BGR24 bottom-up row order
};
#pragma pack(pop)
