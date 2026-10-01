#include "MediaImageProvider.h"
#include <QImageReader>
#include <QFileInfo>
#include <QUrl>
#include <QRunnable>
#include <QThreadPool>

MediaImageProvider* MediaImageProvider::s_instance = nullptr;

MediaImageProvider::MediaImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {
    s_instance = this;
}

MediaImageProvider::~MediaImageProvider() {
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

MediaImageProvider* MediaImageProvider::instance() {
    return s_instance;
}

void MediaImageProvider::preloadImage(const QString &path) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_cache.contains(path)) return;
    }
    
    class PreloadTask : public QRunnable {
    public:
        QString m_path;
        PreloadTask(const QString& p) : m_path(p) {}
        void run() override {
            QImageReader reader(m_path);
            if (reader.canRead()) {
                QImage img = reader.read();
                if (!img.isNull()) {
                    if (MediaImageProvider::instance()) {
                        QMutexLocker lock(&MediaImageProvider::instance()->m_mutex);
                        MediaImageProvider::instance()->m_cache.insert(m_path, img);
                    }
                }
            }
        }
    };
    QThreadPool::globalInstance()->start(new PreloadTask(path));
}

QImage MediaImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    QString path = QUrl::fromPercentEncoding(id.toUtf8());
    
    QImage img;
    {
        QMutexLocker lock(&m_mutex);
        img = m_cache.value(path);
    }
    
    if (img.isNull()) {
        QImageReader reader(path);
        if (reader.canRead()) {
            img = reader.read();
            if (!img.isNull()) {
                QMutexLocker lock(&m_mutex);
                m_cache.insert(path, img);
            }
        }
    }
    
    if (size && !img.isNull()) {
        *size = img.size();
    }
    
    if (requestedSize.isValid() && !img.isNull()) {
        return img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    
    return img;
}

void MediaImageProvider::clearCache() {
    QMutexLocker lock(&m_mutex);
    m_cache.clear();
}
