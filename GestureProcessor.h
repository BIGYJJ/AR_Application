#ifndef GESTUREPROCESSOR_H
#define GESTUREPROCESSOR_H

#include <QObject>
#include <QProcess>
#include <QDebug>
#include <QUdpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include "NavigationDisplayWidget.h"
class GestureProcessor : public QObject
{
    Q_OBJECT

public:
    explicit GestureProcessor(QObject *parent = nullptr);
    ~GestureProcessor();

    void startCamera();
    void stopCamera();

signals:
    // 当检测到手势时发出信号
    void gestureDetected(const QString &gesture);

private slots:
    // 处理UDP数据包
    void processPendingDatagrams();

private:
    // Python进程
    QProcess *pythonProcess;
    // 启动和停止手势识别
    
    // UDP Socket
    QUdpSocket *socket;
    
    // 状态标志
    bool isRunning;
    void sendExitCommand();
    void checkAndCleanupRemainingProcesses();
};

#endif // GESTUREPROCESSOR_H