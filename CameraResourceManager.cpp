// CameraResourceManager.cpp
#include "CameraResourceManager.h"
#include <QDebug>
#include <QThread>
#include <QMetaMethod>

CameraResourceManager::CameraResourceManager(QObject* parent)
    : QObject(parent)
    , m_monitorTimer(new QTimer(this))
{
    // 设置监控定时器，定期检查摄像头状态
    connect(m_monitorTimer, &QTimer::timeout, this, [this]() {
        QMutexLocker locker(&m_mutex);
        
        // 检查所有已分配摄像头是否仍然正常工作
        for (auto it = m_cameraUsers.begin(); it != m_cameraUsers.end(); ) {
            int cameraIndex = it.key();
            QString userId = it.value();
            
            CameraState state = checkSystemCameraState(cameraIndex);
            if (state != CameraState::InUse && state != CameraState::Available) {
                // 摄像头可能已经崩溃或被外部进程抢占
                qWarning() << "CameraResourceManager: 摄像头" << cameraIndex 
                           << "状态异常:" << (int)state << "，当前用户:" << userId;
                
                // 发送抢占信号
                emit cameraPreempted(userId);
                
                // 从用户映射中移除
                it = m_cameraUsers.erase(it);
            } else {
                ++it;
            }
        }
    });
    
    // 启动监控定时器，每5秒检查一次
    m_monitorTimer->start(5000);
    
    qDebug() << "CameraResourceManager: 摄像头资源管理器已初始化";
}

CameraResourceManager::~CameraResourceManager()
{
    m_monitorTimer->stop();
    
    // 释放所有摄像头资源
    resetAllCameras();
}

bool CameraResourceManager::requestCamera(const CameraRequest& request)
{
    QMutexLocker locker(&m_mutex);
    
    // 如果没有指定首选摄像头，将0设为首选
    int preferredIndex = request.preferredCameraIndex;
    if (preferredIndex < 0) {
        preferredIndex = 0;
    }
    
    // Log the request
    qDebug() << "CameraResourceManager: 收到来自" << request.requesterId 
             << "的摄像头请求，优先级:" << (int)request.priority
             << "，首选索引:" << request.preferredCameraIndex;
    
    // Check if already owns a camera
    QString existingCamera;
    for (auto it = m_cameraUsers.begin(); it != m_cameraUsers.end(); ++it) {
        if (it.value() == request.requesterId) {
            existingCamera = QString::number(it.key());
        }
    }
    
    if (!existingCamera.isEmpty()) {
        qDebug() << "CameraResourceManager: " << request.requesterId 
                 << "已经拥有摄像头" << existingCamera;
        return true;
    }
    
    // First perform explicit release of any previously held cameras
    // to avoid potential "ghost" ownerships
    for (auto it = m_cameraUsers.begin(); it != m_cameraUsers.end();) {
        if (it.value() == request.requesterId) {
            int cameraToRelease = it.key();
            it = m_cameraUsers.erase(it);
            releaseCameraResource(cameraToRelease);
        } else {
            ++it;
        }
    }
    
    // 添加安全检查 - 防止重复释放同一设备导致系统不稳定
    static QSet<QString> pendingReleaseRequests;
    QString requestKey = request.requesterId + QString::number(request.preferredCameraIndex);
    
    if (pendingReleaseRequests.contains(requestKey)) {
        qWarning() << "CameraResourceManager: 检测到重复请求" << requestKey << "，防止资源冲突";
        return false;
    }
    
    // 尝试直接分配摄像头
    if (tryAllocateCamera(request)) {
        return true;
    }
    
    // 如果是高优先级请求，考虑抢占
    if (request.priority == RequestPriority::Critical) {
        // 找出最低优先级的当前用户
        QString lowestPriorityUser;
        int targetCameraIndex = -1;
        
        for (auto it = m_cameraUsers.begin(); it != m_cameraUsers.end(); ++it) {
            // 这里简化处理，实际应该根据用户优先级决定
            lowestPriorityUser = it.value();
            targetCameraIndex = it.key();
            break;
        }
        
        if (!lowestPriorityUser.isEmpty()) {
            pendingReleaseRequests.insert(requestKey);
            
            // 通知被抢占
            emit cameraPreempted(lowestPriorityUser);
            
            // 释放摄像头
            if (releaseCameraResource(targetCameraIndex)) {
                // 重新尝试分配
                bool result = tryAllocateCamera(request);
                pendingReleaseRequests.remove(requestKey);
                return result;
            }
            
            pendingReleaseRequests.remove(requestKey);
        }
    }
    
    // 如果仍然无法分配，将请求加入队列
    m_requestQueue.enqueue(request);
    
    qDebug() << "CameraResourceManager: 请求已加入队列，当前队列长度:" << m_requestQueue.size();
    return false;
}

bool CameraResourceManager::releaseCamera(const QString& requesterId)
{
    QMutexLocker locker(&m_mutex);
    
    qDebug() << "CameraResourceManager: 收到来自" << requesterId << "的摄像头释放请求";
    
    bool released = false;
    
    // 释放该用户占用的所有摄像头
    for (auto it = m_cameraUsers.begin(); it != m_cameraUsers.end(); ) {
        if (it.value() == requesterId) {
            int cameraIndex = it.key();
            
            // 先从映射中移除
            it = m_cameraUsers.erase(it);
            
            // 释放摄像头资源
            if (releaseCameraResource(cameraIndex)) {
                released = true;
                qDebug() << "CameraResourceManager: 已释放" << requesterId 
                         << "使用的摄像头" << cameraIndex;
            } else {
                qWarning() << "CameraResourceManager: 释放" << requesterId 
                           << "使用的摄像头" << cameraIndex << "失败";
            }
        } else {
            ++it;
        }
    }
    
    // 如果有请求在队列中，也一并移除
    for (int i = 0; i < m_requestQueue.size(); ) {
        if (m_requestQueue[i].requesterId == requesterId) {
            m_requestQueue.removeAt(i);
        } else {
            ++i;
        }
    }
    
    // 处理等待队列
    if (!m_requestQueue.isEmpty()) {
        processRequestQueue();
    }
    
    return released;
}

CameraState CameraResourceManager::getCameraState(int cameraIndex)
{
    QMutexLocker locker(&m_mutex);
    
    // 如果未指定索引，返回第一个摄像头的状态
    if (cameraIndex < 0) {
        // 查找第一个可用摄像头
        for (int i = 0; i <= 2; ++i) {
            CameraState state = checkSystemCameraState(i);
            if (state != CameraState::NotFound) {
                return state;
            }
        }
        return CameraState::NotFound;
    }
    
    return checkSystemCameraState(cameraIndex);
}

int CameraResourceManager::findAvailableCamera()
{
    QMutexLocker locker(&m_mutex);
    // 优先检查video0是否可用
    if (checkSystemCameraState(0) == CameraState::Available) {
        return 0;
    }
    // 先检查Qt API能否检测到摄像头
    const auto cameras = QMediaDevices::videoInputs();
    
    // 打印所有摄像头信息
    for (int i = 0; i < cameras.size(); i++) {
        qDebug() << "摄像头" << i << ":" << cameras[i].description();
    }
    
    // 尝试特定索引
    QList<int> priorityIndices = {0, 1, 2};
    foreach (int index, priorityIndices) {
        if (checkSystemCameraState(index) == CameraState::Available) {
            return index;
        }
    }
    
    // 如果特定索引都不可用，尝试使用系统脚本
    QProcess process;
    process.start("/mnt/tsp/camera_toggle.sh", QStringList() << "find");
    
    if (process.waitForFinished(5000) && process.exitCode() == 0) {
        QString output = process.readAllStandardOutput().trimmed();
        bool ok;
        int cameraIndex = output.toInt(&ok);
        
        if (ok && cameraIndex >= 0) {
            return cameraIndex;
        }
    }
    
    // 如果都失败，返回默认值
    return 0;
}

QString CameraResourceManager::getCurrentUser()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_cameraUsers.isEmpty()) {
        return QString();
    }
    
    // 返回第一个找到的用户
    return m_cameraUsers.first();
}

QString CameraResourceManager::getLastError()
{
    return m_lastError;
}

bool CameraResourceManager::resetAllCameras()
{
    QMutexLocker locker(&m_mutex);
    
    qDebug() << "CameraResourceManager: 正在重置所有摄像头状态";
    
    // 清空用户映射
    QStringList users = m_cameraUsers.values();
    m_cameraUsers.clear();
    
    // 通知所有用户被抢占
    for (const QString& user : users) {
        emit cameraPreempted(user);
    }
    
    // 清空请求队列
    m_requestQueue.clear();
    
    // 强制释放所有摄像头
    bool allSuccess = true;
    for (int i = 0; i <= 2; ++i) {
        if (!forceReleaseCamera(i)) {
            allSuccess = false;
        }
    }
    
    return allSuccess;
}

void CameraResourceManager::processRequestQueue()
{
    // 注意：此方法假设已持有互斥锁
    
    if (m_requestQueue.isEmpty()) {
        return;
    }
    
    // 按优先级对队列排序
    std::sort(m_requestQueue.begin(), m_requestQueue.end(), 
              [](const CameraRequest& a, const CameraRequest& b) {
                  return static_cast<int>(a.priority) > static_cast<int>(b.priority);
              });
    
    // 尝试处理队列中的请求
    while (!m_requestQueue.isEmpty()) {
        CameraRequest request = m_requestQueue.dequeue();
        
        if (tryAllocateCamera(request)) {
            qDebug() << "CameraResourceManager: 成功从队列中分配摄像头给" << request.requesterId;
            // 成功分配，退出循环
            break;
        } else {
            qDebug() << "CameraResourceManager: 无法从队列中分配摄像头给" << request.requesterId;
            
            // 如果请求者设置了通知回调，通知失败
            if (request.notifyTarget && !request.notifyMethod.isEmpty()) {
                QMetaObject::invokeMethod(request.notifyTarget, request.notifyMethod.toUtf8().constData(),
                                        Qt::QueuedConnection, Q_ARG(bool, false));
            }
        }
    }
}

bool CameraResourceManager::tryAllocateCamera(const CameraRequest& request)
{
    // 注意：此方法假设已持有互斥锁
    
    int cameraIndex = request.preferredCameraIndex;
    
    // 如果指定了索引，先尝试该索引
    if (cameraIndex >= 0) {
        // 检查该索引是否已被占用
        if (!m_cameraUsers.contains(cameraIndex)) {
            // 检查该摄像头是否可用
            if (isCameraAvailable(cameraIndex)) {
                m_cameraUsers[cameraIndex] = request.requesterId;
                
                // 发送分配成功信号
                emit cameraAllocated(request.requesterId, cameraIndex, true);
                
                // 如果请求者设置了通知回调，通知成功
                if (request.notifyTarget && !request.notifyMethod.isEmpty()) {
                    QMetaObject::invokeMethod(request.notifyTarget, request.notifyMethod.toUtf8().constData(),
                                            Qt::QueuedConnection, Q_ARG(bool, true), Q_ARG(int, cameraIndex));
                }
                
                return true;
            }
        }
    }
    
    // 如果指定索引不可用，尝试其他索引
    for (int i = 0; i <= 2; ++i) {
        if (i == cameraIndex) continue;  // 跳过已经尝试过的索引
        
        // 检查该索引是否已被占用
        if (!m_cameraUsers.contains(i)) {
            // 检查该摄像头是否可用
            if (isCameraAvailable(i)) {
                m_cameraUsers[i] = request.requesterId;
                
                // 发送分配成功信号
                emit cameraAllocated(request.requesterId, i, true);
                
                // 如果请求者设置了通知回调，通知成功
                if (request.notifyTarget && !request.notifyMethod.isEmpty()) {
                    QMetaObject::invokeMethod(request.notifyTarget, request.notifyMethod.toUtf8().constData(),
                                            Qt::QueuedConnection, Q_ARG(bool, true), Q_ARG(int, i));
                }
                
                return true;
            }
        }
    }
    
    return false;
}

bool CameraResourceManager::releaseCameraResource(int cameraIndex)
{
    qDebug() << "CameraManager: 尝试释放系统摄像头资源";
    
    // 强制终止所有可能使用摄像头的Python进程
    QProcess::execute("pkill", QStringList() << "-f" << "gesture_recognizer.py");
    QProcess::execute("pkill", QStringList() << "-f" << "python.*opencv");
    
    // 等待进程终止
    QThread::msleep(1000);
    
    // 尝试所有可能的设备索引
    QStringList indexesToTry = {"0", "1", "2"};
    bool anySuccess = false;
    
    foreach (const QString &index, indexesToTry) {
        // 1. 使用系统脚本
        QProcess process;
        process.start("/mnt/tsp/camera_toggle.sh", QStringList() << "release" << index);
        process.waitForFinished(2000);
        
        // 2. 使用fuser命令强制释放
        QProcess fuserProcess;
        fuserProcess.start("sudo", QStringList() << "fuser" << "-k" << 
                         QString("/dev/video%1").arg(index));
        fuserProcess.waitForFinished(2000);
        
        QThread::msleep(500);  // 给系统一些时间释放资源
        
        // 检查是否释放成功
        QProcess checkProcess;
        checkProcess.start("fuser", QStringList() << QString("/dev/video%1").arg(index));
        checkProcess.waitForFinished(1000);
        
        if (checkProcess.exitCode() != 0) {
            anySuccess = true;  // 没有进程使用该设备
        }
    }
    
    return anySuccess;  // 如果任何一个设备释放成功就返回true
}

bool CameraResourceManager::isCameraAvailable(int cameraIndex)
{
    CameraState state = checkSystemCameraState(cameraIndex);
    return state == CameraState::Available;
}

bool CameraResourceManager::forceReleaseCamera(int cameraIndex)
{
    qDebug() << "CameraResourceManager: 强制释放摄像头" << cameraIndex;
    
    // 1. 首先检查设备是否存在
    QString devicePath = QString("/dev/video%1").arg(cameraIndex);
    QFile deviceFile(devicePath);
    if (!deviceFile.exists()) {
        qWarning() << "CameraResourceManager: 设备不存在" << devicePath;
        return false;
    }
    
    bool success = false;  // 添加此声明以修复编译错误
    
    // 2. 首先获取当前进程ID
    qint64 currentPid = QCoreApplication::applicationPid();
    qDebug() << "当前进程ID:" << currentPid;
    
    // 3. 检查哪些进程在使用摄像头
    QProcess checkProcess;
    checkProcess.start("fuser", QStringList() << devicePath);
    checkProcess.waitForFinished(2000);
    QString pidList = checkProcess.readAllStandardOutput().trimmed();
    
    if (!pidList.isEmpty()) {
        // 检查是否只有当前进程在使用摄像头
        QStringList pids = pidList.split(' ', Qt::SkipEmptyParts);
        bool onlySelfUsing = true;
        
        for (const QString &pidStr : pids) {
            bool ok;
            qint64 pid = pidStr.toLongLong(&ok);
            if (ok && pid != currentPid) {
                onlySelfUsing = false;
                break;
            } else if (ok && pid == currentPid) {
                qDebug() << "跳过自身进程ID:" << pid;
            }
        }
        
        // 如果只有当前进程使用，则认为摄像头已释放
        if (onlySelfUsing) {
            qDebug() << "CameraResourceManager: 只有当前进程在使用摄像头，视为已释放";
            return true;
        }
        
        // 过滤掉自己的进程ID
        QStringList otherPids;
        for (const QString &pidStr : pids) {
            bool ok;
            qint64 pid = pidStr.toLongLong(&ok);
            if (ok && pid != currentPid) {
                otherPids.append(pidStr);
            }
        }
        
        // 只杀死其他进程
        if (!otherPids.isEmpty()) {
            qDebug() << "正在终止其他进程:" << otherPids;
            QProcess killProcess;
            killProcess.start("sudo", QStringList() << "kill" << otherPids);
            killProcess.waitForFinished(3000);
            
            // 如果普通终止失败，使用强制终止
            QThread::msleep(1000);
            checkProcess.start("fuser", QStringList() << devicePath);
            checkProcess.waitForFinished(2000);
            QString remainingPids = checkProcess.readAllStandardOutput().trimmed();
            
            if (!remainingPids.isEmpty()) {
                qDebug() << "尝试强制终止剩余进程";
                QProcess forceKillProcess;
                forceKillProcess.start("sudo", QStringList() << "kill" << "-9" << otherPids);
                forceKillProcess.waitForFinished(3000);
            }
        }
    }
    
    // 4. 使用脚本尝试释放资源（但不终止进程）
    QProcess releaseProcess;
    releaseProcess.start("/mnt/tsp/camera_toggle.sh", QStringList() << "release" << QString::number(cameraIndex));
    if (releaseProcess.waitForFinished(3000)) {
        qDebug() << "脚本释放结果:" << releaseProcess.readAllStandardOutput();
        success = true;
    }
    
    // 5. 检查最终状态
    CameraState state = checkSystemCameraState(cameraIndex);
    emit cameraStateChanged(cameraIndex, state);
    
    return state == CameraState::Available || success;
}

CameraState CameraResourceManager::checkSystemCameraState(int cameraIndex)
{
     // 首先检查设备文件是否存在
     QString devicePath = QString("/dev/video%1").arg(cameraIndex);
     QFile deviceFile(devicePath);
     if (!deviceFile.exists()) {
         return CameraState::NotFound;
     }
     
     // 添加: 检查是否为有效摄像头（仅在系统中存在的实际摄像头）
     if (cameraIndex != 0) {  // 假设系统中只有video0是有效摄像头
         qDebug() << "CameraResourceManager: 摄像头" << cameraIndex << "不是有效摄像头";
         return CameraState::NotFound;
     }
    
    // 2. 检查设备是否被占用
    QProcess fuserProcess;
    fuserProcess.start("fuser", QStringList() << QString("/dev/video%1").arg(cameraIndex));
    fuserProcess.waitForFinished(1000);
    
    QString output = fuserProcess.readAllStandardOutput().trimmed();
    if (!output.isEmpty()) {
        // 设备被占用
        // 检查是否被当前进程占用
        if (m_cameraUsers.contains(cameraIndex)) {
            return CameraState::InUse;
        } else {
            return CameraState::Error;  // 被外部进程占用
        }
    }
    
    // 3. 尝试打开设备
    QProcess testProcess;
    testProcess.start("v4l2-ctl", QStringList() << "--device=/dev/video" + QString::number(cameraIndex) << "--all");
    testProcess.waitForFinished(1000);
    
    if (testProcess.exitCode() != 0) {
        return CameraState::Error;  // 设备无法打开
    }
    
    return CameraState::Available;
}