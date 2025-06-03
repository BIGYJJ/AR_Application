#include "MainWindow.h"
#include <QPushButton>
#include <QGesture>
#include <QSwipeGesture>
#include <QDebug>
#include <QTime>
#include <QPainter>
#include "Httpserver.h"
#include "NavigationDisplayWidget.h"
#include "VisionPage.h"
// Existing functions
bool releaseSystemCamera()
{
    // 使用QProcess调用外部脚本
    QProcess process;
    process.start("/mnt/tsp/camera_toggle.sh", QStringList() << "release" << "0");
    
    // 等待脚本执行完成，最多等待5秒
    if (!process.waitForFinished(5000)) {
        qWarning() << "释放摄像头超时";
        return false;
    }
    
    // 检查退出状态
    if (process.exitCode() != 0) {
        qWarning() << "释放摄像头失败:" << process.readAllStandardError();
        return false;
    }
    
    // 为确保资源完全释放，添加短暂延迟
    QThread::msleep(1000);
    
    qDebug() << "摄像头资源已释放:" << process.readAllStandardOutput().trimmed();
    return true;
}

// 检查摄像头是否被占用
bool isCameraInUse()
{
    QProcess process;
    process.start("/mnt/tsp/camera_toggle.sh", QStringList() << "check" << "0");
    
    if (!process.waitForFinished(3000)) {
        qWarning() << "检查摄像头状态超时";
        return true; // 假设被占用
    }
    
    // 如果退出码为0，摄像头空闲；否则被占用
    return process.exitCode() != 0;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      stackedWidget(new QStackedWidget(this)),
      indicatorWidget(new QWidget(this)),
        batteryLevel(10),  // Initialize battery level to 1%
      m_httpServer(nullptr)  // 初始化为nullptr
{
    // Set window attributes for transparency
   
    
    // Set central widget
    setCentralWidget(stackedWidget);
    setupUI();
    subPages.resize(iconPaths.size()); // 初始化子页面容器
    createBatteryIndicator();
    // 创建并初始化摄像头管理器
    cameraManager = new CameraManager(this);
    
    QTimer::singleShot(1000, this, [this]() {
        // 创建并初始化手势处理器
        gestureProcessor = new GestureProcessor(this);
        connect(gestureProcessor, &GestureProcessor::gestureDetected, 
                this, &MainWindow::onGestureDetected);
        
        // 如果当前是主页面，启动手势识别
        if (stackedWidget->currentIndex() < iconPaths.size()) {
            gestureProcessor->startCamera();
        }
    });
}

// Add the setHttpServer method implementation
void MainWindow::setHttpServer(HttpServer* server)
{
    m_httpServer = server;
    
    if (m_httpServer) {
        QString ipAddress = m_httpServer->getLocalIpAddress();
        qDebug() << "HTTP服务器地址：" << ipAddress << ":8080";
        
        // 使用延迟连接，确保所有组件都已初始化
        QTimer::singleShot(1000, this, [this]() {
            // 只连接通用信号
            if (m_httpServer) {
                RequestHandler& handler = m_httpServer->getRequestHandler();
                connect(&handler, &RequestHandler::switchPageRequested, 
                        this, &MainWindow::handleSwitchPage);
                connect(&handler, &RequestHandler::backToMainRequested, 
                        this, &MainWindow::handleBackToMain);
                
                // 注意: 特定页面的信号连接会在页面初始化时完成
                qDebug() << "HTTP服务器基本信号已连接";
            }
        });
    }
}

MainWindow::~MainWindow()
{
    // 确保释放资源
    if (gestureProcessor) {
        gestureProcessor->stopCamera();
    }
}

void MainWindow::wheelEvent(QWheelEvent *event)
{
    // 保持原有的滚轮事件处理
    QPoint angleDelta = event->angleDelta();

    if (!angleDelta.isNull())
    {
        int currentIndex = stackedWidget->currentIndex();
        int N = iconPaths.size();

        // 仅在主页面中处理滚动事件
        if (currentIndex >= 0 && currentIndex < N) {
            if (angleDelta.y() > 0) {
                // 向上滚动，显示上一个图标
                int prevIndex = currentIndex - 1;
                if (prevIndex < 0) {
                    prevIndex = N - 1; // 循环到最后一个主页面
                }
                stackedWidget->setCurrentIndex(prevIndex);
            } else if (angleDelta.y() < 0) {
                // 向下滚动，显示下一个图标
                int nextIndex = currentIndex + 1;
                if (nextIndex >= N) {
                    nextIndex = 0; // 循环到第一个主页面
                }
                stackedWidget->setCurrentIndex(nextIndex);
            }
            updateIndicator(); // 更新指示器
        }
    }

    event->accept(); // 接受事件
}
void MainWindow::setupUI()
{
    // 加载图标路径
    loadIcons();

    for (int i = 0; i < iconPaths.size(); ++i)
    {
        // Create container widget as page
        QWidget *page = new QWidget();
        
        // Create a layout to center the button
        QVBoxLayout *centerLayout = new QVBoxLayout(page);
        centerLayout->setAlignment(Qt::AlignCenter);
        
        // Create the button
        QPushButton *button = new QPushButton();
        button->setFixedSize(200, 200);
        QPixmap pixmap(iconPaths[i]);
        button->setIcon(QIcon(pixmap));
        button->setIconSize(QSize(180, 180));
        button->setFlat(true);
        
        // Add button to the center layout
        centerLayout->addWidget(button);
        
        // 修改: 连接按钮信号到新函数
        connect(button, &QPushButton::clicked, this, [this, i]() {
            int targetIndex = iconPaths.size() + i;
            // 调用懒加载初始化方法
            initPageIfNeeded(i, targetIndex);
            stackedWidget->setCurrentIndex(targetIndex);
            updateIndicator();
            PageChangeEvent(targetIndex);
        });

        // Add page to stacked widget
        stackedWidget->addWidget(page);
    }

    // 添加占位页面到 QStackedWidget
    for (int i = 0; i < iconPaths.size(); ++i) {
        // 创建简单的占位页面
        QWidget *placeholderPage = new QWidget();
        QVBoxLayout *layout = new QVBoxLayout(placeholderPage);
        QLabel *loadingLabel = new QLabel("正在加载页面...", placeholderPage);
        loadingLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(loadingLabel);
        
        // 添加占位页面
        int pageIndex = stackedWidget->addWidget(placeholderPage);
        
        // 记录占位页面索引，以便后续替换
        placeholderIndexMap[i] = pageIndex;
    }

    // 设置初始页面
    stackedWidget->setCurrentIndex(0);

    // 添加导航指示器
    addNavigationIndicators();
}


void MainWindow::initPageIfNeeded(int iconIndex, int targetIndex)
{
    // 检查页面是否已初始化
    QWidget* currentWidget = stackedWidget->widget(targetIndex);
    bool needInit = false;
    
    // 检查当前widget是否是占位页面
    if (placeholderIndexMap.contains(iconIndex) && 
        stackedWidget->indexOf(currentWidget) == placeholderIndexMap[iconIndex]) {
        needInit = true;
    }
    
    if (!needInit) {
        // 页面已初始化，无需重复创建
        return;
    }
    
    qDebug() << "开始初始化页面:" << iconIndex;
    
    QWidget* newPage = nullptr;
    
    // 根据不同的图标索引创建相应的页面
    switch (iconIndex) {
        case 0: // 翻译页面
        {
            translatePage = new TranslatePage(nullptr, this);
            newPage = translatePage;
            
            // 连接返回信号
            connect(translatePage, &TranslatePage::backButtonClicked, this, [this]() {
                stackedWidget->setCurrentIndex(0);
                updateIndicator();
                PageChangeEvent(0);
            });
            break;
        }
        case 1: // PDF查看器页面
        {
            pdfViewerPage = new PDFViewerPage(this);
            newPage = pdfViewerPage;
            
            // 连接返回信号
            connect(pdfViewerPage, &PDFViewerPage::backButtonClicked, this, [this]() {
                stackedWidget->setCurrentIndex(0);
                updateIndicator();
                PageChangeEvent(0); // 添加这一行，确保重启手势识别
            });
            
            // 连接HTTP服务器信号(如果已设置)
            if (m_httpServer) {
                RequestHandler& handler = m_httpServer->getRequestHandler();
                connect(&handler, &RequestHandler::pdfDataReceived, 
                        pdfViewerPage, &PDFViewerPage::networkLoadPDF);
                connect(&handler, &RequestHandler::pdfNextPage, 
                        pdfViewerPage, &PDFViewerPage::nextPage);
                connect(&handler, &RequestHandler::pdfPrevPage, 
                        pdfViewerPage, &PDFViewerPage::prevPage);
            }
            break;
        }
        case 2: // 导航显示页面
        {
            navigationDisplayWidget = new NavigationDisplayWidget(this);
            newPage = navigationDisplayWidget;
            
            // 连接返回信号
            connect(navigationDisplayWidget, &NavigationDisplayWidget::backButtonClicked, this, [this]() {
                stackedWidget->setCurrentIndex(0);
                updateIndicator();
                PageChangeEvent(0); // 添加这一行，确保重启手势识别
            });
            
            // 连接HTTP服务器信号(如果已设置)
            if (m_httpServer) {
                m_httpServer->registerNavigationWidget(navigationDisplayWidget);
                m_httpServer->connectNavigationSignals(navigationDisplayWidget);
            }
            break;
        }
        case 3: // 视觉页面
        {
            visionpage = new VisionPage(nullptr, this);
            newPage = visionpage;
            
            // 连接返回信号
            connect(visionpage, &VisionPage::backButtonClicked, this, [this]() {
                // Use QueuedConnection to avoid direct call during possible deletion
                QMetaObject::invokeMethod(this, [this]() {
                    // Stop recording first to ensure clean resource release
                    if (visionpage) {
                        visionpage->stopRecording();
                        qDebug() << "VisionPage recording stopped via back button";
                    }
                    
                    // Delay page change slightly to ensure resources are properly released
                    QTimer::singleShot(500, this, [this]() {
                        stackedWidget->setCurrentIndex(0);
                        updateIndicator();
                        PageChangeEvent(0); // 添加这一行，确保重启手势识别
                    });
                }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
            break;
        }
        default:
            qWarning() << "未知页面索引:" << iconIndex;
            return;
    }
    
    if (newPage) {
        // 获取当前占位页面
        QWidget* oldWidget = stackedWidget->widget(targetIndex);
        
        // 替换占位页面
        stackedWidget->removeWidget(oldWidget);
        stackedWidget->insertWidget(targetIndex, newPage);
        
        // 删除旧的占位页面
        delete oldWidget;
        
        qDebug() << "页面初始化完成:" << iconIndex;
    }
}

void MainWindow::PageChangeEvent(int index)
{
    qDebug() << "页面切换到索引:" << index;
    
    // 记录切换前的摄像头状态
    auto& cameraManager = CameraResourceManager::instance();
    CameraState initialState = cameraManager.getCameraState(0);
    QString currentUser = cameraManager.getCurrentUser();
    
    qDebug() << "切换前摄像头状态:" << (int)initialState << "用户:" << currentUser;
    
    // 首先停止手势处理器，不管当前是什么页面
    if (gestureProcessor) {
        gestureProcessor->stopCamera();
        QThread::msleep(500); // 等待停止完成
    }
    
    // 释放所有现有摄像头请求
    cameraManager.resetAllCameras();
    QThread::msleep(1000); // 等待资源完全释放
    
    // 根据不同页面，请求不同优先级的摄像头资源
    if (index >= iconPaths.size()) {
        // 子页面请求 - 检查页面是否已初始化
        int pageType = index - iconPaths.size();
        
        if (pageType == 1 && pdfViewerPage != nullptr) { // PDF查看器
            CameraRequest pdfRequest;
            pdfRequest.requesterId = "PDFViewer";
            pdfRequest.priority = RequestPriority::High;
            pdfRequest.preferredCameraIndex = 0; // 明确请求video0
            if (!cameraManager.requestCamera(pdfRequest)) {
                qWarning() << "无法为PDF查看器获取摄像头资源";
            } else {
                qDebug() << "成功为PDF查看器分配摄像头资源";
            }
        }
        else if (pageType == 3 && navigationDisplayWidget != nullptr) { // 导航显示页面
           // Replace the aggressive resource clearing with a safer approach
     // Delay the camera initialization by 1 second
   // m_statusLabel->setText("状态: 初始化视觉识别页面...");
    
    // First show the page without camera initialization
    QTimer::singleShot(6000, this, [this]() {
        // Now initialize the camera after delay
        auto& cameraManager = CameraResourceManager::instance();
    
        // First ensure all cameras are properly released through the manager
        cameraManager.resetAllCameras();
    
        // Wait a reasonable time for resources to be released
        QThread::msleep(500);
    
        // Now make a proper request
        CameraRequest visionRequest;
        visionRequest.requesterId = "VisionPage";
        visionRequest.priority = RequestPriority::High;  // High instead of Critical
        visionRequest.preferredCameraIndex = 0;
        
        // Try to request the camera with proper error handling
        bool resourceObtained = cameraManager.requestCamera(visionRequest);
        
        if (resourceObtained) {
            if (visionpage) {
                visionpage->startRecording();
                //m_statusLabel->setText("状态: 视觉识别已启动");
            }
        } else {
            // Handle the failure case - display a message to the user
            QMessageBox::warning(this, "Camera Error", 
                "Could not access camera for Vision Page. Please close other applications using the camera.");
        }
    });
        }
        else if (pageType == 4 && visionpage != nullptr) { // 视觉识别页面
            CameraRequest visionRequest;
            visionRequest.requesterId = "VisionPage";
            visionRequest.priority = RequestPriority::High;
            visionRequest.preferredCameraIndex = 0; // 明确请求video0
            if (!cameraManager.requestCamera(visionRequest)) {
                qWarning() << "无法为视觉识别页面获取摄像头资源";
            } else {
                qDebug() << "成功为视觉识别页面分配摄像头资源";
            }
        }
    }
    else {
        // 主页面请求 - 启动手势识别
        CameraRequest gestureRequest;
        gestureRequest.requesterId = "GestureRecognizer";
        gestureRequest.priority = RequestPriority::Normal; // 使用Normal而非Critical
        gestureRequest.preferredCameraIndex = 0; // 明确请求video0
        
        if (cameraManager.requestCamera(gestureRequest)) {
            qDebug() << "摄像头资源已分配给手势识别器";
            
            // 延迟一小段时间后启动手势处理器
            QTimer::singleShot(1200, this, [this]() {
                if (gestureProcessor) {
                    gestureProcessor->startCamera();
                    qDebug() << "手势识别已重新启动";
                }
            });
        } else {
            qWarning() << "无法为手势识别器获取摄像头资源";
            
            // 尝试重新释放摄像头资源后再次请求
            cameraManager.resetAllCameras();
            QThread::msleep(1500);
            
            if (cameraManager.requestCamera(gestureRequest)) {
                qDebug() << "第二次尝试：摄像头资源已分配给手势识别器";
                QTimer::singleShot(1200, this, [this]() {
                    if (gestureProcessor) {
                        gestureProcessor->startCamera();
                        qDebug() << "手势识别已重新启动（第二次尝试）";
                    }
                });
            } else {
                qCritical() << "无法启动手势识别，摄像头资源不可用";
            }
        }
    }
}


// 检查摄像头是否可用
bool MainWindow::checkCameraIsAvailable()
{
    if (cameraManager) {
        return cameraManager->isCameraAvailable();
    }
    
    // 如果cameraManager不存在，使用备用方法
    qDebug() << "检查摄像头是否可用...";
    
    // 尝试直接检查摄像头
    bool available = false;
    try {
        const auto cameras = QMediaDevices::videoInputs();
        if (!cameras.isEmpty()) {
            QCamera *testCamera = new QCamera(cameras.first());
            testCamera->start();
            
            // 短暂等待
            QThread::msleep(300);
            
            available = testCamera->isActive();
            
            // 清理测试
            testCamera->stop();
            delete testCamera;
        }
    } catch (...) {
        available = false;
    }
    
    qDebug() << "直接检查摄像头状态: " << (available ? "可用" : "不可用");
    return available;
}

// 添加新函数，发送EXIT命令给手势识别程序
void MainWindow::sendExitCommandToGestureRecognizer()
{
    QUdpSocket socket;
    QByteArray datagram = "EXIT";
    socket.writeDatagram(datagram, QHostAddress::LocalHost, 12346);
    
    // 等待短暂时间确保命令被接收
    QThread::msleep(300);
    qDebug() << "已发送EXIT命令到端口12346";
}

void MainWindow::loadIcons()
{
    // 根据资源文件路径加载图标
    iconPaths.append("/mnt/tsp/AR_Application/icons/icon1.png");
    iconPaths.append("/mnt/tsp/AR_Application/icons/icon2.png");
    iconPaths.append("/mnt/tsp/AR_Application/icons/icon4.png");
    iconPaths.append("/mnt/tsp/AR_Application/icons/icon5.png");
    // 添加更多图标路径
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::Gesture) {
        QGestureEvent *gestureEvent = static_cast<QGestureEvent*>(event);
        if (QGesture *swipe = gestureEvent->gesture(Qt::SwipeGesture)) {
            QSwipeGesture *swipeGesture = static_cast<QSwipeGesture *>(swipe);
            if (swipeGesture->state() == Qt::GestureFinished) {
                if (swipeGesture->horizontalDirection() == QSwipeGesture::Left) {
                    // 向左滑动，显示下一个图标
                    int nextIndex = stackedWidget->currentIndex() + 1;
                    if (nextIndex >= stackedWidget->count()) {
                        nextIndex = 0; // 循环到第一个
                    }
                    stackedWidget->setCurrentIndex(nextIndex);
                    updateIndicator();
                }
                else if (swipeGesture->horizontalDirection() == QSwipeGesture::Right) {
                    // 向右滑动，显示上一个图标
                    int prevIndex = stackedWidget->currentIndex() - 1;
                    if (prevIndex < 0) {
                        prevIndex = stackedWidget->count() - 1; // 循环到最后一个
                    }
                    stackedWidget->setCurrentIndex(prevIndex);
                    updateIndicator();
                }
            }
        }
        return true;
    }
    return QMainWindow::event(event);
}

void MainWindow::addNavigationIndicators()
{
    // 创建一个水平布局来放置指示器圆点
    QHBoxLayout *layout = new QHBoxLayout(indicatorWidget);
    layout->setSpacing(5);
    layout->setAlignment(Qt::AlignCenter);

    for (int i = 0; i < iconPaths.size(); ++i) {
        QLabel *dot = new QLabel(indicatorWidget);
        dot->setFixedSize(10, 10);
        QPixmap pixmap(10, 10);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);  // 使用 QPainter
        painter.setRenderHint(QPainter::Antialiasing);
        if (i == stackedWidget->currentIndex()) {
            painter.setBrush(Qt::blue);
        } else {
            painter.setBrush(Qt::lightGray);
        }
        painter.drawEllipse(0, 0, 10, 10);
        painter.end();
        dot->setPixmap(pixmap);
        indicators.append(dot);
        layout->addWidget(dot);
    }

    // 创建一个布局容器，将 QStackedWidget 和指示器垂直排列
    QWidget *container = new QWidget(this);
    QVBoxLayout *containerLayout = new QVBoxLayout(container);
    containerLayout->addWidget(stackedWidget);
    containerLayout->addWidget(indicatorWidget);
    containerLayout->setStretch(0, 1);
    containerLayout->setStretch(1, 0);
    stackedWidget->setMaximumSize(1000, 600); // 与 main.cpp 中的 resize 一致
    setCentralWidget(container);
}

void MainWindow::updateIndicator()
{
    for (int i = 0; i < indicators.size(); ++i)
    {
        QLabel *dot = indicators[i];
        QPixmap pixmap(10, 10);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);  // 使用 QPainter
        painter.setRenderHint(QPainter::Antialiasing);
        if (i == stackedWidget->currentIndex()) {
            painter.setBrush(Qt::blue);
        } else {
            painter.setBrush(Qt::lightGray);
        }
        painter.drawEllipse(0, 0, 10, 10);
        painter.end();
        dot->setPixmap(pixmap);
    }
}

void MainWindow::onGestureDetected(const QString &gesture)
{
    qDebug() << "Detected Gesture:" << gesture;

    // 只在主页面响应手势
    int currentIndex = stackedWidget->currentIndex();
    if (currentIndex >= iconPaths.size()) {
        return;
    }

    // 处理左划手势
    if (gesture == "swipe_left") {
        int nextIndex = currentIndex + 1;
        if (nextIndex >= iconPaths.size()) {
            nextIndex = 0; // 循环到第一个
        }
        stackedWidget->setCurrentIndex(nextIndex);
        updateIndicator();
    }
    // 处理点击手势
    else if (gesture == "click") {
        qDebug() << "点击手势触发当前图标按钮";
        
        // 点击当前显示的主页面中的按钮
        if (currentIndex >= 0 && currentIndex < iconPaths.size()) {
            // 获取当前显示的页面
            QWidget *currentPage = stackedWidget->widget(currentIndex);
            if (currentPage) {
                // 查找页面中的按钮并模拟点击
                QPushButton *button = currentPage->findChild<QPushButton*>();
                if (button) {
                    // 点击按钮前先触发懒加载初始化
                    int targetIndex = iconPaths.size() + currentIndex;
                    initPageIfNeeded(currentIndex, targetIndex);
                    
                    button->click();
                    qDebug() << "成功触发图标" << currentIndex << "的点击操作";
                } else {
                    qWarning() << "在页面" << currentIndex << "中未找到按钮";
                }
            }
        }
    }
}


void MainWindow::handleSwitchPage(int pageIndex)
{
    qDebug() << "收到页面切换请求，目标页面索引:" << pageIndex;
    
    // 验证页面索引是否有效
    int totalMainPages = iconPaths.size();
    int totalSubPages = subPages.size();
    int totalPages = totalMainPages + totalSubPages;
    
    if (pageIndex >= 0 && pageIndex < totalPages) {
        // 如果是子页面，确保先初始化
        if (pageIndex >= iconPaths.size()) {
            int iconIndex = pageIndex - iconPaths.size();
            initPageIfNeeded(iconIndex, pageIndex);
        }
        
        // 切换到指定页面
        stackedWidget->setCurrentIndex(pageIndex);
        updateIndicator();
        
        // 如果切换到子页面，触发相应的页面变更事件
        if (pageIndex >= iconPaths.size()) {
            PageChangeEvent(pageIndex);
        }
        
        qDebug() << "页面已切换到:" << pageIndex;
    } else {
        qWarning() << "无效的页面索引:" << pageIndex;
    }
}
void MainWindow::handleBackToMain()
{
    qDebug() << "收到返回主页请求";
    
    // 获取当前页面索引
    int currentIndex = stackedWidget->currentIndex();
    
    // 如果当前不在主页面，返回到第一个主页面
    if (currentIndex >= iconPaths.size()) {
        // 计算子页面类型
        int pageType = currentIndex - iconPaths.size();
        
        // 根据页面类型调用相应的资源释放方法
        switch (pageType) {
            case 0: // TranslatePage
                if (translatePage) {
                    // 直接调用内部处理方法，该方法会执行清理并发送信号
                    translatePage->backButtonClickedHandler();
                    // 不需要手动切换页面，因为backButtonClickedHandler会发送信号触发页面切换
                    return;
                }
                break;
                
            case 1: // PDFViewerPage
                if (pdfViewerPage) {
                    // 停止摄像头，然后手动切换页面
                    pdfViewerPage->stopCamera();
                }
                break;
                
            case 2: // NavigationDisplayWidget
                // NavigationDisplayWidget 没有特殊的清理逻辑，只是发送了信号
                if (navigationDisplayWidget) {
                    navigationDisplayWidget->onBackButtonClicked();
                }
                break;
                
            case 3: // VisionPage
                if (visionpage) {
                    // 使用QMetaObject::invokeMethod安全地调用stopRecording方法
                    QMetaObject::invokeMethod(visionpage, "stopRecording", Qt::QueuedConnection);
                    
                    // 延迟页面切换以确保资源正确释放
                    QTimer::singleShot(500, this, [this]() {
                        stackedWidget->setCurrentIndex(0);
                        updateIndicator();
                        PageChangeEvent(0);
                    });
                    return; // 提前返回，因为我们已经安排了延迟页面切换
                }
                break;
                
            default:
                qWarning() << "未知子页面类型:" << pageType;
                break;
        }
        
        // 默认的返回处理：切换回主页面
        stackedWidget->setCurrentIndex(0);
        updateIndicator();
        // 确保调用 PageChangeEvent 来重启手势识别
        PageChangeEvent(0);
        qDebug() << "已返回主页";
    } else {
        qDebug() << "已经在主页，无需操作";
    }
}

void MainWindow::createBatteryIndicator()
{
    // Create battery icon widget
    batteryIconLabel = new QLabel(this);
    batteryIconLabel->setFixedSize(50, 20);
    batteryLevel = 10; // Default to 1%
    
    // Set the initial icon
    updateBatteryIcon();
    
    // Position in the top-right corner
    batteryIconLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    batteryIconLabel->move(width() - batteryIconLabel->width() - 10, 10);
    
    // Ensure it stays visible
    batteryIconLabel->raise();
    batteryIconLabel->show();
}

void MainWindow::updateBatteryIcon()
{
    // Create a pixmap for drawing the battery icon
    QPixmap pixmap(50, 20);
    pixmap.fill(Qt::transparent);
    
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Draw battery outline
    painter.setPen(QPen(Qt::black, 1));
    painter.setBrush(Qt::white);
    painter.drawRect(0, 0, 40, 20);
    
    // Draw battery terminal
    painter.drawRect(40, 5, 5, 10);
    
    // Calculate filled width based on battery level
    int filledWidth = (batteryLevel * 38) / 100;
    
    // Choose color based on battery level
    if (batteryLevel < 20) {
        painter.setBrush(Qt::red);
    } else if (batteryLevel < 50) {
        painter.setBrush(Qt::yellow);
    } else {
        painter.setBrush(Qt::green);
    }
    
    // Draw battery fill level
    painter.drawRect(1, 1, filledWidth, 18);
    
    // Add percentage text
    painter.setPen(Qt::black);
    painter.setFont(QFont("Arial", 8, QFont::Bold));
    painter.drawText(QRect(0, 0, 40, 20), Qt::AlignCenter, QString("%1%").arg(batteryLevel));
    
    painter.end();
    
    // Set the pixmap to the label
    batteryIconLabel->setPixmap(pixmap);
}

void MainWindow::updateBatteryLevel(int level)
{
    // Ensure level is between 0 and 100
    batteryLevel = qBound(0, level, 100);
    updateBatteryIcon();
}