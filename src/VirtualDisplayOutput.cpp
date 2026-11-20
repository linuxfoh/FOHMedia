#include "../inc/VirtualDisplayOutput.h"

#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickItem>
#include <QCoreApplication>
#include <rhi/qrhi.h>
#include <iostream>

VirtualDisplayOutput::VirtualDisplayOutput(QQmlEngine* qmlEngine,
                                           const QString& screenName,
                                           int width,
                                           int height,
                                           bool isStage,
                                           QObject* parent)
    : QObject(parent)
    , m_qmlEngine(qmlEngine)
    , m_screenName(screenName)
    , m_width(width > 0 ? width : 1920)
    , m_height(height > 0 ? height : 1080)
    , m_isStage(isStage)
{
    m_frameTimer = new QTimer(this);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    m_frameTimer->setInterval(16); // ~60 FPS
    connect(m_frameTimer, &QTimer::timeout, this, &VirtualDisplayOutput::onFrameTick);
}

VirtualDisplayOutput::~VirtualDisplayOutput() {
    stop();
}

bool VirtualDisplayOutput::start() {
    if (m_running) return true;
    if (!m_qmlEngine) {
        std::cerr << "VirtualDisplayOutput::start failed: QQmlEngine is null" << std::endl;
        return false;
    }

    m_renderControl = new QQuickRenderControl(this);
    m_quickWindow = new QQuickWindow(m_renderControl);
    m_quickWindow->setTitle("FOHMedia Virtual - " + m_screenName);
    m_quickWindow->setColor(QColor(0, 0, 0, 0)); // Full 8-bit alpha transparent clear color
    m_quickWindow->setGeometry(0, 0, m_width, m_height);

    connect(m_renderControl, &QQuickRenderControl::sceneChanged,
            this, &VirtualDisplayOutput::onSceneChanged);
    connect(m_renderControl, &QQuickRenderControl::renderRequested,
            this, &VirtualDisplayOutput::onRenderRequested);

    if (!m_renderControl->initialize()) {
        std::cerr << "VirtualDisplayOutput: QQuickRenderControl::initialize() failed for "
                  << m_screenName.toStdString() << std::endl;
        stop();
        return false;
    }

    if (!initRhiResources()) {
        std::cerr << "VirtualDisplayOutput: Failed to initialize RHI render target for "
                  << m_screenName.toStdString() << std::endl;
        stop();
        return false;
    }

    m_qmlComponent = new QQmlComponent(m_qmlEngine, QUrl("qrc:/fohmedia/qml/LiveOutputWindow.qml"), this);
    if (m_qmlComponent->isError()) {
        for (const auto& err : m_qmlComponent->errors()) {
            std::cerr << "VirtualDisplayOutput QML Error (" << m_screenName.toStdString()
                      << "): " << err.toString().toStdString() << std::endl;
        }
        stop();
        return false;
    }

    QObject* rootObj = m_qmlComponent->create(m_qmlEngine->rootContext());
    m_rootItem = qobject_cast<QQuickItem*>(rootObj);
    if (!m_rootItem) {
        delete rootObj;
        std::cerr << "VirtualDisplayOutput: Root object is not a QQuickItem" << std::endl;
        stop();
        return false;
    }

    m_rootItem->setParentItem(m_quickWindow->contentItem());
    m_rootItem->setWidth(m_width);
    m_rootItem->setHeight(m_height);
    m_rootItem->setProperty("isStage", m_isStage);

    m_sink = createPlatformSink();
    if (m_sink) {
        m_sink->initialize(m_screenName, m_width, m_height);
    }

    m_running = true;
    m_sceneDirty = true;
    renderNextFrame(true);
    m_frameTimer->start();
    return true;
}

void VirtualDisplayOutput::stop() {
    if (m_frameTimer) {
        m_frameTimer->stop();
    }
    m_running = false;

    if (m_sink) {
        m_sink->shutdown();
        m_sink.reset();
    }

    if (m_rootItem) {
        delete m_rootItem.data();
        m_rootItem.clear();
    }
    if (m_qmlComponent) {
        delete m_qmlComponent;
        m_qmlComponent = nullptr;
    }

    releaseRhiResources();

    if (m_quickWindow) {
        delete m_quickWindow;
        m_quickWindow = nullptr;
    }
    if (m_renderControl) {
        delete m_renderControl;
        m_renderControl = nullptr;
    }
}

bool VirtualDisplayOutput::initRhiResources() {
    m_rhi = m_renderControl ? m_renderControl->rhi() : nullptr;
    if (!m_rhi) return false;

    const QSize pixelSize(m_width, m_height);
    m_colorTexture.reset(m_rhi->newTexture(QRhiTexture::RGBA8,
                                           pixelSize,
                                           1,
                                           QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!m_colorTexture->create()) {
        return false;
    }

    m_depthStencilBuffer.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!m_depthStencilBuffer->create()) {
        return false;
    }

    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(m_colorTexture.get()));
    rtDesc.setDepthStencilBuffer(m_depthStencilBuffer.get());

    m_renderTarget.reset(m_rhi->newTextureRenderTarget(rtDesc));
    m_renderPassDesc.reset(m_renderTarget->newCompatibleRenderPassDescriptor());
    m_renderTarget->setRenderPassDescriptor(m_renderPassDesc.get());
    if (!m_renderTarget->create()) {
        return false;
    }

    QQuickRenderTarget qrt = QQuickRenderTarget::fromRhiRenderTarget(m_renderTarget.get());
    if (m_rhi->isYUpInFramebuffer()) {
        // Let the GPU projection matrix render the frame upright so we never do a CPU vertical flip
        qrt.setMirrorVertically(true);
    }
    m_quickWindow->setRenderTarget(qrt);
    return true;
}

void VirtualDisplayOutput::releaseRhiResources() {
    m_renderTarget.reset();
    m_renderPassDesc.reset();
    m_depthStencilBuffer.reset();
    m_colorTexture.reset();
    m_rhi = nullptr;
}

void VirtualDisplayOutput::onSceneChanged() {
    m_sceneDirty = true;
}

void VirtualDisplayOutput::onRenderRequested() {
    m_sceneDirty = true;
}

void VirtualDisplayOutput::onFrameTick() {
    if (!m_running) return;
    renderNextFrame(false);
}

void VirtualDisplayOutput::renderNextFrame(bool forceRender) {
    if (!m_running || !m_renderControl || !m_rhi || !m_colorTexture) return;

    if (m_rootItem) {
        if (static_cast<int>(m_rootItem->width()) != m_width) m_rootItem->setWidth(m_width);
        if (static_cast<int>(m_rootItem->height()) != m_height) m_rootItem->setHeight(m_height);
    }

    if (forceRender || m_sceneDirty || m_renderedFrameCount == 0) {
        m_sceneDirty = false;

        m_renderControl->polishItems();
        m_renderControl->beginFrame();
        m_renderControl->sync();
        m_renderControl->render();

        const bool needCpuReadback = (!m_sink || m_sink->requiresCpuReadback());
        QRhiReadbackResult readback;
        if (needCpuReadback) {
            QRhiResourceUpdateBatch* batch = m_rhi->nextResourceUpdateBatch();
            batch->readBackTexture(QRhiReadbackDescription(m_colorTexture.get()), &readback);
            m_renderControl->commandBuffer()->resourceUpdate(batch);
        }

        m_renderControl->endFrame();

        if (needCpuReadback && !readback.data.isEmpty() &&
            readback.pixelSize.width() == m_width && readback.pixelSize.height() == m_height) {
            // Wrap the QByteArray directly (implicitly shared, zero-copy) instead of copying/flipping 8.3 MB on CPU
            QByteArray pixelBytes = readback.data;
            uchar* rawPtr = reinterpret_cast<uchar*>(const_cast<char*>(pixelBytes.constData()));
            auto* holder = new QByteArray(std::move(pixelBytes));
            m_lastFrame = QImage(
                rawPtr,
                m_width,
                m_height,
                m_width * 4,
                QImage::Format_RGBA8888_Premultiplied,
                [](void* info) { delete static_cast<QByteArray*>(info); },
                holder
            );
        }
        m_renderedFrameCount++;

        if (m_sink) {
            m_sink->submitFrame(m_rhi, m_colorTexture.get(), m_lastFrame);
        }
    }
}
