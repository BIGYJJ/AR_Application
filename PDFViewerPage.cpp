// PDFViewerPage.cpp
#include "PDFViewerPage.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QPainter>
#include <QShowEvent>
#include <QMediaDevices>
#include <QSlider>

using namespace cv;

PDFViewerPage::PDFViewerPage(QWidget *parent)
    : QOpenGLWidget(parent),
      pdfDocument(new QPdfDocument(this)),
      currentPage(0),
      pdfOpacity(0.7),
      desktopDetected(false),
      desktopLocked(false),
      selectionMode(false),
      kf(8, 8, 0),
      camera(nullptr),
      cameraAvailable(false),
      maxCacheSize(5),
      lastRequestedPage(-1)
{
    setupUI();
    // Initialize Kalman filter parameters
    setupKalmanFilter();
    // Initialize ArUco detector
    initArUcoDetector();
    //setupCamera();  // Remove this line to prevent auto initialization
    // 初始化陀螺仪
    initGyroscope();
}

void PDFViewerPage::setupKalmanFilter()
{
    // 为位姿跟踪设置卡尔曼滤波器
    // 状态向量: [x, y, z, R11, R12, R13, R21, R22, R23, R31, R32, R33, 
    //           vx, vy, vz, vR11, vR12, vR13, vR21, vR22, vR23, vR31, vR32, vR33]
    // 包含位置(3)、旋转矩阵(9)及其对应的速度(12)，共24维
    kf.init(24, 12, 0);
    
    // 转移矩阵：恒定速度模型
    cv::setIdentity(kf.transitionMatrix);
    
    // 添加速度到位置的关系
    for (int i = 0; i < 12; i++) {
        kf.transitionMatrix.at<float>(i, i + 12) = 1.0f;
    }
    
    // 观测矩阵（仅观测位置和旋转，不观测速度）
    cv::Mat measurement = cv::Mat::zeros(12, 24, CV_32F);
    for (int i = 0; i < 12; i++) {
        measurement.at<float>(i, i) = 1.0f;
    }
    kf.measurementMatrix = measurement;
    
    // 过程噪声协方差
    // 位置和旋转部分
    for (int i = 0; i < 12; i++) {
        kf.processNoiseCov.at<float>(i, i) = 1e-4f;
    }
    // 速度部分
    for (int i = 12; i < 24; i++) {
        kf.processNoiseCov.at<float>(i, i) = 1e-3f;
    }
    
    // 测量噪声协方差
    cv::setIdentity(kf.measurementNoiseCov, cv::Scalar::all(1e-2f));
    
    // 误差协方差
    cv::setIdentity(kf.errorCovPost, cv::Scalar::all(1e-1f));
    
    kalmanInitialized = false;
}

PDFViewerPage::~PDFViewerPage()
{
    // 确保摄像头被释放
    if (camera && camera->isActive()) {
        try {
            camera->stop();
            qDebug() << "PDFViewerPage析构: 摄像头已停止";
        } catch (...) {
            qWarning() << "PDFViewerPage析构: 停止摄像头时发生异常";
        }
    }

     // 关闭串口
     if (serialPort && serialPort->isOpen()) {
        serialPort->close();
        qDebug() << "PDFViewerPage析构: 串口已关闭";
    }
  
    if (m_arucoProcessor) {
        m_arucoProcessor->stop();
        m_arucoProcessor->wait();
    }
}

void PDFViewerPage::setupUI()
{
     // Create basic layout
     QVBoxLayout *mainLayout = new QVBoxLayout(this);
    
     // Top toolbar
     QHBoxLayout *toolbarLayout = new QHBoxLayout();
     backButton = new QPushButton("返回", this);
     resetButton = new QPushButton("重置跟踪", this);
     startCameraButton = new QPushButton("启动摄像头", this);
     stopCameraButton = new QPushButton("停止摄像头", this);
     QPushButton *generateArUcoButton = new QPushButton("生成ArUco标记", this);
     QPushButton *emergencyResetButton = new QPushButton("紧急重置", this);
     QLabel *uploadHintLabel = new QLabel("请使用微信小程序上传PDF文件", this);
  

     emergencyResetButton->setStyleSheet("background-color: red; color: white; font-weight: bold;");
     resetButton->setEnabled(false);
     stopCameraButton->setEnabled(false);
     
     toolbarLayout->addWidget(backButton);
     toolbarLayout->addWidget(resetButton);
     toolbarLayout->addWidget(startCameraButton);
     toolbarLayout->addWidget(stopCameraButton);
     toolbarLayout->addWidget(uploadHintLabel);
     toolbarLayout->addWidget(generateArUcoButton);
     toolbarLayout->addWidget(emergencyResetButton);
     toolbarLayout->addStretch();
    
    // Page navigation controls
    prevPageButton = new QPushButton("上一页", this);
    nextPageButton = new QPushButton("下一页", this);
    prevPageButton->setEnabled(false);
    nextPageButton->setEnabled(false);
    
    toolbarLayout->addWidget(prevPageButton);
    toolbarLayout->addWidget(nextPageButton);
    
    // Opacity control
    QHBoxLayout *opacityLayout = new QHBoxLayout();
    QLabel *opacityLabel = new QLabel("PDF透明度:", this);
    opacitySlider = new QSlider(Qt::Horizontal, this);
    opacitySlider->setRange(10, 100);
    opacitySlider->setValue(70);  // Default 70% opacity
    
    opacityLayout->addWidget(opacityLabel);
    opacityLayout->addWidget(opacitySlider);
    
    // Main view area - only show the rendered view
    QHBoxLayout *viewLayout = new QHBoxLayout();
    
    // Hide the viewfinder (camera feed)
    viewfinder = new QVideoWidget(this);
    viewfinder->setVisible(false);
    viewfinder->setMinimumSize(300, 300);
    
    // Main display area for rendered PDF on black background
    processedLabel = new QLabel(this);
    processedLabel->setMinimumHeight(400);  // Increased height for better visibility
    processedLabel->setAlignment(Qt::AlignCenter);
    processedLabel->setStyleSheet("background-color: #000000; color: white; border: 1px solid #555;");
    
    viewLayout->addWidget(processedLabel, 1);
    
    // Status bar
    statusLabel = new QLabel("请导入PDF并将相机对准桌面", this);
    statusLabel->setAlignment(Qt::AlignCenter);
      QHBoxLayout *performanceLayout = new QHBoxLayout();
    // Assemble layout
    mainLayout->addLayout(toolbarLayout);
    mainLayout->addLayout(opacityLayout);
     mainLayout->addLayout(performanceLayout);
    mainLayout->addLayout(viewLayout, 1);
    mainLayout->addWidget(statusLabel);
    
    // Setup camera but don't show it
    //setupCamera();
    
    // Connect signals and slots
    connect(emergencyResetButton, &QPushButton::clicked, this, [this]() {
        // Stop camera
        if (camera && camera->isActive()) {
            camera->stop();
        }
        
        
        
        // Reset all states
        desktopLocked = false;
        desktopDetected = false;
        lockedDesktopContour.clear();
        prevFeaturePoints.clear();
        prevGray = cv::Mat();
        
        // Clear image cache
        currentPdfFrame = QImage();
        
        // Reset UI
        processedLabel->clear();
        
        // Release memory
        cv::destroyAllWindows();
        
        statusLabel->setText("系统已重置，请重新启动摄像头");
    });
    
    connect(backButton, &QPushButton::clicked, this, [this]() {
   
        
        // Then call the regular back button handler
        this->onBackButtonClicked();
    });
    
    connect(resetButton, &QPushButton::clicked, this, &PDFViewerPage::resetDesktopDetection);
    connect(prevPageButton, &QPushButton::clicked, this, &PDFViewerPage::prevPage);
    connect(nextPageButton, &QPushButton::clicked, this, &PDFViewerPage::nextPage);
    connect(opacitySlider, &QSlider::valueChanged, [this](int value) {
        pdfOpacity = value / 100.0;
    });
    
    // Start camera button handler
    connect(startCameraButton, &QPushButton::clicked, this, [this]() {
        statusLabel->setText("尝试启动摄像头...");
        startCameraButton->setEnabled(false);
        
        // 使用延迟启动避免阻塞UI
        QTimer::singleShot(1000, this, [this]() {
            this->setupCamera();
            
            // 进一步延迟摄像头启动
            QTimer::singleShot(500, this, [this]() {
                if (camera) {
                    camera->start();
                    statusLabel->setText("摄像头已启动");
                    stopCameraButton->setEnabled(true);
                } else {
                    statusLabel->setText("摄像头启动失败");
                    startCameraButton->setEnabled(true);
                }
            });
        });
    });
    
    connect(stopCameraButton, &QPushButton::clicked, this, [this]() {
        if (camera && camera->isActive()) {
            camera->stop();
            stopCameraButton->setEnabled(false);
            startCameraButton->setEnabled(true);
            statusLabel->setText("摄像头已停止");
            
            // Clear the processed view
            processedLabel->clear();
        }
    });

   

    connect(generateArUcoButton, &QPushButton::clicked, this, &PDFViewerPage::generateAndShowArUcoMarkers);

    // Create ArUco processor thread
    m_arucoProcessor = new ArUcoProcessorThread(this);
    
    // Connect signals and slots
    connect(m_arucoProcessor, &ArUcoProcessorThread::markersDetected,
            this, &PDFViewerPage::handleDetectedMarkers);
    
    // Start thread
    m_arucoProcessor->start();
    
    // Add SLAM engine selector
    QComboBox *slamEngineSelector = new QComboBox(this);
    slamEngineSelector->addItem("基础跟踪 (默认)");
    slamEngineSelector->addItem("特征点SLAM");
    slamEngineSelector->addItem("光流+惯性SLAM");
    
    QLabel *slamLabel = new QLabel("空间锚定方式:", this);
    toolbarLayout->addWidget(slamLabel);
    toolbarLayout->addWidget(slamEngineSelector);
    
    connect(slamEngineSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), 
           this, [this](int index) {
        switch(index) {
            case 0:
                // Basic tracking
                slamMode = SlamMode::Basic;
                break;
            case 1:
                // Feature point SLAM
                slamMode = SlamMode::FeaturePoint;
                break;
            case 2:
                // Optical flow + inertial SLAM
                slamMode = SlamMode::OpticalFlow;
                break;
        }
        resetDesktopDetection();
    });
    
    // Add ArUco checkbox
    enableArucoCheckbox = new QCheckBox("启用ArUco标记跟踪", this);
    enableArucoCheckbox->setChecked(true);  // Enable by default
    connect(enableArucoCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        useArUcoTracking = checked;
        if (checked && !desktopLocked) {
            statusLabel->setText("ArUco跟踪已启用，请确保标记可见");
        } else if (!checked && desktopLocked) {
            statusLabel->setText("已切换到常规跟踪模式");
        }
    });


   
    
    m_useThreadPoolCheckbox = new QCheckBox("启用线程池加速", this);
    m_useThreadPoolCheckbox->setChecked(m_useThreadPool);
    connect(m_useThreadPoolCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        m_useThreadPool = checked;
        if (checked) {
            statusLabel->setText("线程池已启用，性能将提升");
        } else {
            statusLabel->setText("线程池已禁用，使用单线程处理");
        }
    });
    
    m_performanceLabel = new QLabel("处理性能: 等待中", this);
    m_performanceLabel->setMinimumWidth(250);
    
    // 添加线程数控制下拉框
    QComboBox *threadCountCombo = new QComboBox(this);
    threadCountCombo->addItem("自动线程数", 0);
    for (int i = 1; i <= QThread::idealThreadCount(); i++) {
        threadCountCombo->addItem(QString("%1 线程").arg(i), i);
    }
    connect(threadCountCombo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, [this, threadCountCombo](int index) {
        int threadCount = threadCountCombo->currentData().toInt();
        if (threadCount == 0) {
            // 自动模式 - 使用CPU核心数减1
            ThreadPool::instance().setThreadCount(qMax(2, QThread::idealThreadCount() - 1));
        } else {
            ThreadPool::instance().setThreadCount(threadCount);
        }
        statusLabel->setText(QString("线程池大小已调整为: %1").arg(ThreadPool::instance().threadCount()));
    });
    
    // 创建帧率显示刷新定时器
    QTimer *fpsTimer = new QTimer(this);
    connect(fpsTimer, &QTimer::timeout, this, [this]() {
        if (m_performanceLabel) {
            m_performanceLabel->setText(QString("FPS: %1 | 线程: %2/%3 | 模式: %4")
                .arg(m_currentFps, 0, 'f', 1)
                .arg(ThreadPool::instance().activeThreadCount())
                .arg(ThreadPool::instance().threadCount())
                .arg(m_lowPerformanceMode ? "低性能" : "标准"));
        }
    });
    fpsTimer->start(500); // 每500ms更新一次
    
    QLabel *threadsLabel = new QLabel("线程数:", this);
    performanceLayout->addWidget(m_useThreadPoolCheckbox);
    performanceLayout->addWidget(threadsLabel);
    performanceLayout->addWidget(threadCountCombo);
    performanceLayout->addWidget(m_performanceLabel);
    performanceLayout->addStretch();
    
    // 将性能布局添加到主布局
    // 注意：这里需要根据您现有的布局结构进行适当调整
 
    mainLayout->addWidget(enableArucoCheckbox);
    // 初始化线程池
    ThreadPool::instance().setThreadCount(qMax(2, QThread::idealThreadCount() - 1));
    qDebug() << "线程池初始化完成，线程数:" << ThreadPool::instance().threadCount();

    
    //toolbarLayout->addWidget(enableArucoCheckbox);
}




// 修改startAreaSelection函数
void PDFViewerPage::startAreaSelection()
{
    if (!camera || !camera->isActive()) {
        statusLabel->setText("请先启动摄像头");
        return;
    }
    
    selectionMode = true;
    selectionStart = QPoint(); // 重置选择起点
    selectionRect = QRect();
    statusLabel->setText("请在视频上拖动鼠标框选桌面区域...");
    viewfinder->setCursor(Qt::CrossCursor);
    
    // 禁用其他按钮防止干扰
    resetButton->setEnabled(false);
    importButton->setEnabled(false);
}


void PDFViewerPage::setupCamera()
{
    try {
        qDebug() << "PDFViewerPage: 安全初始化摄像头 (开始)";
        
        // 首先检查是否已有摄像头实例并停止它
        if (camera && camera->isActive()) {
            camera->stop();
            QThread::msleep(500);
            qDebug() << "PDFViewerPage: 已停止现有摄像头";
        }
        
        // 获取资源管理器实例
        auto& cameraManager = CameraResourceManager::instance();
        
        // 显式请求摄像头资源 - 关键步骤!
        CameraRequest request;
        request.requesterId = "PDFViewer";
        request.priority = RequestPriority::Normal;
        request.preferredCameraIndex = 0;  // 明确使用video0
        request.exclusive = true;
        
        // 等待获取摄像头资源
        bool resourceGranted = cameraManager.requestCamera(request);
        if (!resourceGranted) {
            statusLabel->setText("无法获取摄像头资源 - 设备可能正在被其他功能使用");
            qDebug() << "PDFViewerPage: 摄像头资源请求被拒绝";
            return;
        }
        
        // 得到分配的摄像头索引
        int allocatedIndex = -1;
        QMap<int, QString> users = cameraManager.getCameraUsers();
for (auto it = users.begin(); it != users.end(); ++it) {
    if (it.value() == "PDFViewer") {
        allocatedIndex = it.key();
        break;
    }
}
        
        if (allocatedIndex < 0) {
            qWarning() << "PDFViewerPage: 资源管理器分配了资源但未找到索引";
            allocatedIndex = 0; // 回退到默认值
        }
        
        qDebug() << "PDFViewerPage: 资源管理器分配的摄像头索引:" << allocatedIndex;
        
        // 获取摄像头列表
        const auto cameras = QMediaDevices::videoInputs();
        if (cameras.isEmpty()) {
            statusLabel->setText("未检测到摄像头设备");
            cameraManager.releaseCamera("PDFViewer"); // 重要：释放已获取的资源
            return;
        }
        
        // 确保索引在有效范围内
        if (allocatedIndex >= cameras.size()) {
            allocatedIndex = 0;
        }
        
        // 创建摄像头对象并配置
        camera = new QCamera(cameras[allocatedIndex], this);
        
        // 设置捕获会话
        captureSession.setCamera(camera);
        captureSession.setVideoOutput(viewfinder);
        
        // 获取视频接收器
        videoSink = captureSession.videoSink();
        if (!videoSink) {
            cameraManager.releaseCamera("PDFViewer"); // 重要：释放已获取的资源
            throw std::runtime_error("无法获取视频接收器");
        }
        
        // 连接视频帧处理信号
        disconnect(videoSink, &QVideoSink::videoFrameChanged, this, &PDFViewerPage::processFrame);
        connect(videoSink, &QVideoSink::videoFrameChanged, this, &PDFViewerPage::processFrame);
        
        startCameraButton->setEnabled(true);
        stopCameraButton->setEnabled(false);
        
        statusLabel->setText("摄像头资源已成功分配，可以手动启动");
        qDebug() << "PDFViewerPage: 安全初始化摄像头 (完成)";
        
    } catch (const std::exception& e) {
        qWarning() << "PDFViewerPage: setupCamera中发生异常:" << e.what();
        auto& cameraManager = CameraResourceManager::instance();
        cameraManager.releaseCamera("PDFViewer"); // 异常时也要释放资源
        
        if (camera) {
            delete camera;
            camera = nullptr;
        }
        statusLabel->setText("摄像头初始化异常");
    } catch (...) {
        qWarning() << "PDFViewerPage: setupCamera中发生未知异常";
        auto& cameraManager = CameraResourceManager::instance();
        cameraManager.releaseCamera("PDFViewer"); // 异常时也要释放资源
        
        if (camera) {
            delete camera;
            camera = nullptr;
        }
        statusLabel->setText("摄像头初始化未知错误");
    }
}


// 添加系统摄像头释放函数
void PDFViewerPage::releaseSystemCameras()
{
    qDebug() << "PDFViewerPage: 安全释放摄像头资源";
    
    // 只停止摄像头，不删除对象
    if (camera && camera->isActive()) {
        camera->stop();
        qDebug() << "PDFViewerPage: 摄像头已停止";
    }
    
    // 延迟确保资源释放 - 保留但减少延迟时间
    QThread::msleep(500);
    
    qDebug() << "PDFViewerPage: 资源释放完成";
}

void PDFViewerPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    
    // Only update UI elements, don't initialize camera
    startCameraButton->setEnabled(true);
    stopCameraButton->setEnabled(false);
    statusLabel->setText("请在主界面点击'启动摄像头'按钮手动启动摄像头");
    
    qDebug() << "PDFViewerPage显示，等待用户从主界面初始化摄像头";
}






void PDFViewerPage::mousePressEvent(QMouseEvent *event)
{
    if (selectionMode) {
        // 记录选择起点
        QPoint mappedPoint = mapToGlobal(event->pos());
        mappedPoint = viewfinder->mapFromGlobal(mappedPoint);
        
        if (viewfinder->rect().contains(mappedPoint)) {
            selectionStart = mappedPoint;
        }
    } else if (desktopLocked && viewfinder->rect().contains(event->pos())) {
        // 原有的触摸处理代码
        touchStartPos = event->pos();
        isTouching = true;
    }
    QWidget::mousePressEvent(event);
}

void PDFViewerPage::mouseMoveEvent(QMouseEvent *event)
{
    if (selectionMode && !selectionStart.isNull()) {
        QPoint mappedPoint = viewfinder->mapFrom(this, event->pos());
        if (viewfinder->rect().contains(mappedPoint)) {
            selectionRect = QRect(selectionStart, mappedPoint).normalized();
            update();
        }
    }
    QWidget::mouseMoveEvent(event);
}

void PDFViewerPage::mouseReleaseEvent(QMouseEvent *event)
{
    if (selectionMode && !selectionStart.isNull()) {
        // 完成选择
        QPoint mappedPoint = mapToGlobal(event->pos());
        mappedPoint = viewfinder->mapFromGlobal(mappedPoint);
        
        if (viewfinder->rect().contains(mappedPoint)) {
            selectionRect = QRect(selectionStart, mappedPoint).normalized();
            
            // 将QRect转换为OpenCV轮廓
            std::vector<cv::Point> selectedContour = {
                cv::Point(selectionRect.left(), selectionRect.top()),
                cv::Point(selectionRect.right(), selectionRect.top()),
                cv::Point(selectionRect.right(), selectionRect.bottom()),
                cv::Point(selectionRect.left(), selectionRect.bottom())
            };
            
            // 使用选择的区域作为桌面
            lockedDesktopContour = selectedContour;
            desktopLocked = true;
            desktopDetected = true;
            
            // 重置选择状态
            selectionMode = false;
            viewfinder->setCursor(Qt::ArrowCursor);
            statusLabel->setText("已手动设置桌面区域");
            
            // 重新启用按钮
            resetButton->setEnabled(true);
            importButton->setEnabled(true);
        }
    } else if (isTouching) {
        // 计算滑动距离和方向
        QPoint delta = event->pos() - touchStartPos;
        
        // 如果横向滑动足够大，切换页面
        if (abs(delta.x()) > 100 && abs(delta.x()) > abs(delta.y()) * 2) {
            if (delta.x() > 0) {
                // 右滑，上一页
                prevPage();
            } else {
                // 左滑，下一页
                nextPage();
            }
        }
        
        isTouching = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void PDFViewerPage::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    
    if (selectionMode && !selectionRect.isNull()) {
        QPainter painter(this);
        painter.setPen(QPen(Qt::red, 2, Qt::SolidLine));
        QRect globalRect(viewfinder->mapTo(this, selectionRect.topLeft()),
                        viewfinder->mapTo(this, selectionRect.bottomRight()));
        painter.drawRect(globalRect);
    }
}

// 添加键盘快捷键支持
void PDFViewerPage::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Up:
            prevPage();
            break;
            
        case Qt::Key_Right:
        case Qt::Key_Down:
        case Qt::Key_Space:
            nextPage();
            break;
            
        case Qt::Key_R:
            resetDesktopDetection();
            break;
            
        case Qt::Key_Escape:
            onBackButtonClicked();
            break;
            
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            opacitySlider->setValue(opacitySlider->value() + 5);
            break;
            
        case Qt::Key_Minus:
            opacitySlider->setValue(opacitySlider->value() - 5);
            break;
            
        default:
            QWidget::keyPressEvent(event);
    }
}


// 添加设备方向变化处理
void PDFViewerPage::orientationChanged(Qt::ScreenOrientation orientation)
{
    // 重置桌面检测
    resetDesktopDetection();
    
    // 如果有PDF已加载，重新渲染以适应新方向
    if (!currentPdfFrame.isNull()) {
        renderCurrentPDFToImage(viewfinder->size());
    }
    
    statusLabel->setText("设备方向已改变，请重新定位桌面");
}

void PDFViewerPage::initCamera()
{
    const auto cameras = QMediaDevices::videoInputs();
    if (cameras.isEmpty()) {
        statusLabel->setText("未检测到摄像头");
        return;
    }

    camera = new QCamera(cameras.first(), this);
    captureSession.setCamera(camera);
    captureSession.setVideoOutput(viewfinder);
    videoSink = captureSession.videoSink();
    connect(videoSink, &QVideoSink::videoFrameChanged, this, &PDFViewerPage::processFrame);
}

void PDFViewerPage::startCamera()
{
    try {
        qDebug() << "PDFViewerPage: 安全启动摄像头 (开始)";
        
        // 通知用户正在启动摄像头
        statusLabel->setText("正在启动摄像头...");
        startCameraButton->setEnabled(false);
        
        // 如果摄像头对象不存在，提示用户需要先初始化
        if (!camera) {
            qDebug() << "PDFViewerPage: 摄像头未初始化，无法启动";
            statusLabel->setText("请先在主界面点击PDFVIEW按钮初始化摄像头");
            startCameraButton->setEnabled(true);
            return;
        }
        
        // 检查摄像头是否成功创建
        if (!camera) {
            qWarning() << "PDFViewerPage: 摄像头对象不存在，无法启动";
            statusLabel->setText("摄像头初始化失败，请重试");
            startCameraButton->setEnabled(true);
            return;
        }
        
        // 安全启动摄像头
        try {
            qDebug() << "PDFViewerPage: 尝试启动摄像头";
            camera->start();
            
            // 等待短暂时间检查是否成功启动
            QThread::msleep(500);
            
            if (camera->isActive()) {
                qDebug() << "PDFViewerPage: 摄像头启动成功";
                statusLabel->setText("正在检测桌面...");
                
                // 更新按钮状态
                startCameraButton->setEnabled(false);
                stopCameraButton->setEnabled(true);
                resetButton->setEnabled(false);  // 直到桌面被检测到才启用
            } else {
                qWarning() << "PDFViewerPage: 摄像头启动失败";
                statusLabel->setText("摄像头启动失败，请重试");
                startCameraButton->setEnabled(true);
            }
        } catch (const std::exception& e) {
            qWarning() << "PDFViewerPage: 启动摄像头时发生异常:" << e.what();
            statusLabel->setText("摄像头启动异常，请重试");
            startCameraButton->setEnabled(true);
        }
        
        qDebug() << "PDFViewerPage: 安全启动摄像头 (完成)";
    } catch (const std::exception& e) {
        qWarning() << "PDFViewerPage: startCamera中发生未捕获的异常:" << e.what();
        statusLabel->setText("摄像头启动异常，请重试");
        startCameraButton->setEnabled(true);
    } catch (...) {
        qWarning() << "PDFViewerPage: startCamera中发生未知异常";
        statusLabel->setText("摄像头启动未知错误，请重试");
        startCameraButton->setEnabled(true);
    }
}

void PDFViewerPage::resetDesktopDetection()
{
    desktopLocked = false;
    desktopDetected = false;
    lockedDesktopContour.clear();
    prevFeaturePoints.clear();
    prevGray = cv::Mat();
    statusLabel->setText("重置桌面检测，请将文档平放在桌面上");
    resetButton->setEnabled(false);
}


void PDFViewerPage::networkLoadPDF(const QByteArray& pdfData)
{
    if (pdfData.isEmpty()) {
        statusLabel->setText("接收到的PDF数据为空");
        return;
    }
    
    statusLabel->setText("从网络接收PDF数据...");
    
    // 创建临时文件保存接收到的PDF数据
    QTemporaryFile tempFile;
    if (!tempFile.open()) {
        statusLabel->setText("无法创建临时文件");
        return;
    }
    
    // 写入PDF数据
    tempFile.write(pdfData);
    tempFile.flush();
    
    // 加载PDF文件
    if (pdfDocument->load(tempFile.fileName()) != QPdfDocument::Error::None) {
        statusLabel->setText("PDF加载失败");
        return;
    }
    
    // 设置初始页面
    currentPage = 0;
    
    // 渲染PDF
    renderCurrentPDFToImage(QSize(800, 1131));
    
    // 启用导航按钮
    nextPageButton->setEnabled(pdfDocument->pageCount() > 1);
    prevPageButton->setEnabled(false);
    
    statusLabel->setText(QString("PDF已加载，页数: %1").arg(pdfDocument->pageCount()));
}

void PDFViewerPage::nextPage()
{
    if (currentPage < pdfDocument->pageCount() - 1) {
        currentPage++;
        
        // 重要：强制触发PDF重新渲染
        currentPdfFrame = QImage(); // 清空当前图像缓存，强制重新渲染
        
        // 立即渲染新页面
        renderCurrentPDFToImage(viewfinder->size());
        
        // 更新导航按钮状态
        prevPageButton->setEnabled(true);
        nextPageButton->setEnabled(currentPage < pdfDocument->pageCount() - 1);
        
        // 日志输出帮助调试
        qDebug() << "切换到页面:" << (currentPage + 1) << "/" << pdfDocument->pageCount();
        
        // 更新状态标签
        statusLabel->setText(QString("当前页面: %1/%2").arg(currentPage + 1).arg(pdfDocument->pageCount()));
        
        // 强制重新渲染AR视图
        update();
    }
}

void PDFViewerPage::prevPage()
{
    if (currentPage > 0) {
        currentPage--;
        
        // 重要：强制触发PDF重新渲染
        currentPdfFrame = QImage(); // 清空当前图像缓存，强制重新渲染
        
        // 立即渲染新页面
        renderCurrentPDFToImage(viewfinder->size());
        
        // 更新导航按钮状态
        prevPageButton->setEnabled(currentPage > 0);
        nextPageButton->setEnabled(true);
        
        // 日志输出帮助调试
        qDebug() << "切换到页面:" << (currentPage + 1) << "/" << pdfDocument->pageCount();
        
        // 更新状态标签
        statusLabel->setText(QString("当前页面: %1/%2").arg(currentPage + 1).arg(pdfDocument->pageCount()));
        
        // 强制重新渲染AR视图
        update();
    }
}




void PDFViewerPage::enhancedDynamicOverlay(cv::Mat& frame, const std::vector<cv::Point>& contour)
{
     // Ensure exactly four points are provided
     if (contour.size() != 4 || currentPdfFrame.isNull()) return;

     // Order the contour points
     std::vector<cv::Point2f> orderedContour = orderPoints(contour);
     if (orderedContour.size() != 4) return;
 
     // Source points from the PDF image - 确保正确初始化
     std::vector<cv::Point2f> srcPoints;
     if (pdfCorners.size() == 4) {
         srcPoints = pdfCorners;
     } else {
         srcPoints = {
             cv::Point2f(0, 0),
             cv::Point2f(currentPdfFrame.width(), 0),
             cv::Point2f(currentPdfFrame.width(), currentPdfFrame.height()),
             cv::Point2f(0, currentPdfFrame.height())
         };
     }
 
     // Destination points are the ordered contour points
     std::vector<cv::Point2f> dstPoints = orderedContour;
 
     // Compute homography matrix using RANSAC for robustness
     cv::Mat H = cv::findHomography(srcPoints, dstPoints, cv::RANSAC, 3.0);
     if (H.empty()) {
         statusLabel->setText("Homography calculation failed.");
         return;
     }
 
     // 确保PDF图像正确加载并转换格式
     cv::Mat pdfMat;
     try {
         // 确保PDF图像的格式和通道数正确
         QImage tempImage = currentPdfFrame.convertToFormat(QImage::Format_RGB888);
         pdfMat = cv::Mat(tempImage.height(), tempImage.width(), CV_8UC3, 
                         const_cast<uchar*>(tempImage.bits()));
         cv::cvtColor(pdfMat, pdfMat, cv::COLOR_RGB2BGR);
     } catch (const std::exception& e) {
         statusLabel->setText(QString("PDF转换错误: %1").arg(e.what()));
         return;
     }
 
     // Warp the PDF image to fit the destination contour
     cv::Mat warped;
     try {
         // 确保使用正确的大小和插值方法
         cv::warpPerspective(pdfMat, warped, H, frame.size(),
                          cv::INTER_LINEAR, cv::BORDER_CONSTANT);
     } catch (const std::exception& e) {
         statusLabel->setText(QString("透视变换错误: %1").arg(e.what()));
         return;
     }
 
     // Create a mask for blending
     cv::Mat mask = cv::Mat::zeros(frame.size(), CV_8UC1);
     std::vector<cv::Point> intOrderedContour;
     for (const auto& p : orderedContour) {
         intOrderedContour.emplace_back(p.x, p.y);
     }
     std::vector<std::vector<cv::Point>> fillCont = { intOrderedContour };
     cv::fillPoly(mask, fillCont, cv::Scalar(255));
     cv::GaussianBlur(mask, mask, cv::Size(7, 7), 2.0);
 
     // 修复: 确保所有矩阵有相同的通道数和类型
     if (warped.type() != frame.type()) {
         // 将warped转换为frame的类型
         warped.convertTo(warped, frame.type());
     }
     
     // 正确使用遮罩进行alpha混合
     try {
         // 创建与frame相同类型和大小的临时矩阵
         cv::Mat tempFrame = frame.clone();
         
         // 将warped和原frame按照mask和不透明度混合
         double alpha = pdfOpacity;
         cv::Mat blendedRegion;
         cv::addWeighted(warped, alpha, tempFrame, 1.0 - alpha, 0.0, blendedRegion);
         
         // 使用mask将混合结果拷贝回frame
         blendedRegion.copyTo(frame, mask);
     } catch (const std::exception& e) {
         statusLabel->setText(QString("图像混合错误: %1").arg(e.what()));
         return;
     }
}

void PDFViewerPage::enhancedOverlayPDF(cv::Mat& frame, const std::vector<cv::Point>& contour){
    // Parameter check
    if (contour.size() != 4 || currentPdfFrame.isNull()) {
        return;
    }

    try {
        // Source points from the PDF image
        std::vector<cv::Point2f> srcPoints = {
            cv::Point2f(0, 0),
            cv::Point2f(currentPdfFrame.width(), 0),
            cv::Point2f(currentPdfFrame.width(), currentPdfFrame.height()),
            cv::Point2f(0, currentPdfFrame.height())
        };
        
        // Use more precise contour ordering algorithm
        std::vector<cv::Point2f> dstPoints = orderPoints(contour);
        
        // Calculate perspective transformation matrix
        cv::Mat homography = cv::findHomography(srcPoints, dstPoints);
        if (homography.empty()) return;

        // Create PDF matrix, ensure correct format and channels
        QImage tempImage = currentPdfFrame.convertToFormat(QImage::Format_RGB888);
        cv::Mat pdfMat(tempImage.height(), tempImage.width(), CV_8UC3,
                     const_cast<uchar*>(tempImage.bits()));
        cv::cvtColor(pdfMat, pdfMat, cv::COLOR_RGB2BGR);
        
        // Use higher quality perspective transform settings
        cv::Mat warped = cv::Mat::zeros(frame.size(), CV_8UC3);
        cv::warpPerspective(pdfMat, warped, homography, frame.size(), 
                           cv::INTER_LINEAR);
        
        // Create mask to determine visible PDF area
        cv::Mat mask = cv::Mat::zeros(frame.size(), CV_8UC1); // Ensure single channel
        std::vector<std::vector<cv::Point>> contours = {std::vector<cv::Point>(dstPoints.begin(), dstPoints.end())};
        cv::fillPoly(mask, contours, cv::Scalar(255));
        
        // Use larger blur radius for edge smoothing
        cv::GaussianBlur(mask, mask, cv::Size(9, 9), 3.0);
        
        // Copy the warped PDF directly to the frame using the mask
        // Since the frame is already black, we don't need to blend
        warped.copyTo(frame, mask);
        
        // Draw border for visual effect
        cv::polylines(frame, contours, true, cv::Scalar(0, 255, 255), 2);
    } catch (const std::exception& e) {
        // Log error but don't interrupt processing
        qWarning() << "PDFViewerPage: Exception in enhancedOverlayPDF:" << e.what();
    }
}



// 辅助函数：对四边形顶点进行排序，确保顺序为左上、右上、右下、左下
std::vector<cv::Point2f> PDFViewerPage::orderPoints(const std::vector<cv::Point>& pts)
{
    if (pts.size() != 4) return std::vector<cv::Point2f>();

    std::vector<cv::Point2f> ordered(4);
    std::vector<cv::Point2f> points(pts.begin(), pts.end());

    // 计算中心点
    cv::Point2f center(0, 0);
    for (const auto& p : points) {
        center += p;
    }
    center *= 0.25f;
    
    // 计算各点相对于中心点的角度
    std::vector<std::pair<float, int>> angles;
    for (int i = 0; i < 4; i++) {
        // 计算角度 (使用atan2确保正确的象限)
        float angle = std::atan2(points[i].y - center.y, points[i].x - center.x);
        // 转换到0-360度
        angle = angle * 180.0f / M_PI;
        if (angle < 0) angle += 360.0f;
        angles.push_back(std::make_pair(angle, i));
    }
    
    // 按角度排序
    std::sort(angles.begin(), angles.end());
    
    // 重新排序为左上、右上、右下、左下
    // 注意：我们假设摄像头视角大致是俯视的
    ordered[0] = points[angles[2].second]; // 左上 (第3个点，约270度)
    ordered[1] = points[angles[3].second]; // 右上 (第4个点，约315度)
    ordered[2] = points[angles[0].second]; // 右下 (第1个点，约0度)
    ordered[3] = points[angles[1].second]; // 左下 (第2个点，约45度)
    
    return ordered;
}



bool PDFViewerPage::detectDesktop(Mat& frame, std::vector<cv::Point>& contour)
{
    cv::Mat gray, blurred, edges;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    
    // 显示原始图像和处理阶段
    cv::putText(frame, "原图", cv::Point(20, 30), 
               cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 2);
    
    // 使用自适应阈值处理，提高白纸检测能力
    cv::Mat thresh;
    cv::adaptiveThreshold(gray, thresh, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                         cv::THRESH_BINARY_INV, 11, 2);
    
    // 针对白纸，增加形态学处理
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
    cv::morphologyEx(thresh, thresh, cv::MORPH_CLOSE, kernel);
    
    // 边缘检测 - 使用低阈值以捕获白纸边缘
    cv::Canny(thresh, edges, 10, 50);
    
    // 在右下角显示处理结果，帮助调试
    cv::Mat debugImg;
    cv::cvtColor(edges, debugImg, cv::COLOR_GRAY2BGR);
    cv::resize(debugImg, debugImg, cv::Size(frame.cols/4, frame.rows/4));
    frame(cv::Rect(frame.cols-debugImg.cols, frame.rows-debugImg.rows, 
                  debugImg.cols, debugImg.rows)) = debugImg;
    
    // 找轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edges, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    
    // 在所有轮廓上绘制颜色，帮助可视化
    for (int i = 0; i < contours.size(); i++) {
        cv::Scalar color(rand() & 255, rand() & 255, rand() & 255);
        cv::drawContours(frame, contours, i, color, 1);
    }
    
    // 显示找到的轮廓数量
    cv::putText(frame, QString("轮廓数量: %1").arg(contours.size()).toStdString(), 
               cv::Point(20, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 2);
    
    // 添加尺寸偏好，使算法偏向于选择白纸
    // 纸张常见尺寸范围
    float targetRatio = 1.0f; // A4纸约为1.414，但我们给宽泛一些的比例
    
    // 筛选候选轮廓
    std::vector<std::pair<int, float>> candidates; // <contour index, score>
    for (int i = 0; i < contours.size(); i++) {
        double area = cv::contourArea(contours[i]);
        double frameArea = frame.cols * frame.rows;
        
        // 面积需在合理范围内 - 优先选择适合白纸的面积
        if (area < frameArea * 0.03 || area > frameArea * 0.5)
            continue;
            
        // 近似多边形
        std::vector<cv::Point> approx;
        double epsilon = 0.02 * cv::arcLength(contours[i], true);
        cv::approxPolyDP(contours[i], approx, epsilon, true);
        
        // 优先选择四边形
        if (approx.size() == 4 && cv::isContourConvex(approx)) {
            // 计算宽高比
            cv::RotatedRect rect = cv::minAreaRect(approx);
            float ratio = rect.size.width / rect.size.height;
            if (ratio < 1.0f) ratio = 1.0f / ratio; // 始终使用大值/小值
            
            // 打分系统：接近1.4比例的得分高
            float aspectScore = 1.0f - std::min(std::abs(ratio - 1.4f) / 1.4f, 0.8f);
            
            // 中心位置得分 - 更偏向于图像中央的轮廓
            cv::Rect boundRect = cv::boundingRect(approx);
            cv::Point center(boundRect.x + boundRect.width/2, boundRect.y + boundRect.height/2);
            float centerDist = std::sqrt(
                std::pow(center.x - frame.cols/2, 2) + 
                std::pow(center.y - frame.rows/2, 2)
            );
            float centerScore = 1.0f - std::min(centerDist / (frame.cols/2), 1.0f);
            
            // 总分 (高度重视中心位置)
            float score = aspectScore * 0.3f + centerScore * 0.7f;
            
            candidates.push_back(std::make_pair(i, score));
            
            // 在图中显示得分
            cv::putText(frame, 
                       QString("%.2f").arg(score).toStdString(), 
                       cv::Point(boundRect.x, boundRect.y - 5), 
                       cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
        }
    }
    
    // 如果找到候选，选择得分最高的
    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end(), 
                 [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                     return a.second > b.second; // 按得分降序排序
                 });
                 
        int bestIdx = candidates[0].first;
        
        // 近似到四边形
        std::vector<cv::Point> approx;
        double epsilon = 0.02 * cv::arcLength(contours[bestIdx], true);
        cv::approxPolyDP(contours[bestIdx], approx, epsilon, true);
        
        // 返回最优轮廓
        contour = approx;
        
        // 高亮显示选中的轮廓
        cv::drawContours(frame, contours, bestIdx, cv::Scalar(0, 255, 0), 3);
        cv::putText(frame, "选定桌面", cv::Point(20, 90), 
                   cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        
        return true;
    }
    
    return false;
}

// 计算四边形的宽高比
float PDFViewerPage::computeAspectRatio(const std::vector<cv::Point>& quad)
{
    if (quad.size() != 4) return 1.0f; // 默认正方形
    
    // 确保点是按顺序排列的
    std::vector<cv::Point2f> orderedPoints = orderPoints(quad);
    
    // 计算四边形的宽度和高度
    float width1 = cv::norm(orderedPoints[1] - orderedPoints[0]); // 上边
    float width2 = cv::norm(orderedPoints[2] - orderedPoints[3]); // 下边
    float height1 = cv::norm(orderedPoints[3] - orderedPoints[0]); // 左边
    float height2 = cv::norm(orderedPoints[2] - orderedPoints[1]); // 右边
    
    // 取平均值作为宽度和高度
    float avgWidth = (width1 + width2) * 0.5f;
    float avgHeight = (height1 + height2) * 0.5f;
    
    // 确保不会除以零，并返回宽高比
    if (avgHeight < 0.0001f) return 100.0f; // 防止除以零
    
    return avgWidth / avgHeight;
}

void PDFViewerPage::trackDesktop(cv::Mat& currentFrame, std::vector<cv::Point>& contour)
{
    if (prevFeaturePoints.empty() || prevGray.empty() || contour.size() != 4) return;

    // ================== 1. 光流跟踪 ==================
    cv::Mat currentGray;
    cv::cvtColor(currentFrame, currentGray, cv::COLOR_BGR2GRAY);

    // 高精度光流参数设置
    std::vector<cv::Point2f> currentFeaturePoints;
    std::vector<uchar> status;
    std::vector<float> err;
    cv::TermCriteria criteria = cv::TermCriteria(
        cv::TermCriteria::COUNT + cv::TermCriteria::EPS,
        30, 0.01);

    cv::calcOpticalFlowPyrLK(
        prevGray, currentGray, prevFeaturePoints, 
        currentFeaturePoints, status, err,
        cv::Size(21, 21), 3,
        criteria,
        cv::OPTFLOW_LK_GET_MIN_EIGENVALS
    );

    // 过滤掉低质量跟踪点
    std::vector<cv::Point2f> validPrev, validCurrent;
    for (size_t i = 0; i < status.size(); i++) {
        if (status[i] && err[i] < 10.0f) { // 设置误差阈值
            validPrev.push_back(prevFeaturePoints[i]);
            validCurrent.push_back(currentFeaturePoints[i]);
        }
    }

    // ================== 2. 运动估计 ==================
    std::vector<cv::Point2f> transformedContour;
    cv::Mat H;
    
    if (validPrev.size() > 10) {
        // 计算RANSAC单应性矩阵
        H = cv::findHomography(validPrev, validCurrent, cv::RANSAC, 2.0);
        
        if (!H.empty()) {
            // 变换当前轮廓
            std::vector<cv::Point2f> contourPoints;
            for (const auto& p : contour) {
                contourPoints.emplace_back(p.x, p.y);
            }
            
            cv::perspectiveTransform(contourPoints, transformedContour, H);
        }
    } else {
        transformedContour.clear();
        for (const auto& p : contour) {
            transformedContour.push_back(cv::Point2f(p.x, p.y));
        }
    }

    // ================== 新增: 结合陀螺仪数据 ==================
    if (gyroAvailable) {
        // 从光流估计的单应性矩阵提取相机位姿
        cv::Mat cameraMatrix = (cv::Mat_<double>(3,3) << 
                             viewfinder->width(), 0, viewfinder->width()/2,
                             0, viewfinder->height(), viewfinder->height()/2,
                             0, 0, 1);
        std::vector<cv::Point3f> objectPoints = {
            cv::Point3f(-0.5, -0.5, 0),
            cv::Point3f(0.5, -0.5, 0),
            cv::Point3f(0.5, 0.5, 0),
            cv::Point3f(-0.5, 0.5, 0)
        };
        
        std::vector<cv::Point2f> orderedImagePoints = orderPoints(contour);
        
        cv::Mat rvec, tvec;
        // 如果有足够的点且光流估计成功，计算PnP
        if (!H.empty() && !orderedImagePoints.empty()) {
            cv::solvePnP(objectPoints, orderedImagePoints, cameraMatrix, 
                        cv::Mat(), rvec, tvec);
                        
            // 将相机位姿转换为QMatrix4x4
            cv::Mat R;
            cv::Rodrigues(rvec, R);
            
            QMatrix4x4 cameraPose;
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    cameraPose(i, j) = R.at<double>(i, j);
                }
                cameraPose(i, 3) = tvec.at<double>(i, 0);
            }
            
            // 融合陀螺仪和相机数据
            QMatrix4x4 fusedPose = fuseCameraAndGyroData(cameraPose);
            
            // 将融合后的位姿转换回OpenCV格式
            cv::Mat fusedR(3, 3, CV_64F);
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    fusedR.at<double>(i, j) = fusedPose(i, j);
                }
                tvec.at<double>(i, 0) = fusedPose(i, 3);
            }
            
            // 转换回旋转向量
            cv::Rodrigues(fusedR, rvec);
            
            // 重新计算投影点
            std::vector<cv::Point2f> projectedPoints;
            cv::projectPoints(objectPoints, rvec, tvec, cameraMatrix, cv::Mat(), projectedPoints);
            
            // 使用融合的投影作为最终轮廓
            transformedContour = projectedPoints;
            
            // 调整视觉-陀螺仪权重
            // 当视觉跟踪质量好时，增加视觉权重；反之增加陀螺仪权重
            float trackingQuality = static_cast<float>(validCurrent.size()) / prevFeaturePoints.size();
            gyroVisualWeight = 0.3f + 0.5f * trackingQuality;  // 范围 0.3-0.8
        }
    }

    // ================== 3. 卡尔曼滤波 ==================
    if (!kalmanInitialized) { // 第一次初始化
        kf.statePre = cv::Mat::zeros(8, 1, CV_32F);
        for (int i = 0; i < 4; i++) {
            kf.statePre.at<float>(2*i) = transformedContour[i].x;
            kf.statePre.at<float>(2*i+1) = transformedContour[i].y;
        }
        kalmanInitialized = true;
    } else {
        // 预测阶段
        cv::Mat prediction = kf.predict();

        // 构建观测向量
        cv::Mat_<float> measurement(8, 1);
        for (int i = 0; i < 4; i++) {
            measurement(2*i) = transformedContour[i].x;
            measurement(2*i+1) = transformedContour[i].y;
        }

        // 校正阶段
        kf.correct(measurement);
    }

    // ================== 4. 形状约束处理 ==================
    std::vector<cv::Point2f> kalmanPoints(4);
    cv::Mat estimated = kf.statePost;
    for (int i = 0; i < 4; i++) {
        kalmanPoints[i].x = estimated.at<float>(2*i);
        kalmanPoints[i].y = estimated.at<float>(2*i+1);
    }

    // 强制保持宽高比
    std::vector<cv::Point2f> stabilizedPoints = enforceShapeConstraints(kalmanPoints);

    // ================== 5. 更新最终轮廓 ==================
    contour.clear();
    for (const auto& p : stabilizedPoints) {
        contour.emplace_back(p.x, p.y);
    }

    // ================== 6. 跟踪状态维护 ==================
    const int minTrackingPoints = 15;
    if (validCurrent.size() < minTrackingPoints && !gyroAvailable) { 
        // 跟踪点不足且无陀螺仪时自动重置
        resetDesktopDetection();
        statusLabel->setText("跟踪丢失，请重新定位桌面");
    } else if (validCurrent.size() < minTrackingPoints && gyroAvailable) {
        // 跟踪点不足但有陀螺仪时，增加陀螺仪权重
        gyroVisualWeight = 0.2f;
        statusLabel->setText("使用陀螺仪维持跟踪连续性");
    } else {
        // 更新历史数据
        prevGray = currentGray.clone();
        prevFeaturePoints = validCurrent;
    }
}

// 计算轮廓稳定性（基于点运动矢量）
float PDFViewerPage::calculateContourInstability(const std::vector<cv::Point>& contour)
{
    static std::vector<cv::Point> prevContour = contour;
    float totalMovement = 0.0f;
    
    for(size_t i=0; i<contour.size(); ++i){
        cv::Point2f delta = contour[i] - prevContour[i];
        totalMovement += cv::sqrt(delta.x*delta.x + delta.y*delta.y);
    }
    
    prevContour = contour;
    return totalMovement / contour.size();
}

// 基于惯性阻尼的运动平滑
void PDFViewerPage::applyMotionSmoothing(std::vector<cv::Point>& contour)
{
    const float dampingFactor = 0.85f;
    static std::vector<cv::Point> smoothed = contour;
    
    for(size_t i=0; i<contour.size(); ++i){
        cv::Point delta = contour[i] - smoothed[i];
        smoothed[i] += delta * dampingFactor;
    }
    
    contour = smoothed;
}

// 强形状约束辅助方法
std::vector<cv::Point2f> PDFViewerPage::enforceShapeConstraints(const std::vector<cv::Point2f>& points)
{
    // 1. 计算当前四边形的基本属性
    const cv::Point2f& tl = points[0];
    const cv::Point2f& tr = points[1];
    const cv::Point2f& br = points[2];
    const cv::Point2f& bl = points[3];
    
    cv::Point2f center = (tl + tr + br + bl) * 0.25f;
    
    // 2. 计算当前方向向量
    cv::Vec2f xAxis = tr - tl;
    cv::Vec2f yAxis = bl - tl;
    float xLength = cv::norm(xAxis);
    float yLength = cv::norm(yAxis);
    
    // 3. 保持原始宽高比
    float currentRatio = xLength / yLength;
    float ratioError = fabs(currentRatio - initialAspectRatio) / initialAspectRatio;
    
    cv::Vec2f newXAxis = xAxis;
    cv::Vec2f newYAxis = yAxis;
    
    if (ratioError > 0.15f) { // 宽高比误差超过15%时修正
        float targetX = sqrt(initialAspectRatio * xLength * yLength);
        float targetY = targetX / initialAspectRatio;
        
        newXAxis *= targetX / xLength;
        newYAxis *= targetY / yLength;
    }

    // 4. 正交约束
    float cosAngle = newXAxis.dot(newYAxis) / (cv::norm(newXAxis)*cv::norm(newYAxis));
    if (fabs(cosAngle) > 0.1f) { // 如果夹角偏离正交超过5°
        // 计算垂直修正
        cv::Vec2f orthoY(-newXAxis[1], newXAxis[0]);
        orthoY *= cv::norm(newYAxis) / cv::norm(orthoY);
        newYAxis = orthoY;
    }

    // 5. 重构四边形坐标
    const cv::Point2f scaledX = cv::Point2f(0.5f * newXAxis);
    const cv::Point2f scaledY = cv::Point2f(0.5f * newYAxis);
    
    std::vector<cv::Point2f> stabilized(4);
    stabilized[0] = center - scaledX - scaledY;  // 左上
    stabilized[1] = center + scaledX - scaledY;  // 右上
    stabilized[2] = center + scaledX + scaledY;  // 右下
    stabilized[3] = center - scaledX + scaledY;  // 左下


    return stabilized;
}


void PDFViewerPage::renderCurrentPDFToImage(const QSize& targetSize)
{
    if (pdfDocument->pageCount() == 0) return;

    try {
        // 创建缓存键
        QString cacheKey = QString("pdf_page_%1").arg(currentPage);
        
        // 强制刷新页面时，清除此页面的缓存
        if (currentPdfFrame.isNull() && ResourceManager::instance().hasImage(cacheKey)) {
            ResourceManager::instance().clearCacheEntry(cacheKey);
        }
        
        // 检查缓存
        if (ResourceManager::instance().hasImage(cacheKey)) {
            currentPdfFrame = ResourceManager::instance().getImage(cacheKey);
            
            // 更新状态
            statusLabel->setText(QString("使用缓存页面: %1/%2")
                              .arg(currentPage + 1)
                              .arg(pdfDocument->pageCount()));
                              
            qDebug() << "从缓存加载页面" << (currentPage + 1);
            return;
        }
        
        // 使用固定大小而非动态大小
        QSize renderSize(800, 1131); // A4比例，固定分辨率
        
        // 最基本的渲染选项
        QPdfDocumentRenderOptions options;
        
        // 同步渲染PDF
        QImage renderedImage = pdfDocument->render(currentPage, renderSize, options);
        
        if (renderedImage.isNull()) {
            statusLabel->setText("PDF渲染失败");
            return;
        }
        
        // 强制使用深拷贝，避免内存问题
        currentPdfFrame = renderedImage.copy().convertToFormat(QImage::Format_RGB888);
        
        // 缓存结果
        ResourceManager::instance().cacheImage(cacheKey, currentPdfFrame);
        
        // 更新状态
        statusLabel->setText(QString("已渲染页面: %1/%2")
                          .arg(currentPage + 1)
                          .arg(pdfDocument->pageCount()));
        
        qDebug() << "页面" << (currentPage + 1) << "渲染完成，尺寸:" 
                << currentPdfFrame.width() << "x" << currentPdfFrame.height();
    }
    catch (const std::exception& e) {
        statusLabel->setText(QString("PDF渲染异常: %1").arg(e.what()));
    }
    catch (...) {
        statusLabel->setText("PDF渲染未知异常");
    }
}


// 预加载相邻页面
void PDFViewerPage::preloadAdjacentPages()
{
    // 在后台线程中预加载相邻页面
    QtConcurrent::run([this]() {
        QMutexLocker locker(&pdfCacheMutex);
        
        // 预加载前后各一页
        for (int offset = -1; offset <= 1; offset += 2) {
            int pageToLoad = currentPage + offset;
            
            // 检查页码是否有效
            if (pageToLoad >= 0 && pageToLoad < pdfDocument->pageCount() && !pdfPageCache.contains(pageToLoad)) {
                // 如果缓存已满，先检查是否有空间
                if (pdfPageCache.size() >= maxCacheSize) {
                    // 查找距离当前页最远的页面
                    int furthestPage = -1;
                    int maxDistance = -1;
                    
                    for (auto it = pdfPageCache.begin(); it != pdfPageCache.end(); ++it) {
                        int distance = std::abs(it.key() - currentPage);
                        if (distance > maxDistance && it.key() != currentPage) {
                            maxDistance = distance;
                            furthestPage = it.key();
                        }
                    }
                    
                    // 移除最远的页面
                    if (furthestPage >= 0) {
                        pdfPageCache.remove(furthestPage);
                    } else {
                        // 无法腾出空间，跳过此次预加载
                        continue;
                    }
                }
                
                // 渲染页面
                QSize renderSize(800, 1131);
                QPdfDocumentRenderOptions options;
                QImage renderedImage = pdfDocument->render(pageToLoad, renderSize, options);
                
                if (!renderedImage.isNull()) {
                    // 添加到缓存
                    pdfPageCache[pageToLoad] = renderedImage.copy().convertToFormat(QImage::Format_RGB888);
                }
            }
        }
    });
}

// 环境光照分析和调整
void PDFViewerPage::analyzeEnvironmentLighting(const cv::Mat& frame)
{
    try {
        // 检查输入帧是否有效
        if (frame.empty() || frame.rows <= 0 || frame.cols <= 0) {
            qWarning() << "PDFViewerPage: analyzeEnvironmentLighting接收到空帧";
            return;
        }
        
        // 创建输入帧的副本以避免修改原始数据
        cv::Mat frameCopy = frame.clone();
        
        cv::Mat hsvFrame;
        cv::cvtColor(frameCopy, hsvFrame, cv::COLOR_BGR2HSV);
        
        // Extract brightness channel
        std::vector<cv::Mat> hsvChannels;
        cv::split(hsvFrame, hsvChannels);
        
        // 确保分割成功
        if (hsvChannels.size() < 3) {
            qWarning() << "PDFViewerPage: HSV通道分割失败";
            return;
        }
        
        cv::Mat valueChannel = hsvChannels[2];
        
        // Compute average brightness
        cv::Scalar meanValue = cv::mean(valueChannel);
        double avgBrightness = meanValue[0] / 255.0;
        
        // Log brightness for debugging
        qDebug() << "Average Brightness:" << avgBrightness;
        
        // 使用Qt的线程安全方法更新UI
        double finalBrightness = avgBrightness;
        QMetaObject::invokeMethod(this, [this, finalBrightness]() {
            // 只有当组件可见时才处理亮度调整
            if (!isVisible()) return;
            
            // Adjust opacity based on average brightness with hysteresis to prevent frequent changes
            static double lastBrightness = -1;
            static double adjustedOpacity = pdfOpacity;
            
            if (finalBrightness < 0.3 && lastBrightness >= 0.3) { // Dark environment
                adjustedOpacity = qMin(pdfOpacity * 1.2, 0.9);
                if (opacitySlider) {
                    opacitySlider->setValue(adjustedOpacity * 100);
                }
                if (statusLabel) {
                    statusLabel->setText("低光环境检测到，自动增加PDF不透明度");
                }
            }
            else if (finalBrightness > 0.7 && lastBrightness <= 0.7) { // Bright environment
                adjustedOpacity = qMax(pdfOpacity * 0.8, 0.5);
                if (opacitySlider) {
                    opacitySlider->setValue(adjustedOpacity * 100);
                }
                if (statusLabel) {
                    statusLabel->setText("高亮环境检测到，自动降低PDF不透明度");
                }
            }
            
            // Detect sudden brightness changes
            if (lastBrightness > 0 && std::abs(finalBrightness - lastBrightness) > 0.3) {
                resetDesktopDetection();
                if (statusLabel) {
                    statusLabel->setText("光照变化检测到，已重置跟踪");
                }
            }
            
            lastBrightness = finalBrightness;
            pdfOpacity = adjustedOpacity;
        }, Qt::QueuedConnection);
    } catch (const std::exception& e) {
        qWarning() << "PDFViewerPage: analyzeEnvironmentLighting异常:" << e.what();
    } catch (...) {
        qWarning() << "PDFViewerPage: analyzeEnvironmentLighting未知异常";
    }
}



void PDFViewerPage::overlayPDF(cv::Mat& frame, const std::vector<cv::Point>& contour)
{
    if (contour.size() != 4 || currentPdfFrame.isNull()) return;

    try {
        // PDF源点
        std::vector<cv::Point2f> srcPoints = {
            cv::Point2f(0, 0),
            cv::Point2f(currentPdfFrame.width(), 0),
            cv::Point2f(currentPdfFrame.width(), currentPdfFrame.height()),
            cv::Point2f(0, currentPdfFrame.height())
        };

        // 使用更精确的轮廓排序算法
        std::vector<cv::Point2f> dstPoints = orderPoints(contour);
        
        // 计算透视变换矩阵
        cv::Mat homography = cv::findHomography(srcPoints, dstPoints);
        if (homography.empty()) return;

        // 创建PDF矩阵，确保格式和通道数正确
        QImage tempImage = currentPdfFrame.convertToFormat(QImage::Format_RGB888);
        cv::Mat pdfMat(tempImage.height(), tempImage.width(), CV_8UC3,
                     const_cast<uchar*>(tempImage.bits()));
        cv::cvtColor(pdfMat, pdfMat, cv::COLOR_RGB2BGR);
        
        // 使用更高质量的透视变换设置
        cv::Mat warped = cv::Mat::zeros(frame.size(), CV_8UC3);
        cv::warpPerspective(pdfMat, warped, homography, frame.size(), 
                           cv::INTER_LINEAR);
        
        // 创建蒙版以便确定PDF的可见区域
        cv::Mat mask = cv::Mat::zeros(frame.size(), CV_8UC1); // 确保是单通道
        std::vector<std::vector<cv::Point>> contours = {std::vector<cv::Point>(dstPoints.begin(), dstPoints.end())};
        cv::fillPoly(mask, contours, cv::Scalar(255));
        
        // 使用更大的模糊半径实现边缘平滑
        cv::GaussianBlur(mask, mask, cv::Size(9, 9), 3.0);
        
        // 将PDF内容与原始图像混合
        double alpha = pdfOpacity;
        cv::Mat tempFrame = frame.clone(); // 创建副本避免修改原始frame
        cv::Mat blend;
        cv::addWeighted(tempFrame, 1.0 - alpha, warped, alpha, 0, blend);
        
        // 只在蒙版区域内应用混合结果 - 使用正确的copyTo函数
        blend.copyTo(frame, mask); // mask必须是单通道
        
        // 绘制边界以增强视觉效果
        cv::polylines(frame, contours, true, cv::Scalar(0, 255, 255), 2);
    } catch (const std::exception& e) {
        // 记录错误但不中断处理
        qWarning() << "PDFViewerPage: overlayPDF中发生异常:" << e.what();
    }
}

void PDFViewerPage::stopCamera()
{
    qDebug() << "PDFViewerPage: 安全停止摄像头 (开始)";
    
    // 先断开信号以避免清理期间的回调
    if (videoSink) {
        disconnect(videoSink, &QVideoSink::videoFrameChanged, this, &PDFViewerPage::processFrame);
    }
    
    // 安全停止摄像头
    if (camera) {
        if (camera->isActive()) {
            try {
                camera->stop();
                qDebug() << "PDFViewerPage: 摄像头已停止";
                QThread::msleep(500); // 给予足够时间完全停止
            } catch (...) {
                qWarning() << "PDFViewerPage: 停止摄像头时发生异常";
            }
        }
        
        // 确保删除摄像头对象，避免过早重用
        delete camera;
        camera = nullptr;
    }
    
    // 通过中央管理器释放资源
    auto& cameraManager = CameraResourceManager::instance();
    bool released = cameraManager.releaseCamera("PDFViewer");
    
    if (released) {
        qDebug() << "PDFViewerPage: 通过资源管理器成功释放摄像头";
    } else {
        qWarning() << "PDFViewerPage: 资源管理器释放摄像头失败";
        
        // 尝试更激进的方式释放
        cameraManager.resetAllCameras(); // 重置所有摄像头
    }
    
    // 更新UI状态
    startCameraButton->setEnabled(true);
    stopCameraButton->setEnabled(false);
    statusLabel->setText("摄像头已停止");
    
    // 清空显示
    processedLabel->clear();
    
    // 重置跟踪状态
    desktopLocked = false;
    desktopDetected = false;
    
    qDebug() << "PDFViewerPage: 停止摄像头 (完成)";
}

void PDFViewerPage::onBackButtonClicked()
{
    stopCamera();
    emit backButtonClicked();
}


// 使用OpenGL渲染PDF到检测到的桌面
void PDFViewerPage::renderPDFWithOpenGL(const QImage& pdfImage, const std::vector<cv::Point>& contour)
{
    if (contour.size() != 4 || pdfImage.isNull())
        return;
        
    // 初始化OpenGL上下文和着色器
    initializeOpenGLFunctions();
    
    // 将PDF图像转为OpenGL纹理
    GLuint textureId = createTextureFromImage(pdfImage);
    
    // 设置投影矩阵
    QMatrix4x4 projMatrix;
    projMatrix.perspective(45.0f, (float)width() / height(), 0.1f, 100.0f);
    
    // 计算基于检测到轮廓的模型视图矩阵
    QMatrix4x4 modelViewMatrix;
    calculateModelViewFromContour(contour, modelViewMatrix);
    
    // 绑定着色器程序
    shaderProgram.bind();
    
    // 设置着色器统一变量
    shaderProgram.setUniformValue("projectionMatrix", projMatrix);
    shaderProgram.setUniformValue("modelViewMatrix", modelViewMatrix);
    shaderProgram.setUniformValue("textureSampler", 0);
    shaderProgram.setUniformValue("opacity", float(pdfOpacity));
    
    // 绑定纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    
    // 设置顶点数据
    setupVertexData();
    
    // 绘制PDF平面
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // 清理
    shaderProgram.release();
    glDeleteTextures(1, &textureId);
}

// 初始化OpenGL
void PDFViewerPage::initializeGL()
{
    // 初始化OpenGL函数
    initializeOpenGLFunctions();
    
    // 设置OpenGL状态
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // 透明背景
    glEnable(GL_DEPTH_TEST);  // 启用深度测试
    glDepthFunc(GL_LEQUAL);   // 设置深度测试函数
    glEnable(GL_BLEND);       // 启用混合
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);  // 设置混合函数
    
    // 设置OpenGL视口
    glViewport(0, 0, width(), height());
    
    // 创建和绑定VAO
    vao.create();
    vao.bind();
    
    // 创建和绑定VBO
    vbo.create();
    vbo.bind();
    
    // 定义PDF平面的顶点数据
    float vertices[] = {
        // 位置(x,y,z)        // 纹理坐标(u,v)
        -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,  // 左上
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f,  // 右上
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,  // 左下
         1.0f, -1.0f, 0.0f,   1.0f, 1.0f   // 右下
    };
    
    // 分配VBO数据
    vbo.allocate(vertices, sizeof(vertices));
    
    // 设置顶点属性指针
    // 位置属性 (location = 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // 纹理坐标属性 (location = 1)
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // 解绑VBO和VAO
    vbo.release();
    vao.release();
    
    // 初始化着色器程序
    setupShaders();
    
    // 设置投影矩阵
    QMatrix4x4 projection;
    projection.perspective(45.0f, (float)width() / height(), 0.1f, 100.0f);
    
    // 绑定着色器程序
    shaderProgram.bind();
    
    // 设置着色器统一变量
    shaderProgram.setUniformValue("projectionMatrix", projection);
    
    // 解绑着色器程序
    shaderProgram.release();
    
    // 打印OpenGL版本信息（调试用）
    qDebug() << "OpenGL Version:" << reinterpret_cast<const char*>(glGetString(GL_VERSION));
    qDebug() << "OpenGL Renderer:" << reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    qDebug() << "OpenGL Vendor:" << reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    qDebug() << "GLSL Version:" << reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
}

// 设置着色器程序
void PDFViewerPage::setupShaders()
{
       // 顶点着色器
       const char* vertexShaderSource = R"(
        #version 330 core
        layout (location = 0) in vec3 position;
        layout (location = 1) in vec2 texCoord;
        
        uniform mat4 projectionMatrix;
        uniform mat4 modelViewMatrix;
        
        out vec2 fragTexCoord;
        
        void main()
        {
            gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
            fragTexCoord = texCoord;
        }
    )";
    
    // 片段着色器
    const char* fragmentShaderSource = R"(
        #version 330 core
        in vec2 fragTexCoord;
        
        uniform sampler2D textureSampler;
        uniform float opacity;
        
        out vec4 fragColor;
        
        void main()
        {
            vec4 texColor = texture(textureSampler, fragTexCoord);
            fragColor = vec4(texColor.rgb, texColor.a * opacity);
        }
    )";
    
    // 编译着色器程序
    if (!shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource) ||
        !shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource) ||
        !shaderProgram.link()) {
        qWarning() << "Error compiling shaders:" << shaderProgram.log();
        return;
    }
}
float PDFViewerPage::calculateRotationAngle(const std::vector<cv::Point2f>& points)
{
    // 计算桌面平面的旋转角度（相对于屏幕平面）
    if (points.size() < 4) return 0.0f;
    
    // 计算上边缘的向量
    cv::Point2f topEdge = points[1] - points[0];
    
    // 计算与水平方向的夹角
    float angle = atan2(topEdge.y, topEdge.x) * 180.0f / M_PI;
    
    return -angle; // 返回负角度以正确旋转
}

// 设置顶点数据
void PDFViewerPage::setupVertexData()
{
    // PDF平面的顶点数据（位置和纹理坐标）
    float vertices[] = {
        // 位置(x,y,z)        // 纹理坐标(u,v)
        -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,  // 左上
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f,  // 右上
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,  // 左下
         1.0f, -1.0f, 0.0f,   1.0f, 1.0f   // 右下
    };
    
    // 使用Qt的函数而不是原生OpenGL函数
    vao.bind();
    
    // 绑定并分配VBO数据
    vbo.bind();
    vbo.allocate(vertices, sizeof(vertices));
    
    // 设置顶点属性 - 注意这里仍然使用OpenGL函数，但它们是由QOpenGLFunctions提供的
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    
    // 设置纹理坐标属性
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // 解绑VBO和VAO
    vbo.release();
    vao.release();
}


// 从QImage创建OpenGL纹理
GLuint PDFViewerPage::createTextureFromImage(const QImage& image)
{
    QImage textureImage = image.convertToFormat(QImage::Format_RGBA8888);
    
    GLuint textureId;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureImage.width(), textureImage.height(),
                0, GL_RGBA, GL_UNSIGNED_BYTE, textureImage.constBits());
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    return textureId;
}

// 从检测到的轮廓计算模型视图矩阵
void PDFViewerPage::calculateModelViewFromContour(const std::vector<cv::Point>& contour, QMatrix4x4& matrix)
{
    // 计算四个角点的3D位置
    std::vector<cv::Point2f> orderedPoints = orderPoints(contour);
    
    // 计算轮廓的中心点
    cv::Point2f center(0, 0);
    for (const auto& pt : orderedPoints) {
        center += pt;
    }
    center *= 0.25f;
    
    // 计算桌面平面的法向量和基础矩阵
    cv::Point2f topEdge = orderedPoints[1] - orderedPoints[0];
    cv::Point2f rightEdge = orderedPoints[2] - orderedPoints[1];
    
    float width = cv::norm(topEdge);
    float height = cv::norm(rightEdge);
    
    // 设置模型视图矩阵（从摄像机观察桌面）
    matrix.setToIdentity();
    matrix.translate(center.x, center.y, -5.0f);
    matrix.rotate(calculateRotationAngle(orderedPoints), 0.0f, 0.0f, 1.0f);
    matrix.scale(width / 2.0f, height / 2.0f, 1.0f);
}

// 处理帧数据
void PDFViewerPage::processFrame(const QVideoFrame &frame)
{
    try {
        // 基本检查
        if (!isVisible() || !camera || !camera->isActive()) {
            return;
        }
        
        // 帧率控制 - 限制处理频率以避免过度消耗CPU
        static QElapsedTimer fpsTimer;
        if (!fpsTimer.hasExpired(33)) { // 约30 FPS - 从原来的100ms(10FPS)提高到33ms(30FPS)
            return;
        }
        fpsTimer.restart();
        
        // 监控内存使用
        monitorMemoryUsage();

        // 使用线程池或直接处理
        if (m_useThreadPool) {
            processFrameInThreadPool(frame);
        } else {
            // 原始处理逻辑保持不变，作为后备方案
            QVideoFrame clonedFrame = frame;
            if (!clonedFrame.map(QVideoFrame::ReadOnly)) {
                return;
            }
            
            QImage image = clonedFrame.toImage().convertToFormat(QImage::Format_RGB888);
            clonedFrame.unmap();
            
            if (image.isNull()) {
                return;
            }
            
            // 转换为OpenCV，并降低分辨率
            cv::Mat cvFrame(image.height(), image.width(), CV_8UC3, 
                          const_cast<uchar*>(image.constBits()), image.bytesPerLine());
            cv::Mat cvFrameCopy = cvFrame.clone();
            cv::cvtColor(cvFrameCopy, cvFrameCopy, cv::COLOR_RGB2BGR);
            
            // 降低分辨率以提高性能
            cv::Mat resizedFrame;
            cv::resize(cvFrameCopy, resizedFrame, cv::Size(), 0.5, 0.5, cv::INTER_LINEAR);
            
            // 将帧发送到处理线程
            m_arucoProcessor->processFrame(resizedFrame);
        }
    } catch (const std::exception& e) {
        qWarning() << "PDFViewerPage: processFrame中发生异常:" << e.what();
    } catch (...) {
        qWarning() << "PDFViewerPage: processFrame中发生未知异常";
    }
}





void PDFViewerPage::updateDeskPosition(const std::vector<cv::Point>& contour)
{
    if (contour.size() != 4 || !deskInitialized) return;
    
    try {
        // 获取有序的当前桌面角点
        std::vector<cv::Point2f> currentPoints = orderPoints(contour);
        
        // 估计相机内参（理想情况下应该通过标定获取）
        cv::Mat cameraMatrix = (cv::Mat_<double>(3,3) << 
                             viewfinder->width(), 0, viewfinder->width()/2,
                             0, viewfinder->height(), viewfinder->height()/2,
                             0, 0, 1);
        cv::Mat distCoeffs = cv::Mat::zeros(4, 1, CV_64F); // 假设无畸变
        
        // 使用PnP算法估计当前相机相对于桌面的姿态
        cv::Mat rvec, tvec;
        std::vector<cv::Point2f> imagePoints(currentPoints.begin(), currentPoints.end());
        
        // 使用前一帧的位姿作为初始估计，提高稳定性
        bool success = cv::solvePnP(deskCorners3D, imagePoints, cameraMatrix, distCoeffs, 
                                   rvec, tvec, true);
        
        if (!success) {
            qWarning() << "PDFViewerPage: 更新桌面位置失败 - PnP求解失败";
            return;
        }
        
        // 将旋转向量转换为旋转矩阵
        cv::Mat R;
        cv::Rodrigues(rvec, R);
        
        // 计算变换矩阵T (从世界坐标系到相机坐标系)
        cv::Mat T = cv::Mat::eye(4, 4, CV_64F);
        R.copyTo(T(cv::Rect(0, 0, 3, 3)));
        tvec.copyTo(T(cv::Rect(3, 0, 1, 3)));
        
        // 更新世界到相机的变换矩阵
        worldToCamera = QMatrix4x4(
            T.at<double>(0,0), T.at<double>(0,1), T.at<double>(0,2), T.at<double>(0,3),
            T.at<double>(1,0), T.at<double>(1,1), T.at<double>(1,2), T.at<double>(1,3),
            T.at<double>(2,0), T.at<double>(2,1), T.at<double>(2,2), T.at<double>(2,3),
            T.at<double>(3,0), T.at<double>(3,1), T.at<double>(3,2), T.at<double>(3,3)
        );
        
        // 更新桌面边界矩形
        lastDeskBoundingRect = cv::boundingRect(contour);
        
        // 应用卡尔曼滤波进行平滑（避免抖动）
        if (kalmanInitialized) {
            // 创建测量向量 (包含位置和旋转)
            cv::Mat_<float> measurement(12, 1);
            
            // 位置 (平移向量的三个分量)
            measurement(0) = tvec.at<double>(0);
            measurement(1) = tvec.at<double>(1);
            measurement(2) = tvec.at<double>(2);
            
            // 旋转 (旋转矩阵的9个元素)
            int idx = 3;
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    measurement(idx++) = R.at<double>(i, j);
                }
            }
            
            // 卡尔曼滤波器更新
            cv::Mat prediction = kf.predict();
            cv::Mat corrected = kf.correct(measurement);
            
            // 从卡尔曼滤波结果中提取平滑后的位置和旋转
            tvec.at<double>(0) = corrected.at<float>(0);
            tvec.at<double>(1) = corrected.at<float>(1);
            tvec.at<double>(2) = corrected.at<float>(2);
            
            idx = 3;
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    R.at<double>(i, j) = corrected.at<float>(idx++);
                }
            }
            
            // 正交化旋转矩阵确保其有效性
            cv::Mat w, u, vt;
            cv::SVD::compute(R, w, u, vt);
            R = u * vt;
            
            // 更新变换矩阵
            R.copyTo(T(cv::Rect(0, 0, 3, 3)));
            tvec.copyTo(T(cv::Rect(3, 0, 1, 3)));
            
            // 更新世界到相机的变换矩阵
            worldToCamera = QMatrix4x4(
                T.at<double>(0,0), T.at<double>(0,1), T.at<double>(0,2), T.at<double>(0,3),
                T.at<double>(1,0), T.at<double>(1,1), T.at<double>(1,2), T.at<double>(1,3),
                T.at<double>(2,0), T.at<double>(2,1), T.at<double>(2,2), T.at<double>(2,3),
                T.at<double>(3,0), T.at<double>(3,1), T.at<double>(3,2), T.at<double>(3,3)
            );
        }
        
        // 使用最新的变换矩阵更新PDF位置
        // 此调用应该确保PDF被渲染在桌面的正确位置上
        // enhancedOverlayPDF函数将使用这个更新后的矩阵
        
        // 输出一些调试信息
        if (statusLabel) {
            // 计算距离桌面的距离用于调试
            double distance = cv::norm(tvec);
            QMetaObject::invokeMethod(this, [this, distance]() {
                statusLabel->setText(QString("桌面跟踪中 - 距离: %.2f cm").arg(distance * 0.1));
            }, Qt::QueuedConnection);
        }
        
    } catch (const std::exception& e) {
        qWarning() << "PDFViewerPage: updateDeskPosition异常:" << e.what();
    } catch (...) {
        qWarning() << "PDFViewerPage: updateDeskPosition未知异常";
    }
}

bool PDFViewerPage::isDeskStillVisible( cv::Mat& frame)
{
    // 如果桌面未初始化，则认为不可见
    if (!deskInitialized) return false;
    
    // 方法1：检查锚定的桌面区域是否仍在画面中
    cv::Rect frameRect(0, 0, frame.cols, frame.rows);
    if ((lastDeskBoundingRect & frameRect).area() < lastDeskBoundingRect.area() * 0.3) {
        // 如果桌面边界矩形与画面重叠面积小于30%，认为桌面不可见
        return false;
    }
    
    // 方法2：尝试使用跟踪算法检测桌面
    std::vector<cv::Point> currentContour;
    bool detected = detectDesktop(frame, currentContour);
    
    if (detected) {
        // 计算新检测到的桌面与上次桌面的IOU
        cv::Rect newRect = cv::boundingRect(currentContour);
        float intersection = (lastDeskBoundingRect & newRect).area();
        float unionArea = lastDeskBoundingRect.area() + newRect.area() - intersection;
        float iou = intersection / unionArea;
        
        // IOU大于0.5，认为是同一张桌面
        if (iou > 0.5) {
            lockedDesktopContour = currentContour;
            lastDeskBoundingRect = newRect;
            return true;
        }
    }
    
    // 使用光流跟踪检查桌面角点是否仍然可跟踪
    if (!prevGray.empty() && !prevFeaturePoints.empty()) {
        // 光流跟踪逻辑，如果大部分特征点仍能跟踪则返回true
        // (此部分依赖于现有的trackDesktop函数)
        return true;
    }
    
    return false;
}



void PDFViewerPage::initializeDesk3DPosition(const std::vector<cv::Point>& contour)
{
    // 估计桌面在3D空间中的位置
    // 假设桌面是水平放置的，我们使用其2D轮廓来估计其3D位置
    
    // 获取有序的桌面角点
    std::vector<cv::Point2f> orderedPoints = orderPoints(contour);
    
    // 估计桌面尺寸（假设它大约是A4纸的尺寸，297mm x 210mm）
    const float deskWidth = 297.0f;  // mm
    const float deskHeight = 210.0f; // mm
    
    // 创建桌面3D角点（以桌面中心为原点）
    deskCorners3D = {
        cv::Point3f(-deskWidth/2, -deskHeight/2, 0), // 左上
        cv::Point3f(deskWidth/2, -deskHeight/2, 0),  // 右上
        cv::Point3f(deskWidth/2, deskHeight/2, 0),   // 右下
        cv::Point3f(-deskWidth/2, deskHeight/2, 0)   // 左下
    };
    
    // 估计相机内参（这是简化的，理想情况下应该进行相机标定）
    cv::Mat cameraMatrix = (cv::Mat_<double>(3,3) << 
                          viewfinder->width(), 0, viewfinder->width()/2,
                          0, viewfinder->height(), viewfinder->height()/2,
                          0, 0, 1);
    
    // 使用PnP算法估计相机相对于桌面的位置
    cv::Mat rvec, tvec;
    std::vector<cv::Point2f> imagePoints(orderedPoints.begin(), orderedPoints.end());
    cv::solvePnP(deskCorners3D, imagePoints, cameraMatrix, cv::Mat(), rvec, tvec);
    
    // 保存初始变换矩阵
    cv::Mat R;
    cv::Rodrigues(rvec, R);
    cv::Mat P = cv::Mat::eye(4, 4, CV_64F);
    R.copyTo(P(cv::Rect(0, 0, 3, 3)));
    tvec.copyTo(P(cv::Rect(3, 0, 1, 3)));
    
    // 转换为QMatrix4x4
    worldToCamera = QMatrix4x4(
        P.at<double>(0,0), P.at<double>(0,1), P.at<double>(0,2), P.at<double>(0,3),
        P.at<double>(1,0), P.at<double>(1,1), P.at<double>(1,2), P.at<double>(1,3),
        P.at<double>(2,0), P.at<double>(2,1), P.at<double>(2,2), P.at<double>(2,3),
        P.at<double>(3,0), P.at<double>(3,1), P.at<double>(3,2), P.at<double>(3,3)
    );
    
    // 保存桌面边界矩形
    lastDeskBoundingRect = cv::boundingRect(contour);
}



// OpenGL绘制函数
void PDFViewerPage::paintGL()
{ glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    if (!desktopLocked || currentPdfFrame.isNull())
        return;
        
    // 绑定着色器
    shaderProgram.bind();
    
    // 创建模型视图矩阵
    QMatrix4x4 modelView;
    modelView.setToIdentity();
    
    // 如果已锁定桌面轮廓，计算透视变换
    if (!lockedDesktopContour.empty() && lockedDesktopContour.size() == 4) {
        calculateModelViewFromContour(lockedDesktopContour, modelView);
    }
    
    // 设置着色器中的矩阵
    shaderProgram.setUniformValue("modelViewMatrix", modelView);
    
    // 创建并绑定纹理 - 使用我们自己的createTextureFromImage函数
    GLuint textureId = createTextureFromImage(currentPdfFrame);
    shaderProgram.setUniformValue("textureSampler", 0); // 纹理单元0
    shaderProgram.setUniformValue("opacity", float(pdfOpacity));
    
    // 绑定VAO并绘制
    vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vao.release();
    
    // 清理 - 使用正确的OpenGL函数
    glDeleteTextures(1, &textureId);
    shaderProgram.release();
}


void PDFViewerPage::resizeGL(int w, int h)
{
    // 更新OpenGL视口
    glViewport(0, 0, w, h);
    
    // 更新投影矩阵以保持正确的纵横比
    QMatrix4x4 projection;
    projection.perspective(45.0f, static_cast<float>(w) / h, 0.1f, 100.0f);
    
    shaderProgram.bind();
    shaderProgram.setUniformValue("projectionMatrix", projection);
    shaderProgram.release();
}

void PDFViewerPage::saveRecording()
{
    if (recordingFrames.empty()) {
        statusLabel->setText("没有录制内容可保存");
        return;
    }
    
    // 选择保存位置
    QString savePath = QFileDialog::getSaveFileName(this, "保存录制", 
                                                 QDir::homePath() + "/recording.dat",
                                                 "录制文件 (*.dat)");
    if (savePath.isEmpty()) return;
    
    // 创建文件
    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "错误", "无法创建文件: " + file.errorString());
        return;
    }
    
    // 保存基本信息 - 使用显式类型转换避免歧义
    QDataStream stream(&file);
    stream << static_cast<qint32>(recordingFrames.size());  // 显式转换为 qint32
    
    // 保存每一帧和轮廓
    for (size_t i = 0; i < recordingFrames.size(); i++) {
        // 将Mat转换为QImage保存
        cv::Mat frame = recordingFrames[i];
        QImage image(frame.data, frame.cols, frame.rows, 
                   frame.step, QImage::Format_BGR888);
        stream << image;
        
        // 保存轮廓点数 - 同样使用显式类型转换
        const auto& contour = recordingContours[i];
        stream << static_cast<qint32>(contour.size());  // 显式转换为 qint32
        
        // 保存轮廓的每个点
        for (const auto& point : contour) {
            stream << static_cast<qint32>(point.x) << static_cast<qint32>(point.y);  // 显式转换 x 和 y
        }
    }
    
    file.close();
    statusLabel->setText(QString("录制已保存: %1 帧").arg(recordingFrames.size()));
}

void PDFViewerPage::loadRecording()
{
    // 选择录制文件
    QString loadPath = QFileDialog::getOpenFileName(this, "加载录制", 
        QDir::homePath(),
        "录制文件 (*.dat)");
    if (loadPath.isEmpty()) return;

    // 打开文件
    QFile file(loadPath);
    if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, "错误", "无法打开文件: " + file.errorString());
    return;
    }

    // 清空当前录制
    recordingFrames.clear();
    recordingContours.clear();

    // 读取基本信息
    QDataStream stream(&file);
    qint32 frameCount;  // 使用 qint32 而不是 int
    stream >> frameCount;

    // 读取每一帧和轮廓
    for (int i = 0; i < frameCount; i++) {
    // 读取图像
    QImage image;
    stream >> image;
    image = image.convertToFormat(QImage::Format_BGR888);

    // 转换为Mat
    cv::Mat frame(image.height(), image.width(), CV_8UC3, 
    const_cast<uchar*>(image.bits()));
    recordingFrames.push_back(frame.clone());

    // 读取轮廓点数
    qint32 pointCount;  // 使用 qint32 而不是 int
    stream >> pointCount;

    // 读取轮廓的每个点
    std::vector<cv::Point> contour;
    for (int j = 0; j < pointCount; j++) {
    qint32 x, y;  // 使用 qint32 而不是 int
    stream >> x >> y;
    contour.push_back(cv::Point(x, y));
    }

    recordingContours.push_back(contour);
    }

    file.close();
    statusLabel->setText(QString("录制已加载: %1 帧").arg(recordingFrames.size()));
}

void PDFViewerPage::startPlayback()
{
    if (recordingFrames.empty()) {
        statusLabel->setText("没有录制内容可播放");
        return;
    }
    
    // 重置播放状态
    playbackFrame = 0;
    
    // 创建计时器
    if (!playbackTimer) {
        playbackTimer = new QTimer(this);
        connect(playbackTimer, &QTimer::timeout, this, [this]() {
            if (playbackFrame >= recordingFrames.size()) {
                playbackTimer->stop();
                statusLabel->setText("回放完成");
                return;
            }
            
            // 获取当前帧和轮廓
            cv::Mat frame = recordingFrames[playbackFrame].clone();
            std::vector<cv::Point> contour = recordingContours[playbackFrame];
            
            // 叠加PDF
            if (!currentPdfFrame.isNull() && !contour.empty()) {
                enhancedOverlayPDF(frame, contour);
            }
            
            // 显示帧
            QImage image(frame.data, frame.cols, frame.rows, 
                       frame.step, QImage::Format_BGR888);
            processedLabel->setPixmap(QPixmap::fromImage(image)
                                    .scaled(processedLabel->size(), Qt::KeepAspectRatio));
            
            // 更新状态
            statusLabel->setText(QString("回放中: %1/%2").arg(playbackFrame+1).arg(recordingFrames.size()));
            
            // 前进到下一帧
            playbackFrame++;
        });
    }
    
    // 启动计时器
    playbackTimer->start(33); // 约30帧/秒
}

void PDFViewerPage::trackDesktopWithFeatureSLAM(cv::Mat& frame, std::vector<cv::Point>& contour)
{
    // 此函数实现基于特征点的SLAM跟踪
    if (contour.size() != 4) return;
    
    try {
        // 转换为灰度图
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        
        // 如果是首次调用，初始化特征点
        static cv::Ptr<cv::FeatureDetector> detector = cv::ORB::create();
        static cv::Ptr<cv::DescriptorMatcher> matcher = cv::DescriptorMatcher::create("BruteForce-Hamming");
        static cv::Mat prevDescriptors;
        static std::vector<cv::KeyPoint> prevKeypoints;
        static cv::Mat prevImg;
        
        // 在当前帧中检测特征点
        std::vector<cv::KeyPoint> currKeypoints;
        cv::Mat currDescriptors;
        detector->detectAndCompute(gray, cv::noArray(), currKeypoints, currDescriptors);
        
        // 绘制当前特征点
        cv::drawKeypoints(frame, currKeypoints, frame, cv::Scalar(0, 255, 0));
        
        // 如果有前一帧数据，进行特征匹配
        if (!prevImg.empty() && !prevKeypoints.empty() && !prevDescriptors.empty()) {
            std::vector<cv::DMatch> matches;
            if (!currDescriptors.empty() && !prevDescriptors.empty()) {
                matcher->match(currDescriptors, prevDescriptors, matches);
                
                // 筛选好的匹配
                std::vector<cv::DMatch> goodMatches;
                float minDist = std::numeric_limits<float>::max();
                for (const auto& match : matches) {
                    minDist = std::min(minDist, match.distance);
                }
                
                for (const auto& match : matches) {
                    if (match.distance < std::max(2.0f * minDist, 30.0f)) {
                        goodMatches.push_back(match);
                    }
                }
                
                // 绘制匹配的特征点
                cv::Mat matchImg;
                cv::drawMatches(frame, currKeypoints, prevImg, prevKeypoints, 
                               goodMatches, matchImg);
                
                // 提取匹配点坐标
                std::vector<cv::Point2f> currPoints, prevPoints;
                for (const auto& match : goodMatches) {
                    currPoints.push_back(currKeypoints[match.queryIdx].pt);
                    prevPoints.push_back(prevKeypoints[match.trainIdx].pt);
                }
                
                // 如果有足够的匹配点，估计变换矩阵
                if (currPoints.size() >= 4) {
                    cv::Mat H = cv::findHomography(prevPoints, currPoints, cv::RANSAC);
                    if (!H.empty()) {
                        // 变换上一帧的桌面轮廓到当前帧
                        std::vector<cv::Point2f> prevContourPoints;
                        for (const auto& p : contour) {
                            prevContourPoints.push_back(cv::Point2f(p.x, p.y));
                        }
                        
                        std::vector<cv::Point2f> currContourPoints;
                        cv::perspectiveTransform(prevContourPoints, currContourPoints, H);
                        
                        // 更新桌面轮廓
                        contour.clear();
                        for (const auto& p : currContourPoints) {
                            contour.push_back(cv::Point(p.x, p.y));
                        }
                    }
                }
            }
        }
        
        // 保存当前帧信息用于下一次处理
        prevImg = gray.clone();
        prevKeypoints = currKeypoints;
        prevDescriptors = currDescriptors.clone();
        
    } catch (const std::exception& e) {
        qWarning() << "PDFViewerPage: 特征点SLAM跟踪中发生异常:" << e.what();
    } catch (...) {
        qWarning() << "PDFViewerPage: 特征点SLAM跟踪中发生未知异常";
    }
}

void PDFViewerPage::trackDesktopWithOpticalFlowSLAM(cv::Mat& frame, std::vector<cv::Point>& contour)
{
    // 此函数实现基于光流和惯性的SLAM跟踪
    if (contour.size() != 4) return;
    
    try {
        // 转换为灰度图
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        
        // 如果是首次调用，初始化
        static cv::Mat prevGray;
        static std::vector<cv::Point2f> prevCorners;
        static QVector3D prevAcceleration;
        static QQuaternion prevOrientation;
        
        // 当前帧的角点
        std::vector<cv::Point2f> currCorners;
        for (const auto& p : contour) {
            currCorners.push_back(cv::Point2f(p.x, p.y));
        }
        
        // 如果有前一帧数据，进行光流跟踪
        if (!prevGray.empty() && !prevCorners.empty()) {
            std::vector<cv::Point2f> trackedCorners;
            std::vector<uchar> status;
            std::vector<float> err;
            
            // 计算光流
            cv::calcOpticalFlowPyrLK(
                prevGray, gray, prevCorners, trackedCorners, 
                status, err, cv::Size(21, 21), 3,
                cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 30, 0.01)
            );
            
            // 计算当前系统的"惯性"数据 (模拟传感器)
            // 注意：实际应用中，这部分应从实际的IMU传感器获取
            QVector3D currAcceleration(0, 0, 0);  // 假设静止状态
            QQuaternion currOrientation = QQuaternion::fromAxisAndAngle(0, 0, 1, 0);  // 无旋转
            
            // 结合光流和惯性数据进行融合跟踪
            if (std::all_of(status.begin(), status.end(), [](uchar s) { return s != 0; })) {
                // 所有角点跟踪成功
                currCorners = trackedCorners;
            } else {
                // 部分角点跟踪失败，使用惯性预测
                for (size_t i = 0; i < status.size(); i++) {
                    if (status[i] == 0) {
                        // 使用上一帧的位置和"惯性"数据预测当前位置
                        QVector3D cornerPos(prevCorners[i].x, prevCorners[i].y, 0);
                        QVector3D delta = (currAcceleration - prevAcceleration) * 0.033f; // 假设33ms帧间隔
                        cornerPos += delta;
                        
                        // 应用旋转
                        QQuaternion deltaRot = currOrientation * prevOrientation.conjugated();
                        cornerPos = deltaRot.rotatedVector(cornerPos);
                        
                        trackedCorners[i] = cv::Point2f(cornerPos.x(), cornerPos.y());
                    }
                }
                currCorners = trackedCorners;
            }
            
            // 更新桌面轮廓
            contour.clear();
            for (const auto& p : currCorners) {
                contour.push_back(cv::Point(p.x, p.y));
            }
            
            // 保存当前"惯性"数据
            prevAcceleration = currAcceleration;
            prevOrientation = currOrientation;
        }
        
        // 保存当前帧信息用于下一次处理
        prevGray = gray.clone();
        prevCorners = currCorners;
        
    } catch (const std::exception& e) {
        qWarning() << "PDFViewerPage: 光流SLAM跟踪中发生异常:" << e.what();
    } catch (...) {
        qWarning() << "PDFViewerPage: 光流SLAM跟踪中发生未知异常";
    }
}


void PDFViewerPage::initArUcoDetector()
{
    try {
        // 初始化ArUco字典 - 使用4x4_50字典（50个唯一标记）
        arucoDict = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50);
        
        // 创建和配置检测参数
        arucoParams = cv::aruco::DetectorParameters::create();
        
        // 提高检测性能的参数调整
        arucoParams->cornerRefinementMethod = cv::aruco::CORNER_REFINE_SUBPIX;
        arucoParams->adaptiveThreshWinSizeMin = 3;
        arucoParams->adaptiveThreshWinSizeMax = 23;
        arucoParams->adaptiveThreshWinSizeStep = 10;
        arucoParams->cornerRefinementWinSize = 5;
        arucoParams->cornerRefinementMaxIterations = 30;
        arucoParams->cornerRefinementMinAccuracy = 0.01;
        
        // 降低误检率的参数
        arucoParams->minMarkerPerimeterRate = 0.03;  // 标记必须足够大
        arucoParams->maxMarkerPerimeterRate = 0.5;   // 但不能太大
        arucoParams->polygonalApproxAccuracyRate = 0.05;
        arucoParams->minCornerDistanceRate = 0.05;
        
        // UI组件初始化 - 添加一个复选框来启用/禁用ArUco跟踪
        enableArucoCheckbox = new QCheckBox("启用ArUco标记跟踪", this);
        enableArucoCheckbox->setChecked(useArUcoTracking);
        connect(enableArucoCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
            useArUcoTracking = checked;
            if (checked && !desktopLocked) {
                statusLabel->setText("ArUco跟踪已启用，请确保标记可见");
            } else if (!checked && desktopLocked) {
                statusLabel->setText("已切换到常规跟踪模式");
            }
        });
        
        // 将复选框添加到现有的工具栏布局中
        QLayout* toolbarLayout = nullptr;
        for (QObject* child : children()) {
            QLayout* layout = qobject_cast<QLayout*>(child);
            if (layout && layout->objectName() == "toolbarLayout") {
                toolbarLayout = layout;
                break;
            }
        }
        
        if (toolbarLayout) {
            toolbarLayout->addWidget(enableArucoCheckbox);
        } else {
            // 如果找不到现有的工具栏布局，添加到主布局
            QBoxLayout* mainLayout = qobject_cast<QBoxLayout*>(layout());
            if (mainLayout) {
                mainLayout->insertWidget(0, enableArucoCheckbox);
            }
        }
        
        // 初始化计时器
        markerLostTimer.start();
        
        qDebug() << "ArUco检测器初始化成功";
    } catch (const std::exception& e) {
        qWarning() << "ArUco检测器初始化失败:" << e.what();
    } catch (...) {
        qWarning() << "ArUco检测器初始化时发生未知异常";
    }
}

bool PDFViewerPage::detectDesktopWithArUco(cv::Mat& frame, std::vector<cv::Point>& deskContour)
{
    if (!useArUcoTracking || frame.empty()) {
        return false;
    }
    
    try {
        // 转换为灰度图，提高检测效率
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        
        // 清除上一帧的检测结果
        markerIds.clear();
        markerCorners.clear();
        
        // 执行ArUco标记检测
        cv::aruco::detectMarkers(gray, arucoDict, markerCorners, markerIds, arucoParams);
        
        // 在图像中绘制检测到的标记
        if (markerIds.size() > 0) {
            cv::aruco::drawDetectedMarkers(frame, markerCorners, markerIds);
        }
        
        // 根据检测到的标记估计桌面
        if (markerIds.size() >= 1) {
            // 尝试估计桌面轮廓
            estimateDeskFromValidMarkers(markerIds, markerCorners, deskContour);
            
            // 如果找到有效的桌面轮廓
            if (deskContour.size() == 4) {
                // 重置丢失标记计时器
                markerLostTimer.restart();
                
                // 获取桌面四个角点并存储为上次有效角点
                lastValidCorners.clear();
                for (const auto& pt : deskContour) {
                    lastValidCorners.push_back(cv::Point2f(pt.x, pt.y));
                }
                
                // 在图像上标记检测到的桌面
                std::vector<std::vector<cv::Point>> contours = { deskContour };
                cv::polylines(frame, contours, true, cv::Scalar(0, 255, 0), 2);
                
                // 添加标记文本
                cv::putText(frame, "ArUco桌面已锁定", cv::Point(20, 30),
                           cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
                
                return true;
            }
        } 
        else if (lastValidCorners.size() == 4 && markerLostTimer.elapsed() < 3000) {
            // 如果标记暂时丢失不超过3秒，使用最后的有效桌面
            deskContour.clear();
            for (const auto& pt : lastValidCorners) {
                deskContour.push_back(cv::Point(pt.x, pt.y));
            }
            
            // 在图像上标记推断的桌面
            std::vector<std::vector<cv::Point>> contours = { deskContour };
            cv::polylines(frame, contours, true, cv::Scalar(0, 255, 255), 2);
            
            // 添加警告文本
            cv::putText(frame, "标记丢失 - 使用上次位置", cv::Point(20, 30),
                       cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
            
            return true;
        }
        else {
            // 标记完全丢失
            cv::putText(frame, "未检测到标记 - 请确保4个ArUco标记可见", cv::Point(20, 30),
                       cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
            
            // 清除上次有效角点
            if (markerLostTimer.elapsed() > 3000) {
                lastValidCorners.clear();
            }
        }
    }
    catch (const std::exception& e) {
        qWarning() << "ArUco检测异常:" << e.what();
    }
    catch (...) {
        qWarning() << "ArUco检测未知异常";
    }
    
    return false;
}

void PDFViewerPage::estimateDeskFromValidMarkers(
    const std::vector<int>& ids, 
    const std::vector<std::vector<cv::Point2f>>& corners,
    std::vector<cv::Point>& deskContour)
{
    static std::vector<cv::Point> previousContour;
    static QElapsedTimer contourUpdateTimer;
    static bool isFirstRun = true;
    
    // 初始化计时器
    if (isFirstRun) {
        contourUpdateTimer.start();
        isFirstRun = false;
    }
    
    // 清空输出轮廓
    deskContour.clear();
    
    // 如果检测到的标记不多且上次轮廓合理且时间较短，优先使用上次轮廓
    if (ids.size() < 3 && !previousContour.empty() && contourUpdateTimer.elapsed() < 500) {
        deskContour = previousContour;
        return;
    }
    
    // 策略1: 四个标记都被检测到
    if (ids.size() >= 4) {
        // 查找ID 0-3的标记
        std::vector<cv::Point2f> orderedCorners(4);
        bool foundAll = true;
        
        for (int targetId = 0; targetId < 4; targetId++) {
            bool found = false;
            for (size_t i = 0; i < ids.size(); i++) {
                if (ids[i] == targetId) {
                    // 计算标记中心点
                    cv::Point2f center(0, 0);
                    for (const auto& pt : corners[i]) {
                        center += pt;
                    }
                    center *= 0.25f;
                    
                    orderedCorners[targetId] = center;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                foundAll = false;
                break;
            }
        }
        
        if (foundAll) {
            // 使用四个标记的中心点构建桌面轮廓
            for (const auto& pt : orderedCorners) {
                deskContour.push_back(cv::Point(pt.x, pt.y));
            }
            return;
        }
    }
    
    // 策略2: 检测到不足四个标记，但至少有两个
    if (ids.size() >= 2) {
        // 收集所有检测到的标记中心点
        std::vector<cv::Point2f> centers;
        std::vector<int> centerIds;
        
        for (size_t i = 0; i < ids.size(); i++) {
            // 仅考虑ID为0-3的标记
            if (ids[i] >= 0 && ids[i] <= 3) {
                cv::Point2f center(0, 0);
                for (const auto& pt : corners[i]) {
                    center += pt;
                }
                center *= 0.25f;
                
                centers.push_back(center);
                centerIds.push_back(ids[i]);
            }
        }
        
        if (centers.size() >= 2) {
            // 估计标记间的距离关系，推断完整桌面
            std::vector<cv::Point2f> estimatedCorners(4);
            bool cornerEstimated[4] = {false, false, false, false};
            
            // 先记录已知的角点
            for (size_t i = 0; i < centerIds.size(); i++) {
                int id = centerIds[i];
                estimatedCorners[id] = centers[i];
                cornerEstimated[id] = true;
            }
            
            // 简单情况：两个对角标记被检测到
            if (cornerEstimated[0] && cornerEstimated[2]) {
                // 估计右上角 (基于0和2的中点，然后水平移动)
                estimatedCorners[1].x = estimatedCorners[2].x;
                estimatedCorners[1].y = estimatedCorners[0].y;
                cornerEstimated[1] = true;
                
                // 估计左下角
                estimatedCorners[3].x = estimatedCorners[0].x;
                estimatedCorners[3].y = estimatedCorners[2].y;
                cornerEstimated[3] = true;
            }
            else if (cornerEstimated[1] && cornerEstimated[3]) {
                // 估计左上角
                estimatedCorners[0].x = estimatedCorners[3].x;
                estimatedCorners[0].y = estimatedCorners[1].y;
                cornerEstimated[0] = true;
                
                // 估计右下角
                estimatedCorners[2].x = estimatedCorners[1].x;
                estimatedCorners[2].y = estimatedCorners[3].y;
                cornerEstimated[2] = true;
            }
            // 水平或垂直相邻的两个标记
            else if (cornerEstimated[0] && cornerEstimated[1]) {
                // 我们有左上和右上，估计下方角点
                float width = cv::norm(estimatedCorners[1] - estimatedCorners[0]);
                
                // 假设桌面为矩形，根据预设宽高比估计高度
                float height = width * 0.7f; // 假设宽高比约为1.4 (如A4纸)
                
                estimatedCorners[3].x = estimatedCorners[0].x;
                estimatedCorners[3].y = estimatedCorners[0].y + height;
                cornerEstimated[3] = true;
                
                estimatedCorners[2].x = estimatedCorners[1].x;
                estimatedCorners[2].y = estimatedCorners[1].y + height;
                cornerEstimated[2] = true;
            }
            // 其他情况类似处理...
            
            // 检查是否所有角点都已估计
            if (cornerEstimated[0] && cornerEstimated[1] && 
                cornerEstimated[2] && cornerEstimated[3]) {
                
                // 修复：将数组转换为向量
                std::vector<cv::Point2f> cornerVector(estimatedCorners.begin(), estimatedCorners.end());
                
                // 检查估计的轮廓是否合理
                if (isValidDeskConfiguration(cornerVector)) {
                    for (int i = 0; i < 4; i++) {
                        deskContour.push_back(cv::Point(estimatedCorners[i].x, estimatedCorners[i].y));
                    }
                    return;
                }
            }
        }
    }
    
    // 策略3: 只检测到一个标记，使用最后一组有效角点作为参考
    if (ids.size() == 1 && lastValidCorners.size() == 4 && markerLostTimer.elapsed() < 2000) {
        int id = ids[0];
        if (id >= 0 && id <= 3) {
            // 计算检测到的标记中心点
            cv::Point2f center(0, 0);
            for (const auto& pt : corners[0]) {
                center += pt;
            }
            center *= 0.25f;
            
            // 计算相对于上一次有效位置的偏移
            cv::Point2f offset = center - cv::Point2f(lastValidCorners[id].x, lastValidCorners[id].y);
            
            // 应用偏移创建新的轮廓
            for (int i = 0; i < 4; i++) {
                cv::Point2f newPt = cv::Point2f(lastValidCorners[i].x, lastValidCorners[i].y) + offset;
                deskContour.push_back(cv::Point(newPt.x, newPt.y));
            }
            return;
        }
    }

     // 如果成功估计出新轮廓，更新缓存
     if (!deskContour.empty()) {
        previousContour = deskContour;
        contourUpdateTimer.restart();
    }
}


// 验证桌面轮廓是否合理
bool PDFViewerPage::isValidDeskConfiguration(const std::vector<cv::Point2f>& corners)
{
    if (corners.size() != 4) return false;
    
    // 检查最小面积
    double area = cv::contourArea(corners);
    if (area < 1000) return false;  // 面积太小
    
    // 检查凸性
    std::vector<cv::Point> intCorners;
    for (const auto& pt : corners) {
        intCorners.push_back(cv::Point(pt.x, pt.y));
    }
    if (!cv::isContourConvex(intCorners)) return false;
    
    // 检查各边长度和比例
    float sides[4];
    for (int i = 0; i < 4; i++) {
        sides[i] = cv::norm(corners[i] - corners[(i+1)%4]);
        if (sides[i] < 20) return false;  // 边长太短
    }
    
    // 检查对边比例（矩形的对边应该近似相等）
    float ratio1 = sides[0] / sides[2];
    float ratio2 = sides[1] / sides[3];
    
    if (ratio1 < 0.7 || ratio1 > 1.3 || ratio2 < 0.7 || ratio2 > 1.3) {
        return false;  // 对边不平行或长度差异太大
    }
    
    return true;
}

void PDFViewerPage::generateAndShowArUcoMarkers()
{
    // 创建一个对话框展示标记
    QDialog *markerDialog = new QDialog(this);
    markerDialog->setWindowTitle("ArUco标记生成器");
    markerDialog->setMinimumSize(800, 600);
    
    // 创建布局
    QVBoxLayout *mainLayout = new QVBoxLayout(markerDialog);
    QLabel *infoLabel = new QLabel("打印下面的标记并放置在桌面四角。确保标记在相机视野内可见。", markerDialog);
    infoLabel->setWordWrap(true);
    mainLayout->addWidget(infoLabel);
    
    // 创建四个标记的网格布局
    QGridLayout *markersLayout = new QGridLayout();
    
    // 生成四个标记
    for (int id = 0; id < 4; id++) {
        // 创建ArUco标记
        cv::Mat markerImage;
        cv::aruco::drawMarker(arucoDict, id, 200, markerImage, 1);
        
        // 转换为QImage
        QImage qMarkerImage(markerImage.data, markerImage.cols, markerImage.rows, 
                          markerImage.step, QImage::Format_Grayscale8);
        qMarkerImage = qMarkerImage.copy();
        
        // 添加白色边框
        QImage borderedImage(qMarkerImage.width() + 40, qMarkerImage.height() + 40, QImage::Format_RGB888);
        borderedImage.fill(Qt::white);
        
        // 在中心绘制标记
        QPainter painter(&borderedImage);
        painter.drawImage(20, 20, qMarkerImage);
        
        // 添加标记ID
        painter.setPen(Qt::black);
        painter.setFont(QFont("Arial", 12, QFont::Bold));
        painter.drawText(QRect(0, qMarkerImage.height() + 25, borderedImage.width(), 20), 
                        Qt::AlignCenter, QString("ID: %1").arg(id));
        
        // 创建标记图像
        QLabel *markerLabel = new QLabel(markerDialog);
        markerLabel->setPixmap(QPixmap::fromImage(borderedImage));
        markerLabel->setAlignment(Qt::AlignCenter);
        
        // 添加到网格布局
        int row = id / 2;
        int col = id % 2;
        markersLayout->addWidget(markerLabel, row, col);
    }
    
    mainLayout->addLayout(markersLayout);
    
    // 创建桌面布局图
    QLabel *layoutLabel = new QLabel(markerDialog);
    QImage layoutImage(600, 400, QImage::Format_RGB888);
    layoutImage.fill(Qt::white);
    
    QPainter layoutPainter(&layoutImage);
    layoutPainter.setPen(QPen(Qt::black, 2));
    
    // 绘制桌面轮廓
    layoutPainter.drawRect(50, 50, 500, 300);
    
    // 放置标记位置
    QPoint markerPositions[4] = {
        QPoint(50, 50),      // 左上 (ID: 0)
        QPoint(550, 50),     // 右上 (ID: 1)
        QPoint(550, 350),    // 右下 (ID: 2)
        QPoint(50, 350)      // 左下 (ID: 3)
    };
    
    // 绘制标记位置
    layoutPainter.setPen(Qt::red);
    layoutPainter.setFont(QFont("Arial", 10, QFont::Bold));
    
    for (int id = 0; id < 4; id++) {
        layoutPainter.drawEllipse(markerPositions[id], 10, 10);
        layoutPainter.drawText(markerPositions[id] + QPoint(-5, 5), QString::number(id));
    }
    
    // 添加说明文字
    layoutPainter.setPen(Qt::black);
    layoutPainter.setFont(QFont("Arial", 12));
    layoutPainter.drawText(QRect(150, 150, 300, 100), Qt::AlignCenter, 
                          "将标记放置在桌面四角\n箭头表示标记的朝向\n确保标记朝上且不被遮挡");
    
    layoutLabel->setPixmap(QPixmap::fromImage(layoutImage));
    layoutLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(layoutLabel);
    
    // 添加按钮
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    
    QPushButton *saveButton = new QPushButton("保存标记", markerDialog);
    connect(saveButton, &QPushButton::clicked, [this]() {
        // 创建并保存合并的标记图像
        QImage combinedImage(800, 1100, QImage::Format_RGB888);
        combinedImage.fill(Qt::white);
        
        QPainter painter(&combinedImage);
        
        // 顶部标题
        painter.setPen(Qt::black);
        painter.setFont(QFont("Arial", 16, QFont::Bold));
        painter.drawText(QRect(0, 20, 800, 40), Qt::AlignCenter, "AR PDF查看器 - ArUco标记");
        
        // 绘制四个标记
        for (int id = 0; id < 4; id++) {
            // 创建标记
            cv::Mat markerImage;
            cv::aruco::drawMarker(arucoDict, id, 200, markerImage, 1);
            
            // 转换为QImage
            QImage qMarkerImage(markerImage.data, markerImage.cols, markerImage.rows, 
                              markerImage.step, QImage::Format_Grayscale8);
            qMarkerImage = qMarkerImage.copy();
            
            // 定位标记
            int row = id / 2;
            int col = id % 2;
            int x = 100 + col * 300;
            int y = 100 + row * 300;
            
            // 绘制白色背景和边框
            painter.fillRect(x - 20, y - 20, 240, 240, Qt::white);
            painter.setPen(QPen(Qt::black, 2));
            painter.drawRect(x - 20, y - 20, 240, 240);
            
            // 绘制标记
            painter.drawImage(x, y, qMarkerImage);
            
            // 添加ID标签
            painter.setPen(Qt::black);
            painter.setFont(QFont("Arial", 12, QFont::Bold));
            painter.drawText(QRect(x, y + 210, 200, 30), Qt::AlignCenter, 
                            QString("标记 ID: %1").arg(id));
        }
        
        // 添加使用说明
        painter.setFont(QFont("Arial", 14));
        painter.drawText(QRect(100, 600, 600, 200), Qt::AlignCenter, 
                        "使用说明:\n"
                        "1. 打印此页并沿虚线剪下四个标记\n"
                        "2. 将标记贴在桌面四角，遵循下图布局\n"
                        "3. 确保标记平整且没有反光\n"
                        "4. 标记之间距离建议在20-50厘米之间\n"
                        "5. 启动应用，打开摄像头，确保四个标记同时可见");
        
        // 绘制布局示意图
        painter.setPen(QPen(Qt::black, 2));
        painter.drawRect(250, 800, 300, 200);
        
        // 标记位置
        QPoint positions[4] = {
            QPoint(250, 800),    // 左上 (ID: 0)
            QPoint(550, 800),    // 右上 (ID: 1)
            QPoint(550, 1000),   // 右下 (ID: 2)
            QPoint(250, 1000)    // 左下 (ID: 3)
        };
        
        // 绘制标记位置指示
        painter.setPen(Qt::red);
        painter.setFont(QFont("Arial", 10, QFont::Bold));
        
        for (int id = 0; id < 4; id++) {
            painter.drawEllipse(positions[id], 15, 15);
            painter.drawText(positions[id] + QPoint(-5, 5), QString::number(id));
        }
        
        // 保存图像
        QString filePath = QFileDialog::getSaveFileName(this, "保存ArUco标记", 
                                                      QDir::homePath() + "/aruco_markers.png", 
                                                      "图像文件 (*.png *.jpg)");
        if (!filePath.isEmpty()) {
            if (combinedImage.save(filePath)) {
                QMessageBox::information(this, "保存成功", 
                                        "ArUco标记已保存至:\n" + filePath);
            } else {
                QMessageBox::warning(this, "保存失败", 
                                    "无法保存文件，请检查路径权限");
            }
        }
    });
    
    QPushButton *closeButton = new QPushButton("关闭", markerDialog);
    connect(closeButton, &QPushButton::clicked, markerDialog, &QDialog::accept);
    
    buttonLayout->addWidget(saveButton);
    buttonLayout->addWidget(closeButton);
    mainLayout->addLayout(buttonLayout);
    
    markerDialog->setLayout(mainLayout);
    markerDialog->exec();
}



// 线程实现
ArUcoProcessorThread::ArUcoProcessorThread(QObject *parent)
    : QThread(parent), m_running(false), m_maxQueueSize(5)
{
    // 初始化ArUco检测器
    m_arucoDict = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50);
    m_arucoParams = cv::aruco::DetectorParameters::create();
    
    // 优化检测参数
    m_arucoParams->cornerRefinementMethod = cv::aruco::CORNER_REFINE_SUBPIX;
    m_arucoParams->adaptiveThreshWinSizeMin = 3;
    m_arucoParams->adaptiveThreshWinSizeMax = 23;
    m_arucoParams->adaptiveThreshWinSizeStep = 10;
    m_arucoParams->cornerRefinementMaxIterations = 30;
}

ArUcoProcessorThread::~ArUcoProcessorThread()
{
    stop();
    wait();
}

void ArUcoProcessorThread::processFrame(const cv::Mat& frame)
{
    QMutexLocker locker(&m_mutex);
    
    // 限制队列大小，防止内存溢出
    if (m_frameQueue.size() >= m_maxQueueSize) {
        m_frameQueue.dequeue(); // 移除最旧的帧
    }
    
    m_frameQueue.enqueue(frame.clone()); // 添加深拷贝
    m_condition.wakeOne();
}

void ArUcoProcessorThread::stop()
{
    QMutexLocker locker(&m_mutex);
    m_running = false;
    m_condition.wakeAll();
}

void ArUcoProcessorThread::run()
{
    m_running = true;
    
    // 帧处理计数器
    int frameCounter = 0;
    
    // 性能监控
    QElapsedTimer performanceTimer;
    double totalProcessingTime = 0;
    int processedFrames = 0;
    
    while (m_running) {
        performanceTimer.start();
        
        cv::Mat frame;
        
        {
            QMutexLocker locker(&m_mutex);
            
            // 等待新帧或停止信号
            while (m_frameQueue.isEmpty() && m_running) {
                m_condition.wait(&m_mutex);
            }
            
            if (!m_running) {
                break;
            }
            
            frame = m_frameQueue.dequeue();
        }
        
        // 增加计数器
        frameCounter++;
        
        // 图像预处理 - 使用高斯模糊减少噪声
        cv::Mat preprocessed;
        cv::GaussianBlur(frame, preprocessed, cv::Size(5, 5), 0);
        
        // 转换为灰度
        cv::Mat gray;
        cv::cvtColor(preprocessed, gray, cv::COLOR_BGR2GRAY);
        
        // 每3帧完整检测一次，其他帧使用光流跟踪
        std::vector<int> ids;
        std::vector<std::vector<cv::Point2f>> corners;
        
        if (frameCounter % 3 == 0) {
            // 完整的ArUco检测
            cv::aruco::detectMarkers(gray, m_arucoDict, corners, ids, m_arucoParams);
        } 
        else {
            // 如果有上一帧的结果，使用光流跟踪
            static std::vector<int> lastIds;
            static std::vector<std::vector<cv::Point2f>> lastCorners;
            static cv::Mat lastGray;
            
            if (!lastIds.empty() && !lastGray.empty()) {
                ids = lastIds;
                corners = lastCorners;
                
                // 对每个标记的角点进行光流跟踪
                for (size_t i = 0; i < corners.size(); i++) {
                    std::vector<cv::Point2f> oldPoints = corners[i];
                    std::vector<cv::Point2f> newPoints;
                    std::vector<uchar> status;
                    std::vector<float> err;
                    
                    cv::calcOpticalFlowPyrLK(lastGray, gray, oldPoints, newPoints, 
                                           status, err, cv::Size(21, 21), 3);
                    
                    // 更新角点位置
                    for (size_t j = 0; j < status.size(); j++) {
                        if (status[j]) {
                            corners[i][j] = newPoints[j];
                        }
                    }
                }
            } 
            else {
                // 如果没有上一帧结果，执行完整检测
                cv::aruco::detectMarkers(gray, m_arucoDict, corners, ids, m_arucoParams);
            }
            
            // 存储当前帧结果用于下一帧
            lastIds = ids;
            lastCorners = corners;
            lastGray = gray.clone();
        }
        
        // 在帧上绘制检测结果
        cv::Mat outputImage = frame.clone();
        if (!ids.empty()) {
            cv::aruco::drawDetectedMarkers(outputImage, corners, ids);
            
            // 添加性能信息
            double processingTime = performanceTimer.elapsed();
            totalProcessingTime += processingTime;
            processedFrames++;
            
            double avgTime = totalProcessingTime / processedFrames;
            cv::putText(outputImage, 
                       cv::format("Processing: %.1f ms (avg: %.1f ms)", processingTime, avgTime),
                       cv::Point(10, outputImage.rows - 10), 
                       cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
        }
        
        // 每30帧重置性能计数器
        if (processedFrames > 30) {
            totalProcessingTime = 0;
            processedFrames = 0;
        }
        
        // 发送结果信号
        emit markersDetected(ids, corners, outputImage);
    }
}


// 处理检测到的标记
void PDFViewerPage::handleDetectedMarkers(const std::vector<int>& ids,
    const std::vector<std::vector<cv::Point2f>>& corners,
    const cv::Mat& processedImage)
{
 // Use mutex to protect shared resources
 QMutexLocker locker(&m_renderMutex);
    
 // Create a black background for rendering
 //cv::Mat renderFrame = cv::Mat::zeros(processedImage.size(), CV_8UC3);
 cv::Mat renderFrame = processedImage.clone();
 // Update desktop contour based on detection results
 std::vector<cv::Point> deskContour;
 bool markersDetected = false;
 
 if (!ids.empty()) {
     estimateDeskFromValidMarkers(ids, corners, deskContour);
     markersDetected = true;
 }
 
 if (!deskContour.empty()) {
     lockedDesktopContour = deskContour;
     desktopLocked = true;
     desktopDetected = true;
     
     // Reset timeout timer
     markerLostTimer.restart();
     
     // Record last valid corners
     lastValidCorners.clear();
     for (const auto& pt : deskContour) {
         lastValidCorners.push_back(cv::Point2f(pt.x, pt.y));
     }
     
     // Only overlay PDF if markers are detected and PDF is loaded
     if (desktopLocked && !currentPdfFrame.isNull()) {
         // Use simplified rendering method on black background
         enhancedOverlayPDF(renderFrame, lockedDesktopContour);
     }
 } 
 else if (lastValidCorners.size() == 4 && markerLostTimer.elapsed() < 3000) {
     // If markers lost for less than 3 seconds, use last valid desktop
     deskContour.clear();
     for (const auto& pt : lastValidCorners) {
         deskContour.push_back(cv::Point(pt.x, pt.y));
     }
     
     // Only render PDF for a short time after markers are lost
     if (!currentPdfFrame.isNull()) {
         enhancedOverlayPDF(renderFrame, deskContour);
     }
     
     // Add warning text
     cv::putText(renderFrame, "标记丢失 - 使用上次位置", cv::Point(20, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
 }
 else {
     // No markers detected and timeout expired
     desktopLocked = false;
     
     // Do not render PDF, just show a black screen with status message
     cv::putText(renderFrame, "未检测到标记 - 请确保ArUco标记可见", cv::Point(20, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
 }
 
 
 
 // Display the processed image
 QImage displayImage(renderFrame.data, renderFrame.cols, renderFrame.rows, 
                    renderFrame.step, QImage::Format_BGR888);
 QImage displayImageCopy = displayImage.copy();
 
 // Update UI (in main thread)
 QMetaObject::invokeMethod(this, [this, displayImageCopy]() {
     processedLabel->setPixmap(QPixmap::fromImage(displayImageCopy)
                             .scaled(processedLabel->size(), Qt::KeepAspectRatio, 
                                    Qt::SmoothTransformation));
 }, Qt::QueuedConnection);
 
 // Update status message based on marker detection
 QMetaObject::invokeMethod(this, [this, markersDetected]() {
     if (markersDetected) {
         statusLabel->setText(QString("检测到标记 - 正在显示页面 %1/%2").arg(currentPage + 1).arg(pdfDocument->pageCount()));
     } else if (!markersDetected && markerLostTimer.elapsed() < 3000) {
         statusLabel->setText(QString("标记暂时丢失 - 继续显示页面 %1/%2").arg(currentPage + 1).arg(pdfDocument->pageCount()));
     } else {
         statusLabel->setText("未检测到标记 - 请确保ArUco标记在摄像头视野内");
     }
 }, Qt::QueuedConnection);
}



// 添加内存使用监控
void PDFViewerPage::monitorMemoryUsage()
{
    static QElapsedTimer memoryTimer;
    static bool firstRun = true;
    
    if (firstRun) {
        memoryTimer.start();
        firstRun = false;
        return;
    }
    
    // 每10秒检查一次内存使用
    if (memoryTimer.elapsed() > 10000) {
        memoryTimer.restart();
        
        // 获取进程内存信息
        QProcess process;
        process.start("ps", QStringList() << "-p" << QString::number(QCoreApplication::applicationPid()) << "-o" << "rss=");
        process.waitForFinished();
        QString memory = process.readAllStandardOutput().trimmed();
        
        bool ok;
        long memoryUsage = memory.toLong(&ok);
        if (ok) {
            // 转换为MB
            float memoryMB = memoryUsage / 1024.0f;
            
            qDebug() << "当前内存使用:" << memoryMB << "MB";
            
            // 如果内存使用过高，触发清理
            if (memoryMB > 500) {  // 超过500MB时清理
                qDebug() << "内存使用过高，执行清理...";
                
                // 清理资源
                ResourceManager::instance().cleanupResources();
                
                // 建议GC
                QMetaObject::invokeMethod(this, [this]() {
                    processedLabel->clear();
                    processedLabel->update();
                }, Qt::QueuedConnection);
            }
        }
    }
}


void PDFViewerPage::optimizeCameraSettings()
{
    if (!camera) return;
    
    // 查找所有支持的分辨率和帧率
    const auto formats = camera->cameraDevice().videoFormats();
    
    if (formats.isEmpty()) {
        qDebug() << "无法获取相机格式列表";
        return;
    }
    
    // 打印所有支持的格式
    qDebug() << "可用的相机格式:";
    for (const auto& format : formats) {
        qDebug() << "分辨率:" << format.resolution() 
                << "最大帧率:" << format.maxFrameRate()
                << "像素格式:" << format.pixelFormat();
    }
    
    // 寻找低分辨率高帧率的格式
    QCameraFormat optimalFormat;
    bool found = false;
    
    // 首选格式：320x240 @ 30fps
    for (const auto& format : formats) {
        if (format.resolution() == QSize(320, 240) && format.maxFrameRate() >= 30) {
            optimalFormat = format;
            found = true;
            break;
        }
    }
    
    // 次选格式：任何640x480以下的分辨率
    if (!found) {
        for (const auto& format : formats) {
            if (format.resolution().width() <= 640 && format.resolution().height() <= 480) {
                optimalFormat = format;
                found = true;
                break;
            }
        }
    }
    
    // 最后选择：最低的分辨率
    if (!found && !formats.isEmpty()) {
        QSize minRes(100000, 100000);
        for (const auto& format : formats) {
            if (format.resolution().width() * format.resolution().height() < 
                minRes.width() * minRes.height()) {
                minRes = format.resolution();
                optimalFormat = format;
                found = true;
            }
        }
    }
    
    // 应用选择的格式
    if (found) {
        camera->setCameraFormat(optimalFormat);
        qDebug() << "设置相机为低分辨率模式:" 
                << optimalFormat.resolution()
                << "帧率:" << optimalFormat.maxFrameRate();
    } else {
        qDebug() << "无法找到合适的低分辨率相机格式";
    }
    
    
}

void PDFViewerPage::initGyroscope()
{
    gyroRoll = gyroPitch = gyroYaw = 0.0f;
    lastGyroRoll = lastGyroPitch = lastGyroYaw = 0.0f;
    gyroAvailable = false;
    gyroVisualWeight = 0.7f;  // 默认视觉跟踪权重较高
    gyroUpdateTimer.start();
    
    // 初始化旋转矩阵
    gyroRotationMatrix.setToIdentity();
    
    // 配置串口
    serialPort = new QSerialPort(this);
    serialPort->setPortName("/dev/ttyS3");  // 根据实际串口调整
    serialPort->setBaudRate(QSerialPort::Baud115200);
    serialPort->setDataBits(QSerialPort::Data8);
    serialPort->setParity(QSerialPort::NoParity);
    serialPort->setStopBits(QSerialPort::OneStop);
    
    // 连接信号槽
    connect(serialPort, &QSerialPort::readyRead, this, &PDFViewerPage::processSerialData);
    
    // 尝试打开串口
    if (serialPort->open(QIODevice::ReadOnly)) {
        qDebug() << "陀螺仪串口打开成功:" << serialPort->portName();
        gyroAvailable = true;
        
        // 添加UI提示
        if (statusLabel) {
            statusLabel->setText("陀螺仪已连接，增强AR跟踪稳定性");
        }
    } else {
        qWarning() << "陀螺仪串口打开失败:" << serialPort->errorString();
        // 添加UI提示
        if (statusLabel) {
            statusLabel->setText("未检测到陀螺仪，仅使用视觉跟踪");
        }
    }
}

void PDFViewerPage::processSerialData()
{
    if (!serialPort || !serialPort->isOpen())
        return;
        
    // 设置超时
    QElapsedTimer timeout;
    timeout.start();
    
    QByteArray buffer;
    
    // 读取数据直到找到完整的数据帧
    while (timeout.elapsed() < 100) {  // 100ms超时
        if (serialPort->bytesAvailable() > 0) {
            buffer.append(serialPort->readAll());
            
            // 查找帧头和帧尾
            int startIndex = buffer.indexOf("\xaa\xaa");
            int endIndex = buffer.indexOf("\xbb\xbb");
            
            if (startIndex >= 0 && endIndex > startIndex) {
                // 提取有效数据部分
                QByteArray frame = buffer.mid(startIndex + 2, endIndex - startIndex - 2);
                
                // 解析IMU数据
                if (frame.size() >= 7 && frame[0] == 0x02) {  // 0x02表示IMU数据
                    // 保存上一次的角度值用于计算角速度
                    lastGyroRoll = gyroRoll;
                    lastGyroPitch = gyroPitch;
                    lastGyroYaw = gyroYaw;
                    
                    // 解析数据（与pyqt5.py中的解析方式一致）
                    int roll16 = ((uchar)frame[1] << 8 | (uchar)frame[2]) / 100.0f;
                    gyroRoll = (roll16 > 3.2) ? roll16 - 655.35f : roll16;
                    
                    int pitch16 = ((uchar)frame[3] << 8 | (uchar)frame[4]) / 100.0f;
                    gyroPitch = (pitch16 > 3.2) ? pitch16 - 655.35f : pitch16;
                    
                    int yaw16 = ((uchar)frame[5] << 8 | (uchar)frame[6]) / 100.0f;
                    gyroYaw = (yaw16 > 3.2) ? yaw16 - 655.35f : yaw16;
                    
                    // 计算角速度
                    float deltaTime = gyroUpdateTimer.restart() / 1000.0f;  // 转换为秒
                    if (deltaTime > 0) {
                        gyroAngularVelocity.setX((gyroRoll - lastGyroRoll) / deltaTime);
                        gyroAngularVelocity.setY((gyroPitch - lastGyroPitch) / deltaTime);
                        gyroAngularVelocity.setZ((gyroYaw - lastGyroYaw) / deltaTime);
                    }
                    
                    // 更新旋转矩阵
                    updateGyroRotation();
                    
                    qDebug() << "IMU数据:" << "Roll:" << gyroRoll 
                             << "Pitch:" << gyroPitch 
                             << "Yaw:" << gyroYaw;
                    
                    return;  // 成功解析一帧，退出
                }
                
                // 移除处理过的数据
                buffer.remove(0, endIndex + 2);
            }
        } else {
            // 等待更多数据
            QThread::msleep(5);
        }
    }
}

void PDFViewerPage::updateGyroRotation()
{
    // 创建单独的旋转矩阵
    QMatrix4x4 rollMatrix, pitchMatrix, yawMatrix;
    
    // 按照Z-Y-X欧拉角顺序进行旋转
    rollMatrix.rotate(gyroRoll * 180.0f / M_PI, QVector3D(1, 0, 0));
    pitchMatrix.rotate(gyroPitch * 180.0f / M_PI, QVector3D(0, 1, 0));
    yawMatrix.rotate(gyroYaw * 180.0f / M_PI, QVector3D(0, 0, 1));
    
    // 复合旋转
    gyroRotationMatrix = yawMatrix * pitchMatrix * rollMatrix;
}


QMatrix4x4 PDFViewerPage::fuseCameraAndGyroData(const QMatrix4x4& cameraMatrix)
{
    if (!gyroAvailable) {
        return cameraMatrix;  // 无陀螺仪数据时直接返回相机矩阵
    }
    
    // 提取相机矩阵中的旋转部分
    QMatrix4x4 cameraRotation;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            cameraRotation(i, j) = cameraMatrix(i, j);
        }
    }
    
    // 权重融合旋转部分
    QMatrix4x4 fusedRotation;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            fusedRotation(i, j) = gyroVisualWeight * cameraRotation(i, j) + 
                                (1.0f - gyroVisualWeight) * gyroRotationMatrix(i, j);
        }
    }
    
    // 正交化旋转矩阵以确保其有效性
    // 这里实现格拉姆-施密特正交化过程
    QVector3D col0(fusedRotation(0, 0), fusedRotation(1, 0), fusedRotation(2, 0));
    col0.normalize();
    
    QVector3D col1(fusedRotation(0, 1), fusedRotation(1, 1), fusedRotation(2, 1));
    col1 = col1 - QVector3D::dotProduct(col1, col0) * col0;
    col1.normalize();
    
    QVector3D col2 = QVector3D::crossProduct(col0, col1);
    
    // 重建正交化后的旋转矩阵
    fusedRotation(0, 0) = col0.x(); fusedRotation(0, 1) = col1.x(); fusedRotation(0, 2) = col2.x();
    fusedRotation(1, 0) = col0.y(); fusedRotation(1, 1) = col1.y(); fusedRotation(1, 2) = col2.y();
    fusedRotation(2, 0) = col0.z(); fusedRotation(2, 1) = col1.z(); fusedRotation(2, 2) = col2.z();
    
    // 创建最终的融合矩阵，保留相机矩阵的平移部分
    QMatrix4x4 fusedMatrix = fusedRotation;
    fusedMatrix(0, 3) = cameraMatrix(0, 3);
    fusedMatrix(1, 3) = cameraMatrix(1, 3);
    fusedMatrix(2, 3) = cameraMatrix(2, 3);
    
    return fusedMatrix;
}


// 线程池处理视频帧的方法
void PDFViewerPage::processFrameInThreadPool(const QVideoFrame &frame)
{
    // 性能指标计算 - 记录开始时间
    QElapsedTimer frameTimer;
    frameTimer.start();
    
    // 检查是否有太多待处理任务，避免队列堆积
    if (m_pendingTasks.load() > 2) {
        qDebug() << "线程池任务积压过多，跳过此帧处理";
        return;
    }
    
    // 安全映射视频帧
    QVideoFrame clonedFrame = frame;
    if (!clonedFrame.map(QVideoFrame::ReadOnly)) {
        return;
    }
    
    // 转换为QImage
    QImage image = clonedFrame.toImage().convertToFormat(QImage::Format_RGB888);
    clonedFrame.unmap();
    
    if (image.isNull()) {
        return;
    }
    
    // 转换为OpenCV
    cv::Mat cvFrame(image.height(), image.width(), CV_8UC3, 
                   const_cast<uchar*>(image.constBits()), image.bytesPerLine());
    cv::Mat cvFrameCopy = cvFrame.clone();  // 深拷贝，确保数据安全
    cv::cvtColor(cvFrameCopy, cvFrameCopy, cv::COLOR_RGB2BGR);
    
    // 增加待处理任务计数
    m_pendingTasks++;
    
    // 提交高优先级处理任务到线程池
    ThreadPool::instance().enqueue([this, cvFrameCopy, frameTimer]() {
        try {
            // 根据当前性能模式决定处理分辨率
            if (m_lowPerformanceMode) {
                // 低性能模式 - 使用更低分辨率
                cv::Mat lowResFrame;
                cv::resize(cvFrameCopy, lowResFrame, cv::Size(), 0.4, 0.4, cv::INTER_LINEAR);
                processLowResFrame(lowResFrame);
            } else {
                // 标准模式 - 使用标准分辨率
                cv::Mat standardResFrame;
                cv::resize(cvFrameCopy, standardResFrame, cv::Size(), 0.5, 0.5, cv::INTER_LINEAR);
                processHighResFrame(standardResFrame);
            }
            
            // 计算并记录处理时间
            qint64 processTime = frameTimer.elapsed();
            QMutexLocker locker(&m_frameQueueMutex);
            m_frameTimes.enqueue(processTime);
            while (m_frameTimes.size() > m_frameTimeWindowSize) {
                m_frameTimes.dequeue();
            }
            
            // 计算平均帧处理时间和帧率
            qint64 totalTime = 0;
            for (qint64 time : m_frameTimes) {
                totalTime += time;
            }
            double avgTime = totalTime / static_cast<double>(m_frameTimes.size());
            m_currentFps = (avgTime > 0) ? 1000.0 / avgTime : 0.0;
            
            // 根据性能自动调整处理质量
            adjustProcessingQuality();
            
            // 减少待处理任务计数
            m_pendingTasks--;
            m_frameProcessedCondition.wakeAll();
            
            // 更新UI上的性能指标显示
            QMetaObject::invokeMethod(this, [this, processTime]() {
                if (m_performanceLabel) {
                    m_performanceLabel->setText(QString("处理时间: %1 ms | FPS: %2 | 模式: %3")
                        .arg(processTime)
                        .arg(m_currentFps, 0, 'f', 1)
                        .arg(m_lowPerformanceMode ? "低性能" : "标准"));
                }
            }, Qt::QueuedConnection);
            
        } catch (const std::exception& e) {
            qWarning() << "线程池处理帧异常:" << e.what();
            m_pendingTasks--;  // 确保计数器减少，即使发生异常
        } catch (...) {
            qWarning() << "线程池处理帧未知异常";
            m_pendingTasks--;
        }
    });
}

// 高分辨率帧处理方法
void PDFViewerPage::processHighResFrame(const cv::Mat& frame)
{
    try {
        cv::Mat processedFrame = frame.clone();

        // 检测/跟踪桌面
        if (!desktopLocked) {
            // 使用ArUco或常规方法检测桌面
            std::vector<cv::Point> deskContour;
            if (useArUcoTracking) {
                if (detectDesktopWithArUco(processedFrame, deskContour)) {
                    lockedDesktopContour = deskContour;
                    desktopLocked = true;
                    desktopDetected = true;
                    
                    // 重置计时器
                    markerLostTimer.restart();
                }
            } else if (detectDesktop(processedFrame, deskContour)) {
                lockedDesktopContour = deskContour;
                desktopLocked = true;
                desktopDetected = true;
                
                // 检测特征点以备跟踪
                cv::Mat gray;
                cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
                cv::goodFeaturesToTrack(gray, prevFeaturePoints, 100, 0.01, 10);
                prevGray = gray.clone();
            }
        } else {
            // 桌面已锁定，执行跟踪
            switch (slamMode) {
                case SlamMode::FeaturePoint:
                    trackDesktopWithFeatureSLAM(processedFrame, lockedDesktopContour);
                    break;
                case SlamMode::OpticalFlow:
                    trackDesktopWithOpticalFlowSLAM(processedFrame, lockedDesktopContour);
                    break;
                default:
                    trackDesktop(processedFrame, lockedDesktopContour);
                    break;
            }

            // 叠加PDF
            if (desktopLocked && !currentPdfFrame.isNull()) {
                enhancedOverlayPDF(processedFrame, lockedDesktopContour);
            }
        }

        // 发送处理后的帧到UI线程
        QImage displayImage(processedFrame.data, processedFrame.cols, processedFrame.rows, 
                          processedFrame.step, QImage::Format_BGR888);
        QImage displayImageCopy = displayImage.copy();
        
        QMetaObject::invokeMethod(this, [this, displayImageCopy]() {
            processedLabel->setPixmap(QPixmap::fromImage(displayImageCopy)
                                     .scaled(processedLabel->size(), Qt::KeepAspectRatio, 
                                             Qt::SmoothTransformation));
        }, Qt::QueuedConnection);
                
        // 将帧发送到ArUco处理线程
        m_arucoProcessor->processFrame(frame);
        
    } catch (const std::exception& e) {
        qWarning() << "高分辨率帧处理异常:" << e.what();
    } catch (...) {
        qWarning() << "高分辨率帧处理未知异常";
    }
}

// 低分辨率帧处理方法 - 优化性能
void PDFViewerPage::processLowResFrame(const cv::Mat& frame)
{
    try {
        cv::Mat processedFrame = frame.clone();
        
        // 简化版桌面检测/跟踪
        if (!desktopLocked) {
            // 仅使用ArUco检测，速度更快
            std::vector<cv::Point> deskContour;
            if (useArUcoTracking && detectDesktopWithArUco(processedFrame, deskContour)) {
                lockedDesktopContour = deskContour;
                desktopLocked = true;
                desktopDetected = true;
                markerLostTimer.restart();
            }
        } else {
            // 使用最基本的跟踪方法
            trackDesktop(processedFrame, lockedDesktopContour);
            
            // 简化版PDF叠加
            if (desktopLocked && !currentPdfFrame.isNull()) {
                // 用简单版叠加替代增强版，提高性能
                overlayPDF(processedFrame, lockedDesktopContour);
            }
        }
        
        // 显示处理后的帧
        QImage displayImage(processedFrame.data, processedFrame.cols, processedFrame.rows, 
                          processedFrame.step, QImage::Format_BGR888);
        QImage displayImageCopy = displayImage.copy();
        
        QMetaObject::invokeMethod(this, [this, displayImageCopy]() {
            processedLabel->setPixmap(QPixmap::fromImage(displayImageCopy)
                                     .scaled(processedLabel->size(), Qt::KeepAspectRatio));
        }, Qt::QueuedConnection);
        
    } catch (const std::exception& e) {
        qWarning() << "低分辨率帧处理异常:" << e.what();
    } catch (...) {
        qWarning() << "低分辨率帧处理未知异常";
    }
}

// 根据性能自动调整处理质量
void PDFViewerPage::adjustProcessingQuality()
{
    // 帧率过低时切换到低性能模式
    const double lowFpsThreshold = 15.0;  // 当FPS低于15时切换到低性能模式
    const double highFpsThreshold = 25.0; // 当FPS高于25时切换回标准模式
    
    if (!m_lowPerformanceMode && m_currentFps < lowFpsThreshold) {
        m_lowPerformanceMode = true;
        qDebug() << "切换到低性能模式，FPS:" << m_currentFps;
        
        // 调整其他性能相关参数
        QMetaObject::invokeMethod(this, [this]() {
            if (m_performanceLabel) {
                m_performanceLabel->setStyleSheet("color: red;");
            }
        }, Qt::QueuedConnection);
    }
    else if (m_lowPerformanceMode && m_currentFps > highFpsThreshold) {
        m_lowPerformanceMode = false;
        qDebug() << "恢复标准性能模式，FPS:" << m_currentFps;
        
        QMetaObject::invokeMethod(this, [this]() {
            if (m_performanceLabel) {
                m_performanceLabel->setStyleSheet("");
            }
        }, Qt::QueuedConnection);
    }
    
    // 根据性能调整线程池大小
    if (m_pendingTasks.load() > 1 && ThreadPool::instance().activeThreadCount() 
        == ThreadPool::instance().threadCount()) {
        // 所有线程都很忙，且有积压任务，考虑增加线程
        if (ThreadPool::instance().threadCount() < QThread::idealThreadCount()) {
            ThreadPool::instance().setThreadCount(ThreadPool::instance().threadCount() + 1);
            qDebug() << "增加线程池大小:" << ThreadPool::instance().threadCount();
        }
    }
}