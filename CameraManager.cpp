#include "CameraManager.h"
#include "CameraResourceManager.h"  // 添加这一行
CameraManager::CameraManager(QObject *parent) : QObject(parent)
{
    qDebug() << "CameraManager: 初始化";
}

CameraManager::~CameraManager()
{
    // 确保资源被释放
    releaseSystemCamera();
}

bool CameraManager::releaseSystemCamera()
{
   // First, gracefully stop services
    cleanupGestureRecognizer();
    
    // Add short delay
    QThread::msleep(500);
    
    // Only target specific processes, not using sudo
    QProcess::execute("pkill", QStringList() << "-f" << "gesture_recognizer.py");
    QProcess::execute("pkill", QStringList() << "-f" << "python.*opencv");
    
    // Use a less aggressive script call without sudo
    QProcess process;
    process.start("/mnt/tsp/camera_toggle.sh", QStringList() << "release" << "0");
    
    if (!process.waitForFinished(3000)) {
        qWarning() << "Camera release script timeout";
        return false;
    }
    
    // Add delay to let the system catch up
    QThread::msleep(500);
    
    // Check if camera is actually available
    return isCameraAvailable();
}

// 添加新方法检查摄像头进程
bool CameraManager::checkCameraProcesses()
{
    QProcess checkProcess;
    checkProcess.start("sudo", QStringList() << "fuser" << "-v" << "/dev/video*");
    checkProcess.waitForFinished(2000);
    QString output = checkProcess.readAllStandardOutput();
    
    // 如果输出不为空，表示有进程在使用摄像头
    return !output.trimmed().isEmpty();
}


bool CameraManager::isCameraAvailable()
{
    return testCameraAccess(-1);
}

void CameraManager::forceKillCameraProcesses()
{
    qDebug() << "CameraManager: 强制释放摄像头资源";
    
    // 1. 终止手势识别Python进程
    QProcess::execute("pkill", QStringList() << "-f" << "gesture_recognizer.py");
    
    // 2. 尝试停止可能使用摄像头的Qt进程
    // 这里需要谨慎处理，避免影响其他摄像头进程
    QProcess::execute("pkill", QStringList() << "-f" << "python.*opencv");
    
    // 3. 尝试使用系统级命令解除设备占用
    QProcess::execute("/mnt/tsp/camera_toggle.sh", QStringList() << "release" << "0");
    QProcess::execute("/mnt/tsp/camera_toggle.sh", QStringList() << "release" << "1");
    
    // 等待进程终止
    QThread::msleep(800);
    
    qDebug() << "CameraManager: 强制释放完成";
}

QList<QCameraDevice> CameraManager::getAvailableCameras()
{
    return QMediaDevices::videoInputs();
}

bool CameraManager::testCameraAccess(int preferredIndex)
{
    qDebug() << "CameraManager: 测试摄像头访问，首选索引:" << preferredIndex;
    
    // 1. 首先尝试首选摄像头
    if (preferredIndex >= 0 && tryCamera(preferredIndex)) {
        return true;
    }
    
    // 2. 如果未指定索引或指定索引不可用，通过系统脚本查找可用摄像头
    int availableIndex = findAvailableCamera();
    if (availableIndex >= 0 && (preferredIndex < 0 || availableIndex != preferredIndex)) {
        qDebug() << "CameraManager: 尝试备用摄像头索引:" << availableIndex;
        return tryCamera(availableIndex);
    }
    
    // 3. 最后尝试使用默认方式搜索
    qDebug() << "CameraManager: 尝试枚举所有摄像头";
    bool success = false;
    QCamera *testCamera = nullptr;
    
    try {
        // 获取可用摄像头列表
        const auto cameras = QMediaDevices::videoInputs();
        if (cameras.isEmpty()) {
            qWarning() << "CameraManager: 没有找到摄像头设备";
            return false;
        }
        
        // 依次尝试每个摄像头
        for (const auto& cameraDevice : cameras) {
            try {
                testCamera = new QCamera(cameraDevice);
                testCamera->start();
                
                // 等待短暂时间
                QThread::msleep(200);
                
                // 检查是否成功启动
                if (testCamera->isActive()) {
                    success = true;
                    qDebug() << "CameraManager: 成功访问摄像头:" << cameraDevice.description();
                    break;
                }
                
                // 清理当前测试
                testCamera->stop();
                delete testCamera;
                testCamera = nullptr;
            } catch (...) {
                // 忽略单个摄像头的错误，继续尝试下一个
                if (testCamera) {
                    delete testCamera;
                    testCamera = nullptr;
                }
            }
        }
        
    } catch (const std::exception& e) {
        qWarning() << "CameraManager: 测试摄像头访问时发生异常:" << e.what();
        success = false;
    } catch (...) {
        qWarning() << "CameraManager: 测试摄像头访问时发生未知异常";
        success = false;
    }
    
    // 清理资源
    if (testCamera) {
        testCamera->stop();
        delete testCamera;
        testCamera = nullptr;
    }
    
    qDebug() << "CameraManager: 摄像头访问测试结果:" << (success ? "可用" : "不可用");
    return success;
}

