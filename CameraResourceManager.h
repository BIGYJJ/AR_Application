// CameraResourceManager.h
#ifndef CAMERARESOURCEMANAGER_H
#define CAMERARESOURCEMANAGER_H

#include <QObject>
#include <QMutex>
#include <QProcess>
#include <QTimer>
#include <QMap>
#include <QQueue>
#include <QCamera>
#include <QMediaDevices>
#include <QFile>
#include <QCoreApplication>  // 添加这行获取应用程序PID
// 摄像头资源使用状态
enum class CameraState {
    Available,      // 摄像头可用
    InUse,          // 摄像头正在使用
    Error,          // 摄像头错误
    NotFound        // 摄像头不存在
};

// 摄像头资源请求优先级
enum class RequestPriority {
    Low,            // 低优先级（如非关键功能）
    Normal,         // 正常优先级（大多数功能）
    High,           // 高优先级（关键功能）
    Critical        // 最高优先级（可抢占其他功能）
};

// 摄像头请求结构
struct CameraRequest {
    QString requesterId;    // 请求者ID
    RequestPriority priority;  // 优先级
    int preferredCameraIndex;  // 首选摄像头索引
    bool exclusive;            // 是否需要独占访问
    QObject* notifyTarget;     // 通知目标对象
    QString notifyMethod;      // 通知方法名
};

class CameraResourceManager : public QObject
{ 
    Q_OBJECT  // 添加这行

public:
    // 获取单例实例
    static CameraResourceManager& instance() {
        static CameraResourceManager instance;
        return instance;
    }
    
    // 请求摄像头资源
    bool requestCamera(const CameraRequest& request);
    
    // 释放摄像头资源
    bool releaseCamera(const QString& requesterId);
    
    // 获取摄像头状态
    CameraState getCameraState(int cameraIndex = -1);
    
    // 查找可用摄像头
    int findAvailableCamera();
    
    // 获取当前摄像头用户
    QString getCurrentUser();
    
    // 获取摄像头错误原因
    QString getLastError();
    
    // 强制重置所有摄像头状态
    bool resetAllCameras();
    
    QMap<int, QString> getCameraUsers() const {
        QMutexLocker locker(&m_mutex);
        return m_cameraUsers;
    }

    CameraResourceManager(QObject* parent = nullptr);
    ~CameraResourceManager();
    
    // 处理请求队列
    void processRequestQueue();
    
    // 尝试分配摄像头
    bool tryAllocateCamera(const CameraRequest& request);
    
    // 释放指定摄像头资源
    bool releaseCameraResource(int cameraIndex);
    
    // 检查摄像头可用性
    bool isCameraAvailable(int cameraIndex);
    
    // 强制释放摄像头资源
    bool forceReleaseCamera(int cameraIndex);
    
    // 系统级检查摄像头状态
    CameraState checkSystemCameraState(int cameraIndex);

signals:
    // 摄像头状态变化信号
    void cameraStateChanged(int cameraIndex, CameraState state);
    
    // 摄像头资源分配完成信号
    void cameraAllocated(const QString& requesterId, int cameraIndex, bool success);
    
    // 摄像头资源被抢占信号
    void cameraPreempted(const QString& requesterId);

private:
    
    
private:
    mutable QMutex m_mutex;
    QMap<int, QString> m_cameraUsers;  // 摄像头索引 -> 使用者ID
    QQueue<CameraRequest> m_requestQueue;  // 请求队列
    QString m_lastError;  // 最后一次错误信息
    QTimer* m_monitorTimer;  // 监控定时器
    
    // 禁止复制
    CameraResourceManager(const CameraResourceManager&) = delete;
    CameraResourceManager& operator=(const CameraResourceManager&) = delete;
};

#endif // CAMERARESOURCEMANAGER_H