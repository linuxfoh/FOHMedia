#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QUuid>
#include <QtQml/qqmlregistration.h>

struct ScreenConfig {
    static constexpr int UnassignedDisplay = -1;
    static constexpr int DisabledDisplay   = -2;
    static constexpr int VirtualObsDisplay = -3;

    QString id;
    QString name;
    int hardwareDisplayIndex = UnassignedDisplay;
    int width = 1920;
    int height = 1080;
    bool isFullscreen = false;
    bool isLocked = false;
    bool monitorEnabled = true;
    bool disabled = false;
    bool presentationDisabled = false;

    bool isVirtualObsDisplay() const { return hardwareDisplayIndex == VirtualObsDisplay; }

    QJsonObject toJson() const;
    static ScreenConfig fromJson(const QJsonObject& json);
};

class ScreenModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

public:
    enum ScreenRoles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        HardwareDisplayIndexRole,
        WidthRole,
        HeightRole,
        IsFullscreenRole,
        IsLockedRole,
        MonitorEnabledRole,
        DisabledRole,
        PresentationDisabledRole
    };

    explicit ScreenModel(QObject* parent = nullptr);
    ~ScreenModel() override = default;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    QHash<int, QByteArray> roleNames() const override;

    void setScreens(const QList<ScreenConfig>& screens);
    QList<ScreenConfig> screens() const;

    Q_INVOKABLE void addScreen(const QString& name);
    Q_INVOKABLE void removeScreen(int index);
    Q_INVOKABLE void updateScreen(int index, const QString& name, int displayIndex, int width, int height, bool isFullscreen, bool monitorEnabled, bool disabled = false, bool presentationDisabled = false);

    // Returns a list of strings for UI combobox: ["Unassigned", "Virtual Display (OBS)", "Display 1 (1920x1080)", ...]
    Q_INVOKABLE QStringList getHardwareDisplays() const;

    // Maps between ScreenConfig::hardwareDisplayIndex (-1, -3, 0..N-1) and the UI ComboBox index
    Q_INVOKABLE int comboIndexForHardwareDisplay(int hardwareDisplayIndex) const;
    Q_INVOKABLE int hardwareDisplayForComboIndex(int comboIndex) const;
    
    // Updates the width and height of the screen to match the hardware display's native resolution
    Q_INVOKABLE void applyHardwareDisplayGeometry(int screenIndex, int hardwareDisplayIndex);

    Q_INVOKABLE QStringList getScreenNames() const;

signals:
    void screensChanged(); // Emitted when a screen is added/removed/updated

private:
    QList<ScreenConfig> m_screens;
};
