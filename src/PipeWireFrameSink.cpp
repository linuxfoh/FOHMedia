#include "../inc/VirtualDisplayOutput.h"

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)

#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <rhi/qrhi.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace {

struct BufferSlot {
    int fd = -1;
    void* mappedPtr = nullptr;
    size_t mappedSize = 0;
    uint32_t stride = 0;
    bool isDmaBuf = false;
    gbm_bo* bo = nullptr;
};

class PipeWireFrameSink final : public IVirtualFrameSink {
public:
    PipeWireFrameSink() = default;
    ~PipeWireFrameSink() override {
        shutdown();
    }

    bool initialize(const QString& screenName, int width, int height) override {
        shutdown();

        m_screenName = screenName;
        m_width = width > 0 ? width : 1920;
        m_height = height > 0 ? height : 1080;

        static bool s_pwInitialized = false;
        if (!s_pwInitialized) {
            pw_init(nullptr, nullptr);
            s_pwInitialized = true;
        }

        // Open DRM render node for zero-copy GBM DMA-BUF allocation if available
        m_drmFd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (m_drmFd >= 0) {
            m_gbmDevice = gbm_create_device(m_drmFd);
        }

        m_loop = pw_thread_loop_new("fohmedia-pw-out", nullptr);
        if (!m_loop) {
            std::cerr << "PipeWireFrameSink: Failed to create pw_thread_loop" << std::endl;
            return false;
        }

        if (pw_thread_loop_start(m_loop) < 0) {
            std::cerr << "PipeWireFrameSink: Failed to start pw_thread_loop" << std::endl;
            pw_thread_loop_destroy(m_loop);
            m_loop = nullptr;
            return false;
        }

        pw_thread_loop_lock(m_loop);

        QString safeId = screenName.toLower();
        safeId.replace(QRegularExpression("[^a-z0-9_]+"), "_");
        QByteArray nodeNameUtf8 = ("fohmedia_virtual_" + safeId).toUtf8();
        QByteArray nodeDescUtf8 = ("FOHMedia - " + screenName).toUtf8();

        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Source",
            PW_KEY_MEDIA_ROLE, "Camera",
            PW_KEY_MEDIA_CLASS, "Video/Source",
            PW_KEY_NODE_NAME, nodeNameUtf8.constData(),
            PW_KEY_NODE_DESCRIPTION, nodeDescUtf8.constData(),
            PW_KEY_NODE_NICK, nodeDescUtf8.constData(),
            PW_KEY_APP_NAME, "FOHMedia",
            PW_KEY_NODE_DRIVER, "true",
            "node.always-process", "true",
            nullptr
        );

        std::memset(&m_streamEvents, 0, sizeof(m_streamEvents));
        m_streamEvents.version = PW_VERSION_STREAM_EVENTS;
        m_streamEvents.state_changed = &PipeWireFrameSink::onStateChanged;
        m_streamEvents.param_changed = &PipeWireFrameSink::onParamChanged;
        m_streamEvents.add_buffer = &PipeWireFrameSink::onAddBuffer;
        m_streamEvents.remove_buffer = &PipeWireFrameSink::onRemoveBuffer;
        m_streamEvents.process = &PipeWireFrameSink::onProcess;

        m_stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(m_loop),
            nodeDescUtf8.constData(),
            props,
            &m_streamEvents,
            this
        );

        if (!m_stream) {
            pw_thread_loop_unlock(m_loop);
            std::cerr << "PipeWireFrameSink: Failed to create pw_stream" << std::endl;
            shutdown();
            return false;
        }

        uint8_t paramBuffer[2048];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(paramBuffer, sizeof(paramBuffer));

        spa_rectangle rect = SPA_RECTANGLE(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
        spa_fraction fps = SPA_FRACTION(60, 1);

        const spa_pod* params[2];
        // Offer two separate SPA_PARAM_EnumFormat pods with single SPA_POD_Id formats (BGRA and RGBA)
        // because OBS Studio's pipewire-camera-source requires a fixed SPA_POD_Id (not SPA_POD_CHOICE_ENUM_Id)
        // per EnumFormat entry when populating its Video Format, Resolution, and Frame Rate dropdowns.
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,   SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
            SPA_FORMAT_VIDEO_size,     SPA_POD_Rectangle(&rect),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps)
        ));
        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,   SPA_POD_Id(SPA_VIDEO_FORMAT_RGBA),
            SPA_FORMAT_VIDEO_size,     SPA_POD_Rectangle(&rect),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps)
        ));

        int res = pw_stream_connect(
            m_stream,
            PW_DIRECTION_OUTPUT,
            PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS),
            params,
            2
        );

        spa_fraction rate = SPA_FRACTION(1, 60);
        pw_stream_set_rate(m_stream, 1.0);
        Q_UNUSED(rate);

        // Drive the PipeWire graph directly on our own PipeWire thread loop at 60 Hz (16.666 ms)
        // so that graph cycles never stall or depend on the Qt GUI event loop timer.
        m_pwTimer = pw_loop_add_timer(pw_thread_loop_get_loop(m_loop), &PipeWireFrameSink::onPwTimerTick, this);
        if (m_pwTimer) {
            timespec value{};
            value.tv_sec = 0;
            value.tv_nsec = 16666667; // 60 Hz
            timespec interval{};
            interval.tv_sec = 0;
            interval.tv_nsec = 16666667; // 60 Hz
            pw_loop_update_timer(pw_thread_loop_get_loop(m_loop), m_pwTimer, &value, &interval, false);
        }

        pw_thread_loop_unlock(m_loop);

        if (res < 0) {
            std::cerr << "PipeWireFrameSink: pw_stream_connect failed: " << spa_strerror(res) << std::endl;
            shutdown();
            return false;
        }

        m_active = true;
        return true;
    }

    void submitFrame(QRhi* /*rhi*/, QRhiTexture* /*texture*/, const QImage& rgbaImage) override {
        if (!m_active || !m_stream || rgbaImage.isNull()) return;

        QImage converted;
        if (m_negotiatedFormat == SPA_VIDEO_FORMAT_RGBA) {
            converted = (rgbaImage.format() == QImage::Format_RGBA8888_Premultiplied)
                ? rgbaImage
                : rgbaImage.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        } else {
            converted = (rgbaImage.format() == QImage::Format_ARGB32_Premultiplied)
                ? rgbaImage
                : rgbaImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }

        {
            QMutexLocker locker(&m_frameMutex);
            m_latestFrame = converted;
            m_hasFrame = true;
        }
    }

    void shutdown() override {
        m_active = false;
        m_streaming = false;

        if (m_loop) {
            pw_thread_loop_lock(m_loop);
            if (m_pwTimer) {
                pw_loop_destroy_source(pw_thread_loop_get_loop(m_loop), m_pwTimer);
                m_pwTimer = nullptr;
            }
            if (m_stream) {
                pw_stream_disconnect(m_stream);
                pw_stream_destroy(m_stream);
                m_stream = nullptr;
            }
            pw_thread_loop_unlock(m_loop);
            pw_thread_loop_stop(m_loop);
            pw_thread_loop_destroy(m_loop);
            m_loop = nullptr;
        }

        if (m_gbmDevice) {
            gbm_device_destroy(m_gbmDevice);
            m_gbmDevice = nullptr;
        }
        if (m_drmFd >= 0) {
            close(m_drmFd);
            m_drmFd = -1;
        }
    }

    bool isActive() const override {
        return m_active;
    }

    QString backendDescription() const override {
        return QString("PipeWire (FOHMedia - %1)").arg(m_screenName);
    }