void CameraManager::cleanupGestureRecognizer()
{
    qDebug() << "CameraManager: 清理手势识别器";
    
    // 1. 尝试通过UDP发送EXIT命令优雅关闭
    try {
        QUdpSocket exitSocket;
        QByteArray datagram = "EXIT";
        exitSocket.writeDatagram(datagram, QHostAddress::LocalHost, 12345);
        qDebug() << "CameraManager: 已发送EXIT命令到手势识别程序";
        
        // 等待短暂时间确保命令被接收
        QThread::msleep(500);
    } catch (...) {
        qWarning() << "CameraManager: 发送EXIT命令失败";
    }
    
    // 2. 检查进程是否还在运行
    QProcess checkProcess;
checkProcess.start("sudo", QStringList() << "fuser" << "-v" << "/dev/video*");
checkProcess.waitForFinished(2000);
QString output = checkProcess.readAllStandardOutput();
qDebug() << "摄像头使用情况：" << output;
    
    if (checkProcess.exitCode() == 0) {
        // 进程仍在运行，强制终止
        QProcess::execute("pkill", QStringList() << "-f" << "gesture_recognizer.py");
        QThread::msleep(500);  // 等待进程终止
    }
}


int CameraManager::findAvailableCamera()
{
    qDebug() << "CameraManager: 尝试查找可用摄像头";
    
    // 优先使用 video0
    if (tryCamera(0)) {
        qDebug() << "CameraManager: 找到可访问的摄像头索引: 0";
        return 0;
    }
    
    // 原有的检测逻辑作为备选
    const auto cameras = QMediaDevices::videoInputs();
    qDebug() << "检测到" << cameras.size() << "个摄像头";
    
    // 打印所有摄像头信息
    for (int i = 0; i < cameras.size(); i++) {
        qDebug() << "摄像头" << i << ":" << cameras[i].description();
    }
    
    if (cameras.isEmpty()) {
        qWarning() << "CameraManager: 未检测到摄像头，尝试使用脚本查找";
    } else {
        // 检查每个摄像头是否可访问 (除了0，因为已经检查过了)
        for (int i = 1; i < cameras.size(); i++) {
            if (tryCamera(i)) {
                qDebug() << "CameraManager: 找到可访问的摄像头索引:" << i;
                return i;
            }
        }
    }
    
    // 其他查找逻辑保持不变...
    
    qWarning() << "CameraManager: 未找到可用摄像头，返回默认索引0";
    return 0;  // 始终返回默认值0
}


bool CameraManager::tryCamera(int index)
{
    qDebug() << "CameraManager: 尝试访问摄像头索引:" << index;
    
    // 首先检查资源管理器中的状态
    auto& resourceManager = CameraResourceManager::instance();
    if (resourceManager.getCameraState(index) != CameraState::Available) {
        qDebug() << "CameraManager: 摄像头" << index << "当前不可用 (被其他进程使用)";
        return false;
    }
    
    bool success = false;
    QCamera *testCamera = nullptr;
    
    try {
        // 获取所有摄像头
        const auto cameras = QMediaDevices::videoInputs();
        if (cameras.isEmpty()) {
            qWarning() << "CameraManager: 没有找到摄像头设备";
            return false;
        }
        
        // 找到指定索引的摄像头
        QCameraDevice requestedCamera;
        if (index >= 0 && index < cameras.size()) {
            requestedCamera = cameras[index];
        } else {
            qWarning() << "CameraManager: 指定索引超出范围，使用默认摄像头";
            if (!cameras.isEmpty())
                requestedCamera = cameras.first();
            else
                return false;
        }
        
        // 尝试创建并启动摄像头
        testCamera = new QCamera(requestedCamera);
        testCamera->start();
        
        // 等待一小段时间确认摄像头是否真的启动
        QThread::msleep(300);
        
        // 检查摄像头状态
        success = testCamera->isActive();
        
        // 无论结果如何，停止并释放测试摄像头
        testCamera->stop();
        
    } catch (const std::exception& e) {
        qWarning() << "CameraManager: 测试摄像头访问时发生异常:" << e.what();
        success = false;
    } catch (...) {
        qWarning() << "CameraManager: 测试摄像头访问时发生未知异常";
        success = false;
    }
    
    // 清理资源
    if (testCamera) {
        delete testCamera;
        testCamera = nullptr;
    }
    
    qDebug() << "CameraManager: 摄像头" << index << "访问测试结果:" << (success ? "可用" : "不可用");
    return success;
}


