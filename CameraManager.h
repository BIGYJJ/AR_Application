#ifndef CAMERAMANAGER_H
#define CAMERAMANAGER_H

#include <QObject>
#include <QCamera>
#include <QMediaDevices>
#include <QProcess>
#include <QDebug>
#include <QThread>
#include <QUdpSocket>
#include <QHostAddress>

class CameraManager : public QObject
{
    Q_OBJECT
    
public:
    explicit CameraManager(QObject *parent = nullptr);
    ~CameraManager();
    
    // 释放系统摄像头资源
    bool releaseSystemCamera();
    
    // 检查摄像头是否可用
    bool isCameraAvailable();

    
    // 强制关闭使用摄像头的进程
    void forceKillCameraProcesses();
    
    // 获取系统摄像头设备信息
    QList<QCameraDevice> getAvailableCameras();

    int findAvailableCamera(); // 查找可用摄像头
    bool tryCamera(int index); // 测试特定索引的摄像头
    // 尝试使用Qt API检查摄像头
    bool testCameraAccess(int preferredInde=1);

    bool checkCameraProcesses();
private:
    
    
    // 清理可能残留的进程（例如Python手势识别进程）
    void cleanupGestureRecognizer();
};

#endif // CAMERAMANAGER_H