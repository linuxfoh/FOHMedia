#include "../inc/VirtualDisplayOutput.h"

#if defined(Q_OS_MACOS)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import "SyphonMetalServer.h"
#import "SyphonServerDirectory.h"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <algorithm>
#include <cstring>

namespace {

/**
 * macOS Virtual Display Frame Sink using SyphonMetalServer + Metal + IOSurface with full 8-bit Alpha.
 *
 * Publishes rendered Qt Quick RHI Metal textures (id<MTLTexture>) directly on the GPU command queue
 * to a SyphonMetalServer with zero CPU readback (requiresCpuReadback() == false).
 * OBS Studio on macOS discovers the server automatically via its built-in "Syphon Client" source
 * as "[FOHMedia] <ScreenName>" with "Allow Transparency" support.
 */
class SyphonFrameSink final : public IVirtualFrameSink {
public:
    SyphonFrameSink() = default;
    ~SyphonFrameSink() override {
        shutdown();
    }

    bool requiresCpuReadback() const override {
        return false;
    }

    bool initialize(const QString& screenName, int width, int height) override {
        @autoreleasepool {
            shutdown();

            m_screenName = screenName;
            m_width = width > 0 ? width : 1920;
            m_height = height > 0 ? height : 1080;

            m_mtlDevice = MTLCreateSystemDefaultDevice();
            if (!m_mtlDevice) {
                return false;
            }

            m_cmdQueue = [m_mtlDevice newCommandQueue];
            if (!m_cmdQueue) {
                shutdown();
                return false;
            }

            m_syphonServer = [[SyphonMetalServer alloc] initWithName:m_screenName.toNSString()
                                                              device:m_mtlDevice
                                                             options:nil];
            if (!m_syphonServer) {
                shutdown();
                return false;
            }

            // Prevent macOS App Nap and timer coalescing when FOHMedia is in the background
            // or on a different macOS Space (e.g., while OBS is focused).
            NSActivityOptions activityOpts =
                NSActivityUserInitiated |
                NSActivityLatencyCritical |
                NSActivityAutomaticTerminationDisabled;
            m_activityToken = [[NSProcessInfo processInfo]
                beginActivityWithOptions:activityOpts
                                  reason:@"FOHMedia Virtual Display 60 FPS Syphon Output"];

            if (!ensureFallbackTexture()) {
                shutdown();
                return false;
            }

            // Publish an initial transparent frame so the Syphon server's IOSurface is immediately
            // allocated and announced to Syphon clients (including OBS Studio).
            id<MTLCommandBuffer> cmdBuf = [m_cmdQueue commandBuffer];
            if (cmdBuf) {
                [m_syphonServer publishFrameTexture:m_fallbackTexture
                                    onCommandBuffer:cmdBuf
                                        imageRegion:NSMakeRect(0, 0, m_width, m_height)
                                            flipped:YES];
                [cmdBuf commit];
                [cmdBuf waitUntilCompleted];
            }

            updateSurfaceId();
            m_active = true;
            return true;
        }
    }

    void submitFrame(QRhi* rhi, QRhiTexture* texture, const QImage& rgbaImage) override {
        if (!m_active || !m_syphonServer) return;

        @autoreleasepool {
            // Fast path: True zero-copy GPU submission directly from Qt 6's Metal RHI texture
            if (rhi && texture && rhi->backend() == QRhi::Metal) {
                const auto* metalHandles =
                    static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
                id<MTLDevice> rhiDevice = (metalHandles && metalHandles->dev)
                    ? (__bridge id<MTLDevice>)(__bridge void*)metalHandles->dev
                    : m_mtlDevice;
                id<MTLCommandQueue> rhiCmdQueue = (metalHandles && metalHandles->cmdQueue)
                    ? (__bridge id<MTLCommandQueue>)(__bridge void*)metalHandles->cmdQueue
                    : m_cmdQueue;
                id<MTLTexture> srcTexture =
                    (__bridge id<MTLTexture>)reinterpret_cast<void*>(
                        static_cast<uintptr_t>(texture->nativeTexture().object));

                if (rhiDevice && m_syphonServer.device != rhiDevice) {
                    [m_syphonServer stop];
                    m_mtlDevice = rhiDevice;
                    m_cmdQueue = rhiCmdQueue ? rhiCmdQueue : [m_mtlDevice newCommandQueue];
                    m_fallbackTexture = nil;
                    m_syphonServer = [[SyphonMetalServer alloc] initWithName:m_screenName.toNSString()
                                                                      device:m_mtlDevice
                                                                     options:nil];
                }

                if (srcTexture && rhiCmdQueue && m_syphonServer) {
                    id<MTLCommandBuffer> cmdBuf = [rhiCmdQueue commandBuffer];
                    if (cmdBuf) {
                        [m_syphonServer publishFrameTexture:srcTexture
                                            onCommandBuffer:cmdBuf
                                                imageRegion:NSMakeRect(0, 0, m_width, m_height)
                                                    flipped:YES];
                        [cmdBuf commit];
                        updateSurfaceId();
                        return;
                    }
                }
            }

            // Fallback path: CPU QImage upload into Metal texture (used when rhi/texture are nullptr in unit tests)
            if (rgbaImage.isNull() || !m_mtlDevice || !m_cmdQueue) return;
            if (!ensureFallbackTexture()) return;

            QImage rgba = (rgbaImage.format() == QImage::Format_RGBA8888_Premultiplied ||
                           rgbaImage.format() == QImage::Format_RGBA8888)
                ? rgbaImage
                : rgbaImage.convertToFormat(QImage::Format_RGBA8888_Premultiplied);

            const NSUInteger copyW = static_cast<NSUInteger>(std::min(m_width, rgba.width()));
            const NSUInteger copyH = static_cast<NSUInteger>(std::min(m_height, rgba.height()));
            [m_fallbackTexture replaceRegion:MTLRegionMake2D(0, 0, copyW, copyH)
                                 mipmapLevel:0
                                   withBytes:rgba.constBits()
                                 bytesPerRow:static_cast<NSUInteger>(rgba.bytesPerLine())];

            id<MTLCommandBuffer> cmdBuf = [m_cmdQueue commandBuffer];
            if (cmdBuf) {
                [m_syphonServer publishFrameTexture:m_fallbackTexture
                                    onCommandBuffer:cmdBuf
                                        imageRegion:NSMakeRect(0, 0, m_width, m_height)
                                            flipped:YES];
                [cmdBuf commit];
                [cmdBuf waitUntilCompleted];
                updateSurfaceId();
            }
        }
    }

    void shutdown() override {
        @autoreleasepool {
            m_active = false;
            m_surfaceId = 0;
            if (m_activityToken) {
                [[NSProcessInfo processInfo] endActivity:m_activityToken];
                m_activityToken = nil;
            }
            if (m_syphonServer) {
                [m_syphonServer stop];
                m_syphonServer = nil;
            }
            m_fallbackTexture = nil;
            m_cmdQueue = nil;
            m_mtlDevice = nil;
        }
    }

    bool isActive() const override {
        return m_active && (m_syphonServer != nil);
    }

    QString backendDescription() const override {
        return QString("Syphon Metal (FOHMedia - %1, ID %2)").arg(m_screenName).arg(m_surfaceId);
    }

private:
    bool ensureFallbackTexture() {
        if (m_fallbackTexture &&
            static_cast<int>(m_fallbackTexture.width) == m_width &&
            static_cast<int>(m_fallbackTexture.height) == m_height) {
            return true;
        }
        if (!m_mtlDevice) return false;

        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:static_cast<NSUInteger>(m_width)
                                                              height:static_cast<NSUInteger>(m_height)
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        m_fallbackTexture = [m_mtlDevice newTextureWithDescriptor:desc];
        return (m_fallbackTexture != nil);
    }

    void updateSurfaceId() {
        if (!m_syphonServer) return;
        id<MTLTexture> frameTex = [m_syphonServer newFrameImage];
        if (frameTex && frameTex.iosurface) {
            m_surfaceId = IOSurfaceGetID(frameTex.iosurface);
        }
    }

    QString m_screenName;
    int m_width = 1920;
    int m_height = 1080;
    id<MTLDevice> m_mtlDevice = nil;
    id<MTLCommandQueue> m_cmdQueue = nil;
    id<MTLTexture> m_fallbackTexture = nil;
    SyphonMetalServer* m_syphonServer = nil;
    id<NSObject> m_activityToken = nil;
    IOSurfaceID m_surfaceId = 0;
    bool m_active = false;
};

} // namespace

std::unique_ptr<IVirtualFrameSink> VirtualDisplayOutput::createPlatformSink() {
    return std::make_unique<SyphonFrameSink>();
}

#endif // Q_OS_MACOS
