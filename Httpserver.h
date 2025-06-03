#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkInterface>
#include "Requesthandler.h"
#include "Databaseworker.h"
#include "NavigationDisplayWidget.h"

// 检查是否有Qt SSL支持
#if QT_CONFIG(ssl)
#include <QSslSocket>
#include <QSslConfiguration>
#define HAS_SSL 1
#endif

class HttpServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit HttpServer(DatabaseWorker* dbWorker, QObject* parent = nullptr);
    
    // 注册导航显示部件以接收导航数据
    void registerNavigationWidget(NavigationDisplayWidget* widget);
    
    // 获取当前IP地址
    QString getLocalIpAddress() const;
    RequestHandler& getRequestHandler() { return m_requestHandler; }

     // 直接连接导航信号
     void connectNavigationSignals(NavigationDisplayWidget* widget) {
        if (widget) {
            connect(&m_requestHandler, &RequestHandler::navigationDataReceived,
                   widget, &NavigationDisplayWidget::updateNavigation,
                   Qt::QueuedConnection);
            qDebug() << "导航信号已连接";
        }
    }
#if HAS_SSL
    // 仅在有SSL支持时编译此函数
    bool setupSslConfiguration();
#endif

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
    void readClient();
    void discardClient();

private:
    RequestHandler m_requestHandler;
    QMutex m_requestMutex;
    QString findHeaderIgnoreCase(const QMap<QString, QString>& headers, const QString& name) const;
    // 是否使用SSL
    bool m_useSsl = false;
    
    // 发送HTTP响应 - 使用基类QTcpSocket
    void sendResponse(QTcpSocket* socket, const RequestHandler::HttpResponse& response);
    
    // 发送错误响应 - 使用基类QTcpSocket
    void sendErrorResponse(QTcpSocket* socket, int statusCode, const QString& message);
};

#endif // HTTPSERVER_H