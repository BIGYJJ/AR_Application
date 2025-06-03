#include "GestureProcessor.h"

GestureProcessor::GestureProcessor(QObject *parent)
    : QObject(parent),
      pythonProcess(nullptr),
      isRunning(false)
{
    // 初始化UDP Socket
    socket = new QUdpSocket(this);
    
    // 绑定到本地端口
    if (!socket->bind(QHostAddress::LocalHost, 12345)) {
        qWarning() << "无法绑定到UDP端口12345:" << socket->errorString();
    } else {
        qDebug() << "UDP手势接收器已初始化，监听端口12345";
    }
    
    // 连接readyRead信号
    connect(socket, &QUdpSocket::readyRead, this, &GestureProcessor::processPendingDatagrams);
}

GestureProcessor::~GestureProcessor()
{
    stopCamera();
    
    if (socket) {
        socket->close();
    }
}

void GestureProcessor::startCamera()
{
    if (isRunning) {
        qDebug() << "手势识别已经在运行中，无需再次启动";
        return;
    }
    
    // 使用摄像头管理器获取可用摄像头
    auto& cameraManager = CameraResourceManager::instance();
    
    // 明确只请求video0，避免使用无效的摄像头
    CameraRequest request;
    request.requesterId = "GestureRecognizer";
    request.priority = RequestPriority::High; // 改为High不是Critical，避免抢占其他组件
    request.preferredCameraIndex = 0; // 明确指定只使用video0
    
    // 检查视频0是否可用
    if (cameraManager.getCameraState(0) != CameraState::Available) {
        qDebug() << "手势识别器: 摄像头0不可用，无法启动手势识别";
        return;
    }
    
    if (!cameraManager.requestCamera(request)) {
        qWarning() << "GestureProcessor: 无法获取摄像头资源";
        return;
    }
    
    // 确保只使用video0
    int cameraIndex = 0;
    qDebug() << "GestureProcessor: 使用摄像头索引" << cameraIndex;
    
    if (cameraIndex < 0) {
        qWarning() << "GestureProcessor: 无法找到可用摄像头";
        return;
    }
    
    qDebug() << "GestureProcessor: 使用摄像头索引" << cameraIndex;
    
    // 创建新的QProcess实例
    pythonProcess = new QProcess(this);
    
    // 设置进程环境
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("NO_AT_BRIDGE", "1");  // 禁用辅助功能总线
    pythonProcess->setProcessEnvironment(env);
    
    // 连接进程信号
    connect(pythonProcess, &QProcess::errorOccurred, [this](QProcess::ProcessError error) {
        qWarning() << "Python进程错误:" << error;
        isRunning = false;  // 确保状态正确更新
    });
    
    // 启动Python脚本，传入摄像头索引
    qDebug() << "启动手势识别Python脚本，摄像头索引:" << cameraIndex;
    QStringList args;
    args << "./gesture_recognizer.py";
    args << "--camera" << QString::number(cameraIndex);
    args << "--debug"; 
    
    pythonProcess->start("python3", args);
    
    if (!pythonProcess->waitForStarted(5000)) {
        qWarning() << "无法启动Python进程:" << pythonProcess->errorString();
        delete pythonProcess;
        pythonProcess = nullptr;
        return;
    }
    
    // 验证进程是否成功启动
    QThread::msleep(500);
    if (pythonProcess->state() == QProcess::NotRunning) {
        qWarning() << "Python进程启动后立即退出，退出码:" << pythonProcess->exitCode();
        delete pythonProcess;
        pythonProcess = nullptr;
        return;
    }
    
    isRunning = true;
    qDebug() << "手势识别系统已启动";
}

void GestureProcessor::stopCamera()
{
    if (!isRunning && !pythonProcess) {
        qDebug() << "手势识别未运行，无需停止";
        return;
    }
    
    qDebug() << "关闭手势识别系统...";
    isRunning = false;
    
    // 1. 发送EXIT命令并加入更长的等待时间
    sendExitCommand();
    QThread::msleep(1000); // 增加等待时间
    
    // 2. 确保进程被终止
    if (pythonProcess) {
        if (pythonProcess->state() != QProcess::NotRunning) {
            if (!pythonProcess->waitForFinished(3000)) { // 增加等待时间
                pythonProcess->terminate();
                
                // 等待进程终止
                if (!pythonProcess->waitForFinished(2000)) {
                    pythonProcess->kill();
                    pythonProcess->waitForFinished(1000);
                }
            }
        }
        
        delete pythonProcess;
        pythonProcess = nullptr;
    }
    
    // 3. 使用系统命令确保Python进程完全终止
    QProcess::execute("pkill", QStringList() << "-9" << "-f" << "gesture_recognizer.py");
    QThread::msleep(500);
    
    // 4. 释放资源
    auto& cameraManager = CameraResourceManager::instance();
    bool released = cameraManager.releaseCamera("GestureRecognizer");
    
    // 5. 验证释放 - 如果失败，执行更强力的清理
    if (!released) {
        qWarning() << "手势识别摄像头释放失败，尝试应急清理";
        checkAndCleanupRemainingProcesses();
        
        // 再次尝试强制释放
        cameraManager.forceReleaseCamera(0);
    }
    
    qDebug() << "手势识别系统已关闭";
}

void GestureProcessor::sendExitCommand()
{
    // 通过UDP发送EXIT命令
    QUdpSocket exitSocket;
    QByteArray datagram = "EXIT";
    exitSocket.writeDatagram(datagram, QHostAddress::LocalHost, 12346);
    qDebug() << "已发送EXIT命令到手势识别程序";
    
    // 等待短暂时间确保命令被接收
    QThread::msleep(500);
}

void GestureProcessor::checkAndCleanupRemainingProcesses()
{
    // 检查是否有gesture_recognizer.py进程仍在运行
    QProcess checkProcess;
    checkProcess.start("pgrep", QStringList() << "-f" << "gesture_recognizer.py");
    checkProcess.waitForFinished(2000);
    
    QString output = QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed();
    if (!output.isEmpty()) {
        qWarning() << "检测到残留的手势识别进程，正在清理...";
        
        // 先尝试优雅终止
        QProcess::execute("pkill", QStringList() << "-TERM" << "-f" << "gesture_recognizer.py");
        QThread::msleep(1000);
        
        // 再次检查
        checkProcess.start("pgrep", QStringList() << "-f" << "gesture_recognizer.py");
        checkProcess.waitForFinished(2000);
        
        output = QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed();
        if (!output.isEmpty()) {
            qWarning() << "优雅终止失败，强制清理进程";
            QProcess::execute("pkill", QStringList() << "-9" << "-f" << "gesture_recognizer.py");
            QThread::msleep(1000);
        }
    }
    
    // 检查OpenCV相关进程
    checkProcess.start("pgrep", QStringList() << "-f" << "python.*opencv");
    checkProcess.waitForFinished(2000);
    
    output = QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed();
    if (!output.isEmpty()) {
        qWarning() << "检测到残留的OpenCV进程，正在清理...";
        QProcess::execute("pkill", QStringList() << "-9" << "-f" << "python.*opencv");
        QThread::msleep(1000);
    }
}

void GestureProcessor::processPendingDatagrams()
{
    while (socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;
        
        socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        
        qDebug() << "接收到来自" << sender.toString() << ":" << senderPort << "的手势数据";
        
        // 解析JSON
        QJsonDocument doc = QJsonDocument::fromJson(datagram);
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj.contains("gesture")) {
                QString gesture = obj["gesture"].toString();
                qDebug() << "接收到手势:" << gesture;
                
                // 发出信号
                emit gestureDetected(gesture);
            }
        }
    }
}