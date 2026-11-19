#pragma once

#include <QObject>
#include <QString>
#include <QImage>
#include <QSize>
#include <QPointer>
#include <QTimer>
#include <QQuickItem>
#include <memory>

class QQmlEngine;
class QQmlComponent;
class QQuickWindow;
class QQuickRenderControl;
class QRhi;
class QRhiTexture;
class QRhiRenderBuffer;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;

/**
 * Abstract interface for virtual display frame sinks:
 * - Linux:   PipeWireFrameSink (pw_stream with SPA_FORMAT_VIDEO_BGRA/RGBA via DMA-BUF / MemFd)
 * - macOS:   SyphonFrameSink   (IOSurface-backed Metal texture via Syphon protocol)
 * - Windows: SpoutFrameSink    (D3D11 shared texture via DXGI handle & Spout2 shared memory)
 * - Future:  DeckLinkFrameSink (Blackmagic SDI Fill + Key bmdFormat8BitARGB)
 */
class IVirtualFrameSink {
public:
    virtual ~IVirtualFrameSink() = default;

    /// Initialize the platform virtual stream/device with the given logical screen name and resolution.
    virtual bool initialize(const QString& screenName, int width, int height) = 0;

    /// Submit a rendered RGBA/BGRA frame from the Qt RHI pipeline.
    virtual void submitFrame(QRhi* rhi, QRhiTexture* texture, const QImage& rgbaImage) = 0;

    /// Cleanly tear down the virtual stream/device.
    virtual void shutdown() = 0;

    /// Returns true if the sink is currently active and streaming/registered.
    virtual bool isActive() const = 0;

    /// Human-readable description of the backend (e.g. "PipeWire (FOHMedia - Audience)").
    virtual QString backendDescription() const = 0;
};

/**
 * Headless 60 FPS Qt Quick renderer that renders LiveOutputWindow.qml offscreen with
 * an 8-bit alpha channel (transparent clear color) and pushes frames to an IVirtualFrameSink.
 */
class VirtualDisplayOutput : public QObject {
    Q_OBJECT

public:
    explicit VirtualDisplayOutput(QQmlEngine* qmlEngine,
                                  const QString& screenName,
                                  int width,
                                  int height,
                                  bool isStage,
                                  QObject* parent = nullptr);
    ~VirtualDisplayOutput() override;

    bool start();
    void stop();

    bool isRunning() const { return m_running; }
    QString screenName() const { return m_screenName; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    QQuickItem* rootObject() const { return m_rootItem; }
    QQuickWindow* quickWindow() const { return m_quickWindow; }
    IVirtualFrameSink* frameSink() const { return m_sink.get(); }
    uint64_t renderedFrameCount() const { return m_renderedFrameCount; }
    QImage lastRenderedFrame() const { return m_lastFrame; }

    /// Force an immediate synchronous frame render and submission.
    void renderNextFrame(bool forceRender = true);

    /// Factory helper to instantiate the current platform's default IVirtualFrameSink.
    static std::unique_ptr<IVirtualFrameSink> createPlatformSink();

private slots:
    void onSceneChanged();
    void onRenderRequested();
    void onFrameTick();

private:
    bool initRhiResources();
    void releaseRhiResources();

    QQmlEngine* m_qmlEngine = nullptr;
    QString m_screenName;
    int m_width = 1920;
    int m_height = 1080;
    bool m_isStage = false;
    bool m_running = false;
    bool m_sceneDirty = true;

    QQuickRenderControl* m_renderControl = nullptr;
    QQuickWindow* m_quickWindow = nullptr;
    QQmlComponent* m_qmlComponent = nullptr;
    QPointer<QQuickItem> m_rootItem;

    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiTexture> m_colorTexture;
    std::unique_ptr<QRhiRenderBuffer> m_depthStencilBuffer;
    std::unique_ptr<QRhiTextureRenderTarget> m_renderTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPassDesc;

    QTimer* m_frameTimer = nullptr;
    std::unique_ptr<IVirtualFrameSink> m_sink;

    uint64_t m_renderedFrameCount = 0;
    QImage m_lastFrame;
};