private:
    static void onPwTimerTick(void* data, uint64_t /*expirations*/) {
        auto* self = static_cast<PipeWireFrameSink*>(data);
        if (self->m_stream && self->m_streaming) {
            pw_stream_trigger_process(self->m_stream);
        }
    }

    static void onStateChanged(void* data, pw_stream_state /*old*/, pw_stream_state state, const char* /*error*/) {
        auto* self = static_cast<PipeWireFrameSink*>(data);
        self->m_streaming = (state == PW_STREAM_STATE_STREAMING);
    }

    static void onParamChanged(void* data, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<PipeWireFrameSink*>(data);
        if (!param || id != SPA_PARAM_Format) return;

        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;

        self->m_negotiatedFormat = info.format;
        self->m_stride = static_cast<uint32_t>(self->m_width * 4);
        const uint32_t frameBytes = self->m_stride * static_cast<uint32_t>(self->m_height);

        uint8_t paramsBuf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(paramsBuf, sizeof(paramsBuf));

        // Use only SPA_DATA_MemFd for CPU-written frame buffers so that CPU writes are 100% cache-coherent
        // with OBS's GL upload (mmap() directly on a tiled/unflushed GPU GBM DMA-BUF fd causes coherency stalls).
        const uint32_t dataTypes = (1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr);

        const spa_pod* bufferParams[2];
        bufferParams[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 2, 4),
            SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size,    SPA_POD_Int(static_cast<int32_t>(frameBytes)),
            SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(static_cast<int32_t>(self->m_stride)),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(dataTypes)
        ));
        bufferParams[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))
        ));

        pw_stream_update_params(self->m_stream, bufferParams, 2);
    }

    static void onAddBuffer(void* data, pw_buffer* pwBuf) {
        auto* self = static_cast<PipeWireFrameSink*>(data);
        spa_buffer* buf = pwBuf->buffer;
        spa_data* d = &buf->datas[0];

        auto* slot = new BufferSlot();
        pwBuf->user_data = slot;

        const uint32_t defaultStride = static_cast<uint32_t>(self->m_width * 4);
        const size_t defaultSize = static_cast<size_t>(defaultStride) * static_cast<size_t>(self->m_height);

        // 1. Try zero-copy DMA-BUF allocation via GBM if negotiated
        if ((d->type & (1u << SPA_DATA_DmaBuf)) && self->m_gbmDevice) {
            uint32_t gbmFormat = (self->m_negotiatedFormat == SPA_VIDEO_FORMAT_RGBA)
                ? GBM_FORMAT_ABGR8888
                : GBM_FORMAT_ARGB8888;
            gbm_bo* bo = gbm_bo_create(
                self->m_gbmDevice,
                static_cast<uint32_t>(self->m_width),
                static_cast<uint32_t>(self->m_height),
                gbmFormat,
                GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR
            );
            if (bo) {
                int dmaFd = gbm_bo_get_fd(bo);
                uint32_t boStride = gbm_bo_get_stride(bo);
                size_t boSize = static_cast<size_t>(boStride) * static_cast<size_t>(self->m_height);
                void* mapped = mmap(nullptr, boSize, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd, 0);
                if (dmaFd >= 0 && mapped != MAP_FAILED) {
                    slot->bo = bo;
                    slot->fd = dmaFd;
                    slot->mappedPtr = mapped;
                    slot->mappedSize = boSize;
                    slot->stride = boStride;
                    slot->isDmaBuf = true;

                    d->type = SPA_DATA_DmaBuf;
                    d->flags = SPA_DATA_FLAG_READWRITE;
                    d->fd = dmaFd;
                    d->mapoffset = 0;
                    d->maxsize = static_cast<uint32_t>(boSize);
                    d->data = nullptr;
                    return;
                }
                if (dmaFd >= 0) close(dmaFd);
                gbm_bo_destroy(bo);
            }
        }

        // 2. Fallback to MemFd shared memory
        int memFd = memfd_create("fohmedia-pw-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (memFd >= 0 && ftruncate(memFd, static_cast<off_t>(defaultSize)) == 0) {
            void* mapped = mmap(nullptr, defaultSize, PROT_READ | PROT_WRITE, MAP_SHARED, memFd, 0);
            if (mapped != MAP_FAILED) {
                slot->fd = memFd;
                slot->mappedPtr = mapped;
                slot->mappedSize = defaultSize;
                slot->stride = defaultStride;
                slot->isDmaBuf = false;

                d->type = SPA_DATA_MemFd;
                d->flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
                d->fd = memFd;
                d->mapoffset = 0;
                d->maxsize = static_cast<uint32_t>(defaultSize);
                d->data = mapped;
                return;
            }
            close(memFd);
        }
    }

    static void onRemoveBuffer(void* /*data*/, pw_buffer* pwBuf) {
        auto* slot = static_cast<BufferSlot*>(pwBuf->user_data);
        if (!slot) return;

        if (slot->mappedPtr && slot->mappedPtr != MAP_FAILED) {
            munmap(slot->mappedPtr, slot->mappedSize);
        }
        if (slot->fd >= 0) {
            close(slot->fd);
        }
        if (slot->bo) {
            gbm_bo_destroy(slot->bo);
        }
        delete slot;
        pwBuf->user_data = nullptr;
    }

    static void onProcess(void* data) {
        auto* self = static_cast<PipeWireFrameSink*>(data);
        self->writeNextBufferLocked();
    }

    void writeNextBufferLocked() {
        if (!m_stream || !m_streaming) return;

        pw_buffer* b = pw_stream_dequeue_buffer(m_stream);
        if (!b) return;

        auto* slot = static_cast<BufferSlot*>(b->user_data);
        spa_buffer* buf = b->buffer;
        spa_data* d = &buf->datas[0];

        if (slot && slot->mappedPtr) {
            QImage frameCopy;
            {
                QMutexLocker locker(&m_frameMutex);
                if (m_hasFrame) {
                    frameCopy = m_latestFrame;
                }
            }

            const uint32_t dstStride = slot->stride > 0 ? slot->stride : static_cast<uint32_t>(m_width * 4);
            if (!frameCopy.isNull()) {
                const int copyRows = std::min(m_height, frameCopy.height());
                const size_t rowBytes = static_cast<size_t>(std::min(m_width, frameCopy.width())) * 4;
                auto* dstBase = static_cast<uint8_t*>(slot->mappedPtr);
                if (dstStride == rowBytes) {
                    std::memcpy(dstBase, frameCopy.constBits(), rowBytes * static_cast<size_t>(copyRows));
                } else {
                    for (int y = 0; y < copyRows; ++y) {
                        std::memcpy(dstBase + static_cast<size_t>(y) * dstStride,
                                    frameCopy.constScanLine(y),
                                    rowBytes);
                    }
                }
            } else {
                std::memset(slot->mappedPtr, 0, slot->mappedSize);
            }

            d->chunk->offset = 0;
            d->chunk->size = static_cast<uint32_t>(slot->mappedSize);
            d->chunk->stride = static_cast<int32_t>(dstStride);
        }

        if (auto* h = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header)))) {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            h->pts = static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
            h->flags = 0;
            h->seq = m_seq++;
            h->dts_offset = 0;
        }

        pw_stream_queue_buffer(m_stream, b);
    }

    QString m_screenName;
    int m_width = 1920;
    int m_height = 1080;
    uint32_t m_stride = 1920 * 4;
    spa_video_format m_negotiatedFormat = SPA_VIDEO_FORMAT_BGRA;
    uint64_t m_seq = 0;

    int m_drmFd = -1;
    gbm_device* m_gbmDevice = nullptr;

    pw_thread_loop* m_loop = nullptr;
    pw_stream* m_stream = nullptr;
    spa_source* m_pwTimer = nullptr;
    pw_stream_events m_streamEvents{};

    bool m_active = false;
    bool m_streaming = false;

    QMutex m_frameMutex;
    QImage m_latestFrame;
    bool m_hasFrame = false;
};

} // namespace

std::unique_ptr<IVirtualFrameSink> VirtualDisplayOutput::createPlatformSink() {
    return std::make_unique<PipeWireFrameSink>();
}

#endif // Q_OS_LINUX
