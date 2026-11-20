#include "../inc/VirtualDisplayOutput.h"

#if defined(Q_OS_WIN)

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

namespace {

// Standard Spout2 SharedTextureInfo binary layout (280 bytes)
#pragma pack(push, 1)
struct SpoutSharedTextureInfo {
    uint32_t shareHandle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; // DXGI_FORMAT_R8G8B8A8_UNORM (28) or DXGI_FORMAT_B8G8R8A8_UNORM (87)
    uint32_t usage = 0;
    uint8_t  description[256]{};
    uint32_t partnerId = 0;
};
#pragma pack(pop)
static_assert(sizeof(SpoutSharedTextureInfo) == 280, "SpoutSharedTextureInfo must be 280 bytes");

static constexpr size_t kSpoutDefaultMaxSenders = 64;
static constexpr size_t kSpoutMinSenders = 10;
static constexpr size_t kSpoutMaxSendersLimit = 256;
static constexpr size_t kSpoutMaxNameLen = 256;
static constexpr DWORD  kSpoutMutexTimeoutMs = 67;
// Spout2 partnerId bit: GPU/GLDX-compatible texture sharing (bCPU = false, bGLDX = true)
static constexpr uint32_t kSpoutPartnerIdGpuShared = 0x40000000u;

size_t readSpoutMaxSenders() {
    HKEY key = nullptr;
    DWORD value = static_cast<DWORD>(kSpoutDefaultMaxSenders);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Leading Edge\\Spout", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = sizeof(value);
        if (RegQueryValueExA(key, "MaxSenders", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS ||
            type != REG_DWORD) {
            value = static_cast<DWORD>(kSpoutDefaultMaxSenders);
        }
        RegCloseKey(key);
    }
    return std::clamp(static_cast<size_t>(value), kSpoutMinSenders, kSpoutMaxSendersLimit);
}

/**
 * Lightweight Spout2-compatible shared memory segment with "<name>_mutex" synchronization,
 * matching SpoutSharedMemory in leadedge/Spout2.
 */
class SpoutSharedMem {
public:
    SpoutSharedMem() = default;
    ~SpoutSharedMem() { close(); }

    SpoutSharedMem(const SpoutSharedMem&) = delete;
    SpoutSharedMem& operator=(const SpoutSharedMem&) = delete;

    bool create(const char* name, size_t size) {
        close();
        if (!name || !*name || size == 0) return false;

        m_map = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(size),
            name
        );
        if (!m_map) return false;

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            SetLastError(NO_ERROR);
        }

        // Map entire existing or newly-created file mapping object (dwNumberOfBytesToMap = 0)
        m_view = MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!m_view) {
            close();
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(m_view, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.RegionSize > 0) {
            m_mappedSize = mbi.RegionSize;
        } else {
            m_mappedSize = size;
        }

        std::string mutexName(name);
        mutexName += "_mutex";
        m_mutex = CreateMutexA(nullptr, FALSE, mutexName.c_str());
        if (!m_mutex) {
            close();
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            SetLastError(NO_ERROR);
        }
        return true;
    }

    bool isOpen() const {
        return m_map != nullptr && m_view != nullptr && m_mutex != nullptr;
    }

    size_t mappedSize() const {
        return m_mappedSize;
    }

    void* lock(DWORD timeoutMs = kSpoutMutexTimeoutMs) {
        if (!isOpen()) return nullptr;
        const DWORD waitResult = WaitForSingleObject(m_mutex, timeoutMs);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
            return m_view;
        }
        return nullptr;
    }

    void unlock() {
        if (m_mutex) {
            ReleaseMutex(m_mutex);
        }
    }

    void close() {
        if (m_view) {
            UnmapViewOfFile(m_view);
            m_view = nullptr;
        }
        if (m_map) {
            CloseHandle(m_map);
            m_map = nullptr;
        }
        if (m_mutex) {
            CloseHandle(m_mutex);
            m_mutex = nullptr;
        }
        m_mappedSize = 0;
    }

private:
    HANDLE m_map = nullptr;
    void*  m_view = nullptr;
    HANDLE m_mutex = nullptr;
    size_t m_mappedSize = 0;
};

void readSenderSetFromBuffer(const char* buffer, std::set<std::string>& senderNames, size_t maxSenders) {
    senderNames.clear();
    if (!buffer || maxSenders == 0) return;

    for (size_t i = 0; i < maxSenders; ++i) {
        const char* slot = buffer + (i * kSpoutMaxNameLen);
        if (static_cast<unsigned char>(slot[0]) == 0) {
            break;
        }
        const size_t len = strnlen(slot, kSpoutMaxNameLen - 1);
        if (len > 0) {
            senderNames.emplace(slot, len);
        }
    }
}

void writeBufferFromSenderSet(const std::set<std::string>& senderNames, char* buffer, size_t maxSenders) {
    if (!buffer || maxSenders == 0) return;

    size_t i = 0;
    for (const std::string& name : senderNames) {
        if (i >= maxSenders) break;
        char* slot = buffer + (i * kSpoutMaxNameLen);
        const size_t copyLen = std::min(name.size(), kSpoutMaxNameLen - 1);
        std::memcpy(slot, name.data(), copyLen);
        slot[copyLen] = '\0';
        ++i;
    }

    // Spout2 terminates the contiguous sender list with a null byte at the next entry
    if (i < maxSenders) {
        buffer[i * kSpoutMaxNameLen] = '\0';
    }
}

bool sharedInfoExists(const std::string& senderName) {
    if (senderName.empty()) return false;
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, senderName.c_str());
    if (!hMap) return false;
    CloseHandle(hMap);
    return true;
}

/**
 * Windows 11 Spout2 / DXGI Shared Texture Frame Sink with full 8-bit Alpha.
 *
 * Uses Qt 6's native D3D11 device & context (QRhiD3D11NativeHandles) for true zero-copy
 * GPU-to-GPU texture sharing (ID3D11DeviceContext::CopyResource into a D3D11_RESOURCE_MISC_SHARED
 * texture) with requiresCpuReadback() == false, synchronized with Spout2's named mutexes
 * ("<name>_mutex", "<name>_SpoutAccessMutex") and frame counter semaphore
 * ("<name>_Count_Semaphore"), while retaining a lazy standalone D3D11 device +
 * UpdateSubresource fallback when called without a QRhi context (e.g. in unit tests).
 */
class SpoutFrameSink final : public IVirtualFrameSink {
public:
    SpoutFrameSink() = default;
    ~SpoutFrameSink() override {
        shutdown();
    }

    bool requiresCpuReadback() const override {
        return false;
    }

    bool initialize(const QString& screenName, int width, int height) override {
        shutdown();

        m_screenName = screenName;
        m_senderName = ("FOHMedia - " + screenName).left(static_cast<int>(kSpoutMaxNameLen - 1));
        m_width = width > 0 ? width : 1920;
        m_height = height > 0 ? height : 1080;
        m_sharedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        const QByteArray nameBytes = m_senderName.toUtf8();
        const std::string accessMutexName = std::string(nameBytes.constData()) + "_SpoutAccessMutex";
        m_accessMutex = CreateMutexA(nullptr, FALSE, accessMutexName.c_str());
        if (!m_accessMutex) {
            shutdown();
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            SetLastError(NO_ERROR);
        }

        const std::string semName = std::string(nameBytes.constData()) + "_Count_Semaphore";
        m_frameCountSemaphore = CreateSemaphoreA(nullptr, 1, LONG_MAX, semName.c_str());
        if (m_frameCountSemaphore && GetLastError() == ERROR_ALREADY_EXISTS) {
            SetLastError(NO_ERROR);
        }

        if (!registerSpoutSender()) {
            shutdown();
            return false;
        }

        m_active = true;
        return true;
    }

    void submitFrame(QRhi* rhi, QRhiTexture* texture, const QImage& rgbaImage) override {
        if (!m_active) return;

        // Fast path: True zero-copy GPU-to-GPU D3D11 texture copy on Qt's native D3D11 device
        if (rhi && texture && rhi->backend() == QRhi::D3D11) {
            const auto* d3dHandles =
                static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
            auto* qtDevice = d3dHandles ? static_cast<ID3D11Device*>(d3dHandles->dev) : nullptr;
            auto* qtContext = d3dHandles ? static_cast<ID3D11DeviceContext*>(d3dHandles->context) : nullptr;
            auto* srcTex = reinterpret_cast<ID3D11Texture2D*>(
                static_cast<uintptr_t>(texture->nativeTexture().object));

            if (qtDevice && qtContext && srcTex) {
                D3D11_TEXTURE2D_DESC srcDesc{};
                srcTex->GetDesc(&srcDesc);
                if (srcDesc.Width == 0 || srcDesc.Height == 0) return;

                const bool locked = lockTextureAccess();
                if (m_activeDevice != qtDevice ||
                    !m_sharedTexture ||
                    m_sharedFormat != srcDesc.Format ||
                    static_cast<UINT>(m_width) != srcDesc.Width ||
                    static_cast<UINT>(m_height) != srcDesc.Height) {
                    if (!recreateSharedTextureOnDevice(qtDevice,
                                                       srcDesc.Width,
                                                       srcDesc.Height,
                                                       srcDesc.Format)) {
                        if (locked) unlockTextureAccess();
                        return;
                    }
                    updateSpoutSenderInfo();
                }

                qtContext->CopyResource(m_sharedTexture, srcTex);
                qtContext->Flush();
                incrementFrameCount();
                if (locked) unlockTextureAccess();
                return;
            }
        }

        // Fallback path: CPU QImage upload via UpdateSubresource (used when rhi/texture are nullptr in unit tests)
        if (rgbaImage.isNull()) return;
        if (!ensureFallbackDevice()) return;

        const UINT imgWidth = static_cast<UINT>(rgbaImage.width() > 0 ? rgbaImage.width() : m_width);
        const UINT imgHeight = static_cast<UINT>(rgbaImage.height() > 0 ? rgbaImage.height() : m_height);

        const QImage bgra = (rgbaImage.format() == QImage::Format_ARGB32_Premultiplied)
            ? rgbaImage
            : rgbaImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        const bool locked = lockTextureAccess();
        if (m_activeDevice != m_fallbackDevice ||
            !m_sharedTexture ||
            m_sharedFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
            static_cast<UINT>(m_width) != imgWidth ||
            static_cast<UINT>(m_height) != imgHeight) {
            if (!recreateSharedTextureOnDevice(m_fallbackDevice,
                                               imgWidth,
                                               imgHeight,
                                               DXGI_FORMAT_B8G8R8A8_UNORM)) {
                if (locked) unlockTextureAccess();
                return;
            }
            updateSpoutSenderInfo();
        }

        m_fallbackContext->UpdateSubresource(
            m_sharedTexture,
            0,
            nullptr,
            bgra.constBits(),
            static_cast<UINT>(bgra.bytesPerLine()),
            0
        );
        m_fallbackContext->Flush();
        incrementFrameCount();
        if (locked) unlockTextureAccess();
    }

    void shutdown() override {
        m_active = false;
        unregisterSpoutSender();

        if (m_sharedTexture) {
            m_sharedTexture->Release();
            m_sharedTexture = nullptr;
        }
        if (m_activeDevice) {
            m_activeDevice->Release();
            m_activeDevice = nullptr;
        }
        if (m_fallbackContext) {
            m_fallbackContext->Release();
            m_fallbackContext = nullptr;
        }
        if (m_fallbackDevice) {
            m_fallbackDevice->Release();
            m_fallbackDevice = nullptr;
        }
        if (m_frameCountSemaphore) {
            CloseHandle(m_frameCountSemaphore);
            m_frameCountSemaphore = nullptr;
        }
        if (m_accessMutex) {
            CloseHandle(m_accessMutex);
            m_accessMutex = nullptr;
        }
        m_sharedHandle = nullptr;
    }

    bool isActive() const override {
        return m_active;
    }

    QString backendDescription() const override {
        if (m_sharedHandle) {
            return QString("Spout2 DXGI (FOHMedia - %1, Handle 0x%2)")
                .arg(m_screenName)
                .arg(static_cast<qulonglong>(reinterpret_cast<uintptr_t>(m_sharedHandle)), 0, 16);
        }
        return QString("Spout2 DXGI (FOHMedia - %1)").arg(m_screenName);
    }

private:
    bool ensureFallbackDevice() {
        if (m_fallbackDevice && m_fallbackContext) return true;

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL createdLevel{};

        const HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            &m_fallbackDevice,
            &createdLevel,
            &m_fallbackContext
        );
        return SUCCEEDED(hr) && m_fallbackDevice && m_fallbackContext;
    }

    bool lockTextureAccess() {
        if (!m_accessMutex) return false;
        const DWORD waitResult = WaitForSingleObject(m_accessMutex, kSpoutMutexTimeoutMs);
        return (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED);
    }

    void unlockTextureAccess() {
        if (m_accessMutex) {
            ReleaseMutex(m_accessMutex);
        }
    }

    void incrementFrameCount() {
        if (!m_frameCountSemaphore) return;
        if (WaitForSingleObject(m_frameCountSemaphore, 0) == WAIT_OBJECT_0) {
            ReleaseSemaphore(m_frameCountSemaphore, 2, nullptr);
        }
    }

    bool recreateSharedTextureOnDevice(ID3D11Device* device,
                                       UINT width,
                                       UINT height,
                                       DXGI_FORMAT format) {
        if (!device || width == 0 || height == 0) return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        ID3D11Texture2D* newTexture = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &newTexture);
        if (FAILED(hr) || !newTexture) {
            return false;
        }

        IDXGIResource* dxgiRes = nullptr;
        hr = newTexture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiRes));
        if (FAILED(hr) || !dxgiRes) {
            newTexture->Release();
            return false;
        }
        HANDLE newHandle = nullptr;
        hr = dxgiRes->GetSharedHandle(&newHandle);
        dxgiRes->Release();
        if (FAILED(hr) || !newHandle) {
            newTexture->Release();
            return false;
        }

        if (m_sharedTexture) {
            m_sharedTexture->Release();
            m_sharedTexture = nullptr;
        }
        if (m_activeDevice != device) {
            if (m_activeDevice) {
                m_activeDevice->Release();
            }
            m_activeDevice = device;
            m_activeDevice->AddRef();
        }

        m_sharedTexture = newTexture;
        m_sharedHandle = newHandle;
        m_width = static_cast<int>(width);
        m_height = static_cast<int>(height);
        m_sharedFormat = format;
        return true;
    }

    bool updateSpoutSenderInfo() {
        if (!m_senderInfoMem.isOpen()) return false;

        auto* info = static_cast<SpoutSharedTextureInfo*>(m_senderInfoMem.lock());
        if (!info) return false;

        SpoutSharedTextureInfo localInfo{};
        localInfo.shareHandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_sharedHandle));
        localInfo.width = static_cast<uint32_t>(m_width);
        localInfo.height = static_cast<uint32_t>(m_height);
        localInfo.format = static_cast<uint32_t>(m_sharedFormat);
        localInfo.usage = 0;
        localInfo.partnerId = kSpoutPartnerIdGpuShared;

        char exePath[kSpoutMaxNameLen]{};
        const DWORD exeLen = GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(sizeof(exePath) - 1));
        if (exeLen > 0) {
            std::memcpy(localInfo.description, exePath, std::min<size_t>(exeLen, sizeof(localInfo.description) - 1));
        }

        std::memcpy(info, &localInfo, sizeof(SpoutSharedTextureInfo));
        m_senderInfoMem.unlock();
        return true;
    }

    bool registerSpoutSender() {
        const QByteArray nameBytes = m_senderName.toUtf8();
        const std::string senderStr(nameBytes.constData());

        // 1. Create per-sender SharedTextureInfo named shared memory + "<sender>_mutex"
        if (!m_senderInfoMem.create(senderStr.c_str(), sizeof(SpoutSharedTextureInfo))) {
            return false;
        }
        if (!updateSpoutSenderInfo()) {
            return false;
        }

        // 2. Register sender name contiguously in global "SpoutSenderNames" map
        const size_t maxSenders = readSpoutMaxSenders();
        if (m_senderNamesMem.create("SpoutSenderNames", maxSenders * kSpoutMaxNameLen)) {
            if (auto* namesBuf = static_cast<char*>(m_senderNamesMem.lock())) {
                const size_t capacity = m_senderNamesMem.mappedSize() / kSpoutMaxNameLen;
                std::set<std::string> senders;
                readSenderSetFromBuffer(namesBuf, senders, capacity);
                senders.insert(senderStr);
                writeBufferFromSenderSet(senders, namesBuf, capacity);
                m_senderNamesMem.unlock();
            }
        }

        // 3. Set "ActiveSenderName" if none is currently active (enables OBS "Use first available sender")
        if (m_activeSenderMem.create("ActiveSenderName", kSpoutMaxNameLen)) {
            if (auto* activeBuf = static_cast<char*>(m_activeSenderMem.lock())) {
                const size_t currentLen = strnlen(activeBuf, kSpoutMaxNameLen - 1);
                const std::string currentActive(activeBuf, currentLen);
                if (currentActive.empty() || !sharedInfoExists(currentActive)) {
                    const size_t copyLen = std::min(senderStr.size(), kSpoutMaxNameLen - 1);
                    std::memcpy(activeBuf, senderStr.data(), copyLen);
                    activeBuf[copyLen] = '\0';
                }
                m_activeSenderMem.unlock();
            }
        }

        return true;
    }

    void unregisterSpoutSender() {
        const QByteArray nameBytes = m_senderName.toUtf8();
        const std::string senderStr(nameBytes.constData());
        std::string fallbackActiveSender;

        if (m_senderNamesMem.isOpen()) {
            if (auto* namesBuf = static_cast<char*>(m_senderNamesMem.lock())) {
                const size_t capacity = m_senderNamesMem.mappedSize() / kSpoutMaxNameLen;
                std::set<std::string> senders;
                readSenderSetFromBuffer(namesBuf, senders, capacity);
                senders.erase(senderStr);
                writeBufferFromSenderSet(senders, namesBuf, capacity);
                if (!senders.empty()) {
                    fallbackActiveSender = *senders.begin();
                }
                m_senderNamesMem.unlock();
            }
            m_senderNamesMem.close();
        }

        if (m_activeSenderMem.isOpen()) {
            if (auto* activeBuf = static_cast<char*>(m_activeSenderMem.lock())) {
                const size_t currentLen = strnlen(activeBuf, kSpoutMaxNameLen - 1);
                const std::string currentActive(activeBuf, currentLen);
                if (currentActive == senderStr) {
                    if (!fallbackActiveSender.empty()) {
                        const size_t copyLen = std::min(fallbackActiveSender.size(), kSpoutMaxNameLen - 1);
                        std::memcpy(activeBuf, fallbackActiveSender.data(), copyLen);
                        activeBuf[copyLen] = '\0';
                    } else {
                        activeBuf[0] = '\0';
                    }
                }
                m_activeSenderMem.unlock();
            }
            m_activeSenderMem.close();
        }

        m_senderInfoMem.close();
    }

    QString m_screenName;
    QString m_senderName;
    int m_width = 1920;
    int m_height = 1080;

    ID3D11Device* m_fallbackDevice = nullptr;
    ID3D11DeviceContext* m_fallbackContext = nullptr;
    ID3D11Device* m_activeDevice = nullptr;
    ID3D11Texture2D* m_sharedTexture = nullptr;
    DXGI_FORMAT m_sharedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    HANDLE m_sharedHandle = nullptr;
    HANDLE m_accessMutex = nullptr;
    HANDLE m_frameCountSemaphore = nullptr;
    SpoutSharedMem m_senderInfoMem;
    SpoutSharedMem m_senderNamesMem;
    SpoutSharedMem m_activeSenderMem;
    bool m_active = false;
};

} // namespace

std::unique_ptr<IVirtualFrameSink> VirtualDisplayOutput::createPlatformSink() {
    return std::make_unique<SpoutFrameSink>();
}

#endif // Q_OS_WIN
