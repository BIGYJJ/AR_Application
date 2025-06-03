#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStackedWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWheelEvent>
#include <QGesture>
#include <QProcess>
#include <QThread>
#include <QMessageBox>
#include <QTimer>
#include "Translate.h"
#include "PDFViewerPage.h"
#include "NavigationPage.h"
#include "CameraManager.h"
#include "GestureProcessor.h"
#include "VisionPage.h"
#include "CameraResourceManager.h"

// Forward declarations
class HttpServer;
class NavigationDisplayWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    
    // Set the HTTP server reference after construction
    void setHttpServer(HttpServer* server);
    
    // Method to update battery level
    void updateBatteryLevel(int level);

protected:
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;

private:
    // UI组件
    QStackedWidget *stackedWidget;
    QWidget *indicatorWidget;
    QList<QLabel*> indicators;
    QStringList iconPaths;
    QVector<QWidget*> subPages;
    
    // Battery indicator widget
    QLabel *batteryIconLabel;
    int batteryLevel;

    // 子页面 - 全部初始化为nullptr
    TranslatePage *translatePage = nullptr;
    PDFViewerPage *pdfViewerPage = nullptr;
    NavigationPage *navigationPage = nullptr;
    VisionPage *visionpage = nullptr;
    NavigationDisplayWidget *navigationDisplayWidget = nullptr;

    // 存储占位页面索引，用于后续替换
    QMap<int, int> placeholderIndexMap;

    // 摄像头和手势相关
    CameraManager *cameraManager;
    GestureProcessor *gestureProcessor;
    
    // HTTP服务器引用
    HttpServer* m_httpServer;

    // 初始化UI
    void setupUI();
    void loadIcons();
    void addNavigationIndicators();
    void updateIndicator();
    void createBatteryIndicator();
    void updateBatteryIcon();

    // 页面切换处理
    void PageChangeEvent(int index);

    // 摄像头管理
    bool checkCameraIsAvailable();
    void sendExitCommandToGestureRecognizer();

    // 懒加载页面辅助方法
    void initPageIfNeeded(int iconIndex, int targetIndex);

private slots:
    void onGestureDetected(const QString &gesture);
    void handleSwitchPage(int pageIndex);
    void handleBackToMain();
};

#endif // MAINWINDOW_H