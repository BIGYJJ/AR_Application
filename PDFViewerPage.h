#ifndef PDFVIEWERPAGE_H
#define PDFVIEWERPAGE_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QVideoWidget>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QPdfDocument>
#include <QSlider>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QMatrix4x4>
#include <opencv2/opencv.hpp>
#include <opencv2/aruco.hpp>
#include <QPropertyAnimation>
#include <QOpenGLBuffer>              // 添加这行
#include <QOpenGLVertexArrayObject>   // 添加这行
#include <QThread>
#include <QProcess>
#include <QTimer>
#include <QPdfDocumentRenderOptions>
#include <QComboBox>
#include <QQuaternion>
#include <QVector3D>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <iostream>
#include <vector>
#include <string>
#include <QCheckBox>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QImageReader>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QSerialPort>
#include <QSerialPortInfo>
#include "ThreadPool.h"
#include "CameraResourceManager.h"  // 添加中央摄像头管理器
// 定义ArUco处理线程类
class ArUcoProcessorThread : public QThread {
    Q_OBJECT
public:
    explicit ArUcoProcessorThread(QObject *parent = nullptr);
    ~ArUcoProcessorThread();

    void processFrame(const cv::Mat& frame);
    void stop();
    

signals:
    void markersDetected(const std::vector<int>& ids,
                         const std::vector<std::vector<cv::Point2f>>& corners,
                         const cv::Mat& image);

protected:
    void run() override;

private:
    bool m_running;
    QMutex m_mutex;
    QWaitCondition m_condition;
    QQueue<cv::Mat> m_frameQueue;
    cv::Ptr<cv::aruco::Dictionary> m_arucoDict;
    cv::Ptr<cv::aruco::DetectorParameters> m_arucoParams;
    int m_maxQueueSize;
};


class ResourceManager {
    public:
        // 单例模式
        static ResourceManager& instance() {
            static ResourceManager instance;
            return instance;
        }
        void clearCacheEntry(const QString& key) {
            QMutexLocker locker(&m_mutex);
            if (m_imageCache.contains(key)) {
                m_imageCache.remove(key);
                qDebug() << "已清除缓存:" << key;
            }
        }
        // 清理未使用的缓存资源
        void cleanupResources() {
            QMutexLocker locker(&m_mutex);
            
            // 清理图像缓存
            for (auto it = m_imageCache.begin(); it != m_imageCache.end();) {
                if (it.value().lastUsed.elapsed() > 30000) { // 30秒未使用
                    it = m_imageCache.erase(it);
                } else {
                    ++it;
                }
            }
            
            // 手动触发一次垃圾回收
            // 移除错误的QImageReader::cleanup()调用
            // 这里改为手动触发Qt的垃圾回收机制
            qDebug() << "释放未使用的图像资源，当前缓存项数：" << m_imageCache.size();
        }
        
        // 缓存图像
        void cacheImage(const QString& key, const QImage& image) {
            QMutexLocker locker(&m_mutex);
            
            // 限制缓存大小
            if (m_imageCache.size() >= m_maxCacheSize) {
                QString oldestKey;
                qint64 oldestTime = std::numeric_limits<qint64>::max();
                
                // 查找最旧的项
                for (auto it = m_imageCache.begin(); it != m_imageCache.end(); ++it) {
                    if (it.value().lastUsed.elapsed() > oldestTime) {
                        oldestTime = it.value().lastUsed.elapsed();
                        oldestKey = it.key();
                    }
                }
                
                // 移除最旧的项
                if (!oldestKey.isEmpty()) {
                    m_imageCache.remove(oldestKey);
                }
            }
            
            // 添加新图像
            CachedImage cachedImg;
            cachedImg.image = image;
            cachedImg.lastUsed.start();
            m_imageCache[key] = cachedImg;
        }
        
        // 获取缓存图像
        QImage getImage(const QString& key) {
            QMutexLocker locker(&m_mutex);
            
            auto it = m_imageCache.find(key);
            if (it != m_imageCache.end()) {
                // 更新使用时间
                it.value().lastUsed.restart();
                return it.value().image;
            }
            
            return QImage();
        }
        
        // 是否存在缓存图像
        bool hasImage(const QString& key) {
            QMutexLocker locker(&m_mutex);
            return m_imageCache.contains(key);
        }
        
        // 清空全部缓存
        void clearAllCache() {
            QMutexLocker locker(&m_mutex);
            m_imageCache.clear();
            qDebug() << "已清空所有图像缓存";
        }
        
    private:
        ResourceManager() : m_maxCacheSize(20) {
            // 启动清理定时器
            m_cleanupTimer.setInterval(60000); // 每分钟清理一次
            m_cleanupTimer.setSingleShot(false);
            QObject::connect(&m_cleanupTimer, &QTimer::timeout, [this]() {
                cleanupResources();
            });
            m_cleanupTimer.start();
        }
        
        ~ResourceManager() {
            m_cleanupTimer.stop();
            clearAllCache();
        }
        
        struct CachedImage {
            QImage image;
            QElapsedTimer lastUsed;
        };
            // 为ResourceManager添加清除特定缓存项的方法
    
        QMap<QString, CachedImage> m_imageCache;
        QMutex m_mutex;
        QTimer m_cleanupTimer;
        int m_maxCacheSize;
    };
    






class PDFViewerPage : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    enum class SlamMode {
        Basic,          // 基础跟踪模式 
        FeaturePoint,   // 基于特征点的SLAM
        OpticalFlow     // 基于光流和惯性的SLAM
    };
    explicit PDFViewerPage(QWidget *parent = nullptr);
    ~PDFViewerPage();

    void setupCamera();
    void startCamera();
    void stopCamera();
    
    // 设置摄像头可用状态
    void setCameraAvailable(bool available) {
        cameraAvailable = available;
        
        // 根据可用性更新UI
        if (startCameraButton) {
            startCameraButton->setEnabled(available);
        }
        
        if (statusLabel) {
            if (available) {
                statusLabel->setText("摄像头可用，点击'启动摄像头'开始");
            } else {
                statusLabel->setText("摄像头资源不可用");
            }
        }
    }
    QLabel *statusLabel;
    // 返回摄像头是否可用
    bool isCameraAvailable() const {
        return cameraAvailable;
    }

    void initializeCameraOnDemand() {
        if (!camera) {
            setupCamera();
            statusLabel->setText("摄像头已初始化，请点击'启动摄像头'开始");
        }
    }
signals:
    void backButtonClicked();

protected:
    
    
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

public slots:
    void resetDesktopDetection();
    void networkLoadPDF(const QByteArray& pdfData);
    void onBackButtonClicked();
    void processFrame(const QVideoFrame &frame);
    void nextPage();
    void prevPage();
    
    void initGyroscope();
    void processSerialData();
    void updateGyroRotation();
    QMatrix4x4 fuseCameraAndGyroData(const QMatrix4x4& cameraMatrix);
private:
    // OpenGL相关
    QOpenGLVertexArrayObject vao;
    QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLShaderProgram shaderProgram;
    void setupShaders();
    void setupVertexData();
    GLuint createTextureFromImage(const QImage& image);
    void calculateModelViewFromContour(const std::vector<cv::Point>& contour, QMatrix4x4& matrix);
    void renderPDFWithOpenGL(const QImage& pdfImage, const std::vector<cv::Point>& contour);
    
    // UI组件
    QPushButton *importButton;
    QPushButton *backButton;
    QPushButton *resetButton;
    QPushButton *nextPageButton;
    QPushButton *prevPageButton;
    
    QScrollArea *scrollArea;
    QLabel *pdfLabel;
    QVideoWidget *viewfinder;
    QLabel *processedLabel;
    QSlider *opacitySlider;

    // 摄像头相关
    QCamera *camera;
    QMediaCaptureSession captureSession;
    QVideoSink *videoSink;
    

    // PDF相关
    QPdfDocument *pdfDocument;
    int currentPage = 0;
    QImage currentPdfFrame;
    void renderCurrentPDFToImage(const QSize& targetSize);
    std::vector<cv::Point2f> pdfCorners;

    // 桌面检测与跟踪
    bool desktopDetected = false;
    bool desktopLocked = false;
    std::vector<cv::Point> lockedDesktopContour;
    bool detectDesktop(cv::Mat& frame, std::vector<cv::Point>& contour);
    void trackDesktop(cv::Mat& currentFrame, std::vector<cv::Point>& contour);
    std::vector<cv::Point2f> orderPoints(const std::vector<cv::Point>& pts);
    float computeAspectRatio(const std::vector<cv::Point>& quad);
    
    // 卡尔曼滤波相关
    cv::KalmanFilter kf;
    bool kalmanInitialized = false;
    std::vector<cv::Point2f> kalmanPoints;
    void setupKalmanFilter();
    std::vector<cv::Point2f> enforceShapeConstraints(const std::vector<cv::Point2f>& points);
    
    // 触摸控制相关
    QPoint touchStartPos;
    bool isTouching = false;
    
    // 渲染参数
    double pdfOpacity = 0.7;
    
    // 特征点跟踪
    std::vector<cv::Point2f> prevFeaturePoints;
    cv::Mat prevGray;
    
    // 用于区域选择
    bool selectionMode = false;
    QPoint selectionStart;
    QRect selectionRect;
    
    // 光照分析
    void analyzeEnvironmentLighting(const cv::Mat& frame);
    
    // 布局设置
    void setupUI();

    void startAreaSelection();
    void paintEvent(QPaintEvent *event);
    void orientationChanged(Qt::ScreenOrientation orientation);
    void initCamera();
    void enhancedDynamicOverlay(cv::Mat& frame, const std::vector<cv::Point>& contour);
    void enhancedOverlayPDF(cv::Mat& frame, const std::vector<cv::Point>& contour);
    float calculateContourInstability(const std::vector<cv::Point>& contour);
    void applyMotionSmoothing(std::vector<cv::Point>& contour);
    void overlayPDF(cv::Mat& frame, const std::vector<cv::Point>& contour);
    float calculateRotationAngle(const std::vector<cv::Point2f>& points);

     // OpenGL相关
     void initializeGL() override;
     void paintGL() override;
     void resizeGL(int w, int h) override;
     void releaseSystemCameras();
     QPushButton *startCameraButton;  // 手动启动摄像头按钮
    QPushButton *stopCameraButton;   // 手动停止摄像头按钮
    
    

    bool cameraAvailable = false;

    bool deskTracking;              // 是否在跟踪桌面
    cv::Mat lastHomography;         // 上一次的变换矩阵
    cv::Rect lastDeskBoundingRect;  // 上一次桌面的边界矩形
    QMatrix4x4 worldToCamera;       // 世界坐标到摄像头坐标的变换矩阵
    std::vector<cv::Point3f> deskCorners3D; // 桌面在世界坐标系中的3D角点
    bool deskInitialized;           // 桌面是否已初始化3D位置
    int framesWithoutDesk;          // 连续没有检测到桌面的帧数

    // 添加新函数
    void initializeDesk3DPosition(const std::vector<cv::Point>& contour);
    bool isDeskStillVisible( cv::Mat& frame);
    void updateDeskPosition(const std::vector<cv::Point>& contour);
    SlamMode slamMode = SlamMode::Basic;  // 默认使用基础跟踪模式

    // 录制和回放相关
    bool isRecording = false;
    std::vector<cv::Mat> recordingFrames;
    std::vector<std::vector<cv::Point>> recordingContours;
    int playbackFrame = 0;
    QTimer *playbackTimer = nullptr;
    
    // 录制和回放相关函数
    void saveRecording();
    void loadRecording();
    void startPlayback();
     // SLAM相关跟踪函数
     void trackDesktopWithFeatureSLAM(cv::Mat& frame, std::vector<cv::Point>& contour);
     void trackDesktopWithOpticalFlowSLAM(cv::Mat& frame, std::vector<cv::Point>& contour);
     float initialAspectRatio = 1.0f;  // 假设初始宽高比为1:1


     // ArUco相关变量
    cv::Ptr<cv::aruco::Dictionary> arucoDict;
    cv::Ptr<cv::aruco::DetectorParameters> arucoParams;
    bool useArUcoTracking = true;  // 控制是否使用ArUco跟踪
    QCheckBox* enableArucoCheckbox;  // UI控制组件

    // 最后一次检测到的标记信息
    std::vector<int> markerIds;
    std::vector<std::vector<cv::Point2f>> markerCorners;
    std::vector<cv::Point2f> lastValidCorners;  // 存储最后一组有效的角点
    QElapsedTimer markerLostTimer;  // 用于处理标记暂时丢失的情况
    
    // ArUco相关方法
    void initArUcoDetector();
    bool detectDesktopWithArUco(cv::Mat& frame, std::vector<cv::Point>& deskContour);
    bool isValidDeskConfiguration(const std::vector<cv::Point2f>& corners);
    void estimateDeskFromValidMarkers(const std::vector<int>& ids, 
                                    const std::vector<std::vector<cv::Point2f>>& corners,
                                    std::vector<cv::Point>& deskContour);
    void generateAndShowArUcoMarkers();


    ArUcoProcessorThread* m_arucoProcessor;
    QMutex m_renderMutex;

    void monitorMemoryUsage();
    void handleDetectedMarkers(const std::vector<int>& ids,
        const std::vector<std::vector<cv::Point2f>>& corners,
        const cv::Mat& processedImage);

        void optimizeCameraSettings();

    // PDF页面缓存
    QMap<int, QImage> pdfPageCache;
    QMutex pdfCacheMutex;
    int maxCacheSize;
    int lastRequestedPage;

    void preloadAdjacentPages();


    // 陀螺仪数据相关
    QSerialPort *serialPort;
    float gyroRoll, gyroPitch, gyroYaw;  // 当前陀螺仪角度
    float lastGyroRoll, lastGyroPitch, lastGyroYaw;  // 上一次陀螺仪角度
    QElapsedTimer gyroUpdateTimer;  // 陀螺仪更新计时器
    bool gyroAvailable;  // 陀螺仪是否可用
    QVector3D gyroAngularVelocity;  // 角速度
    
    // 融合跟踪相关
    QMatrix4x4 gyroRotationMatrix;  // 基于陀螺仪的旋转矩阵
    float gyroVisualWeight;  // 视觉和陀螺仪权重平衡系数(0-1)


    bool m_useThreadPool = true;               // 控制是否使用线程池
    std::atomic<int> m_pendingTasks;           // 待处理任务计数
    QMutex m_frameQueueMutex;                  // 帧队列互斥锁
    QWaitCondition m_frameProcessedCondition;  // 帧处理完成条件变量
    QQueue<cv::Mat> m_processedFrames;         // 已处理帧队列
    QElapsedTimer m_frameProcessTimer;         // 帧处理计时器
    int m_processingTimeThreshold = 30;        // 处理时间阈值(毫秒)

    // 线程池相关方法
    void processFrameInThreadPool(const QVideoFrame &frame);
    void handleProcessedFrame(const cv::Mat& processedFrame);
    void detectDesktopInThread(const cv::Mat& frame);
    void trackDesktopInThread(cv::Mat& currentFrame);
    void pdfOverlayInThread(cv::Mat& frame);
    
    // 高分辨率帧处理
    void processHighResFrame(const cv::Mat& frame);
    void processLowResFrame(const cv::Mat& frame);
    
    // 自适应处理控制
    void adjustProcessingQuality();
    
    // 帧率和性能监控
    QQueue<qint64> m_frameTimes;               // 存储最近帧处理时间
    int m_frameTimeWindowSize = 30;            // 帧时间窗口大小
    double m_currentFps = 0.0;                 // 当前帧率
    bool m_lowPerformanceMode = false;         // 低性能模式标志
    
    // PDFViewerPage类中添加的UI控制
    QCheckBox* m_useThreadPoolCheckbox;        // 线程池开关
    QLabel* m_performanceLabel;                // 性能指标显示
};

#endif // PDFVIEWERPAGE_H


// 陀螺仪数据的作用
// 陀螺仪数据在增强现实系统中具有多重重要作用：

// 快速响应姿态变化：陀螺仪提供设备方向的高频率测量（通常100-200Hz），远高于相机帧率
// 跟踪补偿：在标记暂时遮挡或光线条件不佳时，可以保持姿态跟踪的连续性
// 抖动抑制：结合视觉跟踪和IMU数据可以降低轻微手部抖动的影响
// 预测性跟踪：可以预测摄像头的下一个位置，提高渲染准确性
// 加速全局位置估计：与视觉SLAM结合时，能极大提高系统精度