#include "../inc/VirtualDisplayOutput.h"

#if defined(Q_OS_WIN)

#include <rhi/qrhi.h>
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
    uint32_t format = 0; // DXGI_FORMAT_B8G8R8A8_UNORM (87)
    uint32_t usage = 0;
    uint8_t  description[256]{};
    uint32_t partnerId = 0;
};
#pragma pack(pop)

static constexpr size_t kSpoutMaxSenders = 64;
static constexpr size_t kSpoutMaxNameLen = 256;

/**
 * Windows 11 Spout2 / DXGI Shared Texture Frame Sink with full 8-bit BGRA Alpha.
 * Allocates a D3D11 shared texture (D3D11_RESOURCE_MISC_SHARED, DXGI_FORMAT_B8G8R8A8_UNORM)
 * and publishes its DXGI shared handle in the standard Spout2 named shared memory map so
 * OBS Studio (via obs-spout2-plugin) captures "FOHMedia - <ScreenName>" with zero-copy GPU alpha.
 */
class SpoutFrameSink final : public IVirtualFrameSink {
public:
    SpoutFrameSink() = default;
    ~SpoutFrameSink() override {
        shutdown();
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
            &m_device,
            &createdLevel,
            &m_context
        );
        if (FAILED(hr) || !m_device) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(m_width);
        desc.Height = static_cast<UINT>(m_height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        hr = m_device->CreateTexture2D(&desc, nullptr, &m_sharedTexture);
        if (FAILED(hr) || !m_sharedTexture) {
            shutdown();
            return false;
        }

        IDXGIResource* dxgiRes = nullptr;
        hr = m_sharedTexture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiRes));
        if (FAILED(hr) || !dxgiRes) {
            shutdown();
            return false;
        }
        hr = dxgiRes->GetSharedHandle(&m_sharedHandle);
        dxgiRes->Release();
        if (FAILED(hr) || !m_sharedHandle) {
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

    void submitFrame(QRhi* /*rhi*/, QRhiTexture* /*texture*/, const QImage& rgbaImage) override {
        if (!m_active || !m_context || !m_sharedTexture || rgbaImage.isNull()) return;

        QImage bgra = (rgbaImage.format() == QImage::Format_ARGB32_Premultiplied)
            ? rgbaImage
            : rgbaImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        m_context->UpdateSubresource(
            m_sharedTexture,
            0,
            nullptr,
            bgra.constBits(),
            static_cast<UINT>(bgra.bytesPerLine()),
            0
        );
        m_context->Flush();
    }

    void shutdown() override {
        m_active = false;
        unregisterSpoutSender();

        if (m_sharedTexture) {
            m_sharedTexture->Release();
            m_sharedTexture = nullptr;
        }
        if (m_context) {
            m_context->Release();
            m_context = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
        m_sharedHandle = nullptr;
    }

    bool isActive() const override {
        return m_active;
    }

    QString backendDescription() const override {
        return QString("Spout2 DXGI (FOHMedia - %1)").arg(m_screenName);
    }

private:
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

        auto* info = static_cast<SpoutSharedTextureInfo*>(
            MapViewOfFile(m_senderInfoMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SpoutSharedTextureInfo))
        );
        if (!info) return false;

        std::memset(info, 0, sizeof(SpoutSharedTextureInfo));
        info->shareHandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_sharedHandle));
        info->width = static_cast<uint32_t>(m_width);
        info->height = static_cast<uint32_t>(m_height);
        info->format = static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
        UnmapViewOfFile(info);

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

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    ID3D11Texture2D* m_sharedTexture = nullptr;
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
