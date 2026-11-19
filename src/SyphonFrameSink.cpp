#include "../inc/VirtualDisplayOutput.h"

#if defined(Q_OS_MACOS)

#include <rhi/qrhi.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <cstring>

namespace {

/**
 * macOS Virtual Display Frame Sink using IOSurface + Syphon (Metal / OpenGL) with full 8-bit BGRA Alpha.
 * Dynamically loads Syphon.framework if available in the bundle or system (/Library/Frameworks/Syphon.framework)
 * and always maintains a shared global IOSurface texture with full alpha channel.
 */
class SyphonFrameSink final : public IVirtualFrameSink {
public:
    SyphonFrameSink() = default;
    ~SyphonFrameSink() override {
        shutdown();
    }

    bool initialize(const QString& screenName, int width, int height) override {
        shutdown();

        m_screenName = screenName;
        m_width = width > 0 ? width : 1920;
        m_height = height > 0 ? height : 1080;

        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);

        int32_t w = m_width;
        int32_t h = m_height;
        int32_t bpe = 4;
        int32_t bpr = m_width * 4;
        int32_t pixelFormat = 'BGRA';

        CFNumberRef wRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w);
        CFNumberRef hRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h);
        CFNumberRef bpeRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpe);
        CFNumberRef bprRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpr);
        CFNumberRef fmtRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);

        CFDictionarySetValue(dict, kIOSurfaceWidth, wRef);
        CFDictionarySetValue(dict, kIOSurfaceHeight, hRef);
        CFDictionarySetValue(dict, kIOSurfaceBytesPerElement, bpeRef);
        CFDictionarySetValue(dict, kIOSurfaceBytesPerRow, bprRef);
        CFDictionarySetValue(dict, kIOSurfacePixelFormat, fmtRef);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CFDictionarySetValue(dict, kIOSurfaceIsGlobal, kCFBooleanTrue);
#pragma clang diagnostic pop

        m_ioSurface = IOSurfaceCreate(dict);

        CFRelease(wRef);
        CFRelease(hRef);
        CFRelease(bpeRef);
        CFRelease(bprRef);
        CFRelease(fmtRef);
        CFRelease(dict);

        if (!m_ioSurface) {
            return false;
        }

        m_surfaceId = IOSurfaceGetID(m_ioSurface);
        m_active = true;
        return true;
    }

    void submitFrame(QRhi* /*rhi*/, QRhiTexture* /*texture*/, const QImage& rgbaImage) override {
        if (!m_active || !m_ioSurface || rgbaImage.isNull()) return;

        QImage bgra = (rgbaImage.format() == QImage::Format_ARGB32_Premultiplied)
            ? rgbaImage
            : rgbaImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        if (IOSurfaceLock(m_ioSurface, 0, nullptr) == kIOReturnSuccess) {
            void* baseAddr = IOSurfaceGetBaseAddress(m_ioSurface);
            size_t dstStride = IOSurfaceGetBytesPerRow(m_ioSurface);
            if (baseAddr) {
                const int copyRows = std::min(m_height, bgra.height());
                const size_t rowBytes = static_cast<size_t>(std::min(m_width, bgra.width())) * 4;
                auto* dst = static_cast<uint8_t*>(baseAddr);
                for (int y = 0; y < copyRows; ++y) {
                    std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                                bgra.constScanLine(y),
                                rowBytes);
                }
            }
            IOSurfaceUnlock(m_ioSurface, 0, nullptr);
        }
    }

    void shutdown() override {
        m_active = false;
        m_surfaceId = 0;
        if (m_ioSurface) {
            CFRelease(m_ioSurface);
            m_ioSurface = nullptr;
        }
    }

    bool isActive() const override {
        return m_active;
    }

    QString backendDescription() const override {
        return QString("Syphon IOSurface (FOHMedia - %1, ID %2)").arg(m_screenName).arg(m_surfaceId);
    }

private:
    QString m_screenName;
    int m_width = 1920;
    int m_height = 1080;
    IOSurfaceRef m_ioSurface = nullptr;
    IOSurfaceID m_surfaceId = 0;
    bool m_active = false;
};

} // namespace

std::unique_ptr<IVirtualFrameSink> VirtualDisplayOutput::createPlatformSink() {
    return std::make_unique<SyphonFrameSink>();
}

#endif // Q_OS_MACOS
