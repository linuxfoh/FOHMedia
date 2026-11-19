#include "../inc/VirtualDisplayOutput.h"

#if defined(Q_OS_WIN)

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstring>
#include <cstdint>

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

static constexpr size_t kSpoutMaxSenders = 64;
static constexpr size_t kSpoutMaxNameLen = 256;

/**
 * Windows 11 Spout2 / DXGI Shared Texture Frame Sink with full 8-bit Alpha.
 *
 * Uses Qt 6's native D3D11 device & context (QRhiD3D11NativeHandles) for true zero-copy
 * GPU-to-GPU texture sharing (ID3D11DeviceContext::CopyResource into a D3D11_RESOURCE_MISC_SHARED
 * texture) with requiresCpuReadback() == false, while retaining a standalone D3D11 device +
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
        m_senderName = ("FOHMedia - " + screenName).left(255);
        m_width = width > 0 ? width : 1920;
        m_height = height > 0 ? height : 1080;

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL createdLevel{};

        HRESULT hr = D3D11CreateDevice(
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
        if (FAILED(hr) || !m_fallbackDevice) {
            return false;
        }

        if (!recreateSharedTextureOnDevice(m_fallbackDevice,
                                           static_cast<UINT>(m_width),
                                           static_cast<UINT>(m_height),
                                           DXGI_FORMAT_B8G8R8A8_UNORM)) {
            shutdown();
            return false;
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

                if (m_activeDevice != qtDevice ||
                    !m_sharedTexture ||
                    m_sharedFormat != srcDesc.Format ||
                    static_cast<UINT>(m_width) != srcDesc.Width ||
                    static_cast<UINT>(m_height) != srcDesc.Height) {
                    if (!recreateSharedTextureOnDevice(qtDevice,
                                                       srcDesc.Width,
                                                       srcDesc.Height,
                                                       srcDesc.Format)) {
                        return;
                    }
                    updateSpoutSenderInfo();
                }

                qtContext->CopyResource(m_sharedTexture, srcTex);
                qtContext->Flush();
                return;
            }
        }

        // Fallback path: CPU QImage upload via UpdateSubresource (used when rhi/texture are nullptr in unit tests)
        if (rgbaImage.isNull() || !m_fallbackDevice || !m_fallbackContext) return;

        if (m_activeDevice != m_fallbackDevice ||
            !m_sharedTexture ||
            m_sharedFormat != DXGI_FORMAT_B8G8R8A8_UNORM) {
            if (!recreateSharedTextureOnDevice(m_fallbackDevice,
                                               static_cast<UINT>(m_width),
                                               static_cast<UINT>(m_height),
                                               DXGI_FORMAT_B8G8R8A8_UNORM)) {
                return;
            }
            updateSpoutSenderInfo();
        }

        QImage bgra = (rgbaImage.format() == QImage::Format_ARGB32_Premultiplied)
            ? rgbaImage
            : rgbaImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        m_fallbackContext->UpdateSubresource(
            m_sharedTexture,
            0,
            nullptr,
            bgra.constBits(),
            static_cast<UINT>(bgra.bytesPerLine()),
            0
        );
        m_fallbackContext->Flush();
    }

    void shutdown() override {
        m_active = false;
        unregisterSpoutSender();

        if (m_sharedTexture) {
            m_sharedTexture->Release();
            m_sharedTexture = nullptr;
        }
        if (m_fallbackContext) {
            m_fallbackContext->Release();
            m_fallbackContext = nullptr;
        }
        if (m_fallbackDevice) {
            m_fallbackDevice->Release();
            m_fallbackDevice = nullptr;
        }
        m_activeDevice = nullptr;
        m_sharedHandle = nullptr;
    }

    bool isActive() const override {
        return m_active;
    }

    QString backendDescription() const override {
        return QString("Spout2 DXGI (FOHMedia - %1)").arg(m_screenName);
    }

private:
    bool recreateSharedTextureOnDevice(ID3D11Device* device,
                                       UINT width,
                                       UINT height,
                                       DXGI_FORMAT format) {
        if (!device || width == 0 || height == 0) return false;

        if (m_sharedTexture) {
            m_sharedTexture->Release();
            m_sharedTexture = nullptr;
        }
        m_sharedHandle = nullptr;

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

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_sharedTexture);
        if (FAILED(hr) || !m_sharedTexture) {
            return false;
        }

        IDXGIResource* dxgiRes = nullptr;
        hr = m_sharedTexture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiRes));
        if (FAILED(hr) || !dxgiRes) {
            m_sharedTexture->Release();
            m_sharedTexture = nullptr;
            return false;
        }
        hr = dxgiRes->GetSharedHandle(&m_sharedHandle);
        dxgiRes->Release();
        if (FAILED(hr) || !m_sharedHandle) {
            m_sharedTexture->Release();
            m_sharedTexture = nullptr;
            return false;
        }

        m_activeDevice = device;
        m_width = static_cast<int>(width);
        m_height = static_cast<int>(height);
        m_sharedFormat = format;
        return true;
    }

    bool updateSpoutSenderInfo() {
        if (!m_senderInfoMap) return false;

        auto* info = static_cast<SpoutSharedTextureInfo*>(
            MapViewOfFile(m_senderInfoMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SpoutSharedTextureInfo))
        );
        if (!info) return false;

        std::memset(info, 0, sizeof(SpoutSharedTextureInfo));
        info->shareHandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_sharedHandle));
        info->width = static_cast<uint32_t>(m_width);
        info->height = static_cast<uint32_t>(m_height);
        info->format = static_cast<uint32_t>(m_sharedFormat);
        UnmapViewOfFile(info);
        return true;
    }

    bool registerSpoutSender() {
        QByteArray nameBytes = m_senderName.toUtf8();

        // 1. Create per-sender SharedTextureInfo named shared memory
        m_senderInfoMap = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            sizeof(SpoutSharedTextureInfo),
            nameBytes.constData()
        );
        if (!m_senderInfoMap) return false;

        if (!updateSpoutSenderInfo()) {
            return false;
        }

        // 2. Register sender name in global "SpoutSenderNames" map
        const DWORD namesMapSize = static_cast<DWORD>(kSpoutMaxSenders * kSpoutMaxNameLen);
        m_senderNamesMap = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            namesMapSize,
            "SpoutSenderNames"
        );
        if (m_senderNamesMap) {
            auto* namesBuf = static_cast<char*>(
                MapViewOfFile(m_senderNamesMap, FILE_MAP_ALL_ACCESS, 0, 0, namesMapSize)
            );
            if (namesBuf) {
                bool exists = false;
                int emptySlot = -1;
                for (size_t i = 0; i < kSpoutMaxSenders; ++i) {
                    char* slot = namesBuf + (i * kSpoutMaxNameLen);
                    if (slot[0] == '\0') {
                        if (emptySlot < 0) emptySlot = static_cast<int>(i);
                    } else if (std::strncmp(slot, nameBytes.constData(), kSpoutMaxNameLen - 1) == 0) {
                        exists = true;
                        break;
                    }
                }
                if (!exists && emptySlot >= 0) {
                    char* slot = namesBuf + (static_cast<size_t>(emptySlot) * kSpoutMaxNameLen);
                    std::memset(slot, 0, kSpoutMaxNameLen);
                    std::strncpy(slot, nameBytes.constData(), kSpoutMaxNameLen - 1);
                }
                UnmapViewOfFile(namesBuf);
            }
        }
        return true;
    }

    void unregisterSpoutSender() {
        QByteArray nameBytes = m_senderName.toUtf8();
        if (m_senderNamesMap) {
            const DWORD namesMapSize = static_cast<DWORD>(kSpoutMaxSenders * kSpoutMaxNameLen);
            auto* namesBuf = static_cast<char*>(
                MapViewOfFile(m_senderNamesMap, FILE_MAP_ALL_ACCESS, 0, 0, namesMapSize)
            );
            if (namesBuf) {
                for (size_t i = 0; i < kSpoutMaxSenders; ++i) {
                    char* slot = namesBuf + (i * kSpoutMaxNameLen);
                    if (std::strncmp(slot, nameBytes.constData(), kSpoutMaxNameLen - 1) == 0) {
                        std::memset(slot, 0, kSpoutMaxNameLen);
                    }
                }
                UnmapViewOfFile(namesBuf);
            }
            CloseHandle(m_senderNamesMap);
            m_senderNamesMap = nullptr;
        }
        if (m_senderInfoMap) {
            CloseHandle(m_senderInfoMap);
            m_senderInfoMap = nullptr;
        }
    }

    QString m_screenName;
    QString m_senderName;
    int m_width = 1920;
    int m_height = 1080;

    ID3D11Device* m_fallbackDevice = nullptr;
    ID3D11DeviceContext* m_fallbackContext = nullptr;
    ID3D11Device* m_activeDevice = nullptr;
    ID3D11Texture2D* m_sharedTexture = nullptr;
    DXGI_FORMAT m_sharedFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    HANDLE m_sharedHandle = nullptr;
    HANDLE m_senderInfoMap = nullptr;
    HANDLE m_senderNamesMap = nullptr;
    bool m_active = false;
};

} // namespace

std::unique_ptr<IVirtualFrameSink> VirtualDisplayOutput::createPlatformSink() {
    return std::make_unique<SpoutFrameSink>();
}

#endif // Q_OS_WIN
