#pragma once
#include <QQuickImageProvider>
#include <QImage>
#include <QHash>
#include <QMutex>
#include <QString>

class MediaImageProvider : public QQuickImageProvider {
public:
    MediaImageProvider();
    ~MediaImageProvider() override;
    
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    
    static MediaImageProvider* instance();
    
    void preloadImage(const QString &path);
    void clearCache();

private:
    QHash<QString, QImage> m_cache;
    QMutex m_mutex;
    static MediaImageProvider* s_instance;
};
