#include <QApplication>
#include "MainWindow.h"
#include <QSqlDatabase>
#include <QDebug>

#include <cstring>   // 解决 memcpy/memset 错误
#include <utility>   // 解决 std::move 错误
#include "Databaseworker.h"
#include "Httpserver.h"
#include <QRandomGenerator>
#include <QFile>
#include "CameraResourceManager.h"  // 添加中央摄像头管理器

// 安全的摄像头初始化函数
void safeInitializeCamera()
{
    qDebug() << "开始安全初始化摄像头资源...";
    
    // 先终止可能残留的进程
    QProcess::execute("pkill", QStringList() << "-f" << "gesture_recognizer.py");
    QProcess::execute("pkill", QStringList() << "-f" << "python.*opencv");
    
    // 等待更长时间确保进程终止
    QThread::msleep(2000);
    
    // 重置所有摄像头资源
    auto& cameraManager = CameraResourceManager::instance();
    cameraManager.resetAllCameras();
    
    // 增加更长的稳定化延时
    QThread::msleep(2000);
    
    // 检查摄像头状态
    for (int i = 0; i <= 2; ++i) {
        CameraState state = cameraManager.getCameraState(i);
        qDebug() << "摄像头" << i << "初始状态:" << (int)state;
    }
    
    // 预先分配资源给手势识别器
    CameraRequest request;
    request.requesterId = "GestureRecognizer";
    request.priority = RequestPriority::Critical;
    request.preferredCameraIndex = 0;
    
    if (cameraManager.requestCamera(request)) {
        qDebug() << "摄像头资源已预分配给手势识别器";
    } else {
        qWarning() << "无法预分配摄像头资源给手势识别器";
    }
    
    qDebug() << "摄像头资源初始化完成";
}

int main(int argc, char *argv[]) {
    
    QApplication app(argc, argv);
    
    // Create main window
    MainWindow window;
    window.resize(1000, 600);
    
    // 使用安全方式初始化摄像头资源
    try {
        safeInitializeCamera();
    } catch (const std::exception& e) {
        qCritical() << "摄像头初始化异常: " << e.what();
    } catch (...) {
        qCritical() << "摄像头初始化发生未知异常";
    }
    
    // Create database worker - keep as a static or global object to maintain connection
    DatabaseWorker dbWorker;
    if(!dbWorker.connect("localhost", 3306, "root", "MyStrongPassword123!", "translation_db")) {
        qCritical() << "数据库连接失败!";
        return 1;
    }

    // Create HTTP server - will remain active throughout the application's lifetime
    HttpServer server(&dbWorker);

#if HAS_SSL
    // 只有在编译支持SSL时才执行这段代码
    // 尝试设置SSL配置
    bool sslEnabled = server.setupSslConfiguration();
    if(sslEnabled) {
        qDebug() << "已成功启用SSL安全连接";
    } else {
        qWarning() << "SSL配置失败，将使用不安全的HTTP连接";
    }
#else
    qInfo() << "编译时没有启用SSL支持，服务器将使用普通HTTP协议";
#endif

    if(!server.listen(QHostAddress::Any, 8080)) {
        qCritical() << "服务器启动失败:" << server.errorString();
        return 2;
    }
    else {
        QString ipAddress = server.getLocalIpAddress();
        qDebug() << "服务器成功启动在地址:" << ipAddress << "端口: 8080";
        
        // 显示所有可用的网络地址
        QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
        qDebug() << "本机所有网络地址:";
        for (const QHostAddress &address : addresses) {
            qDebug() << "  - " << address.toString() << (address.protocol() == QAbstractSocket::IPv4Protocol ? " (IPv4)" : " (IPv6)");
        }
        
        // 尝试测试数据库连接
        QJsonArray testQuery = dbWorker.queryData("SELECT 1 AS test");
        if (!testQuery.isEmpty()) {
            qDebug() << "数据库连接测试成功";
        } else {
            qWarning() << "数据库连接测试失败";
        }
    }

    // Set HTTP server reference to main window
    window.setHttpServer(&server);
    
    window.setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | 
                          Qt::WindowTitleHint | Qt::WindowCloseButtonHint | 
                          Qt::WindowMinMaxButtonsHint);
    window.showNormal();

    return app.exec();
}