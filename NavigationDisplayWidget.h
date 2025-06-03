#ifndef NAVIGATIONDISPLAYWIDGET_H
#define NAVIGATIONDISPLAYWIDGET_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QComboBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPixmap>
#include <QPainter>
#include <QDebug>
#include <QThread>
#include <QNetworkInterface>
#include <QShowEvent>
#include <QHideEvent>
#include "CameraResourceManager.h"  // 添加中央摄像头管理器
class NavigationDisplayWidget : public QWidget
{
    Q_OBJECT

public:
    explicit NavigationDisplayWidget(QWidget *parent = nullptr);
    ~NavigationDisplayWidget();

signals:
    void backButtonClicked();
    void navigationUpdated(const QString &direction, const QString &distance);

public slots:
    void startServer();
    void stopServer();
    void updateNavigation(const QString &direction, const QString &distance);
    void handleNetworkReply(QNetworkReply *reply);
    void pollNavData();
    void onBackButtonClicked();

protected:
    // 添加显示和隐藏事件处理
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private slots:
    

private:
    // UI Elements
    QLabel *m_titleLabel;
    QLabel *m_directionLabel;
    QLabel *m_distanceLabel;
    QLabel *m_directionImageLabel;
    QLabel *m_statusLabel;
    QPushButton *m_backButton;
    
    // Status variables
    bool m_serverRunning;
    QString m_currentDirection;
    QString m_currentDistance;
    
    // Network
    QNetworkAccessManager *m_networkManager;
    QTimer *m_pollTimer;
    
    // Server details
    int m_serverPort;
    
    // Helper functions
    void setupUI();
    void updateDirectionImage(const QString &direction);
    void updateStatusDisplay(const QString &status);
    QPixmap createDirectionArrow(const QString &direction);
    bool m_cameraInitialized = false;
};

#endif // NAVIGATIONDISPLAYWIDGET_H