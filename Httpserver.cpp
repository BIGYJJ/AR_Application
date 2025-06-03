#include "Httpserver.h"
#include <QTextStream>
#include <QStringList>
#include <QDateTime>
#include <QDebug>
#include <QUrlQuery>
#include <QFile>

#if HAS_SSL
#include <QSslCertificate>
#include <QSslKey>
#endif

HttpServer::HttpServer(DatabaseWorker* dbWorker, QObject* parent)
    : QTcpServer(parent), m_requestHandler(dbWorker, this)
{
    qDebug() << "HTTP服务器已初始化";
    qDebug() << "本地IP地址:" << getLocalIpAddress();
}

void HttpServer::registerNavigationWidget(NavigationDisplayWidget* widget)
{
    qDebug() << "HttpServer正在注册导航显示部件...";
    
    try {
        m_requestHandler.registerNavigationWidget(widget);
        qDebug() << "HttpServer注册导航显示部件完成";
    } catch (const std::exception& e) {
        qCritical() << "注册导航显示部件时发生异常:" << e.what();
    } catch (...) {
        qCritical() << "注册导航显示部件时发生未知异常";
    }
}

#if HAS_SSL
bool HttpServer::setupSslConfiguration()
{
    // 设置SSL证书和私钥路径
    const QString certPath = "server.crt";
    const QString keyPath = "server.key";

    // 加载证书
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        qCritical() << "无法打开证书文件:" << certPath;
        return false;
    }
    QSslCertificate certificate(&certFile, QSsl::Pem);
    certFile.close();

    if (certificate.isNull()) {
        qCritical() << "证书为空";
        return false;
    }

    // 加载私钥
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        qCritical() << "无法打开私钥文件:" << keyPath;
        return false;
    }
    QSslKey sslKey(&keyFile, QSsl::Rsa, QSsl::Pem);
    keyFile.close();

    if (sslKey.isNull()) {
        qCritical() << "SSL密钥为空";
        return false;
    }

    // 配置SSL
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setLocalCertificate(certificate);
    sslConfig.setPrivateKey(sslKey);
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    
    // 设置SSL配置
    QSslConfiguration::setDefaultConfiguration(sslConfig);
    
    m_useSsl = true;
    qDebug() << "SSL配置成功设置";
    return true;
}
#endif

QString HttpServer::getLocalIpAddress() const
{
    const QHostAddress &localhost = QHostAddress(QHostAddress::LocalHost);
    QHostAddress ipAddress;
    
    // 尝试获取首个非本地回环地址
    for (const QHostAddress &address : QNetworkInterface::allAddresses()) {
        if (address != localhost && address.protocol() == QAbstractSocket::IPv4Protocol) {
            return address.toString();
        }
    }
    
    // 如果找不到其他地址，返回本地回环地址
    return localhost.toString();
}

void HttpServer::incomingConnection(qintptr socketDescriptor)
{
#if HAS_SSL
    if (m_useSsl) {
        // 使用SSL Socket
        QSslSocket* sslSocket = new QSslSocket(this);
        
        if (sslSocket->setSocketDescriptor(socketDescriptor)) {
            connect(sslSocket, &QSslSocket::encrypted, this, [this, sslSocket]() {
                qDebug() << "SSL连接已建立，客户端:" << sslSocket->peerAddress().toString();
            });
            
            connect(sslSocket, QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
                this, [sslSocket](const QList<QSslError> &errors) {
                    qWarning() << "SSL错误:";
                    for (const QSslError &error : errors) {
                        qWarning() << "  -" << error.errorString();
                    }
                    
                    // 在开发环境下，忽略SSL错误
                    // 注意：生产环境中应删除此行！
                    sslSocket->ignoreSslErrors();
                });
            
            connect(sslSocket, &QSslSocket::readyRead, this, &HttpServer::readClient);
            connect(sslSocket, &QSslSocket::disconnected, this, &HttpServer::discardClient);
            
            // 启动服务器端加密
            sslSocket->startServerEncryption();
        } else {
            qWarning() << "无法为SSL套接字设置套接字描述符";
            sslSocket->deleteLater();
        }
        return;
    }
#endif

    // 使用标准TCP Socket（当没有SSL或SSL未启用时）
    QTcpSocket* client = new QTcpSocket(this);
    client->setSocketDescriptor(socketDescriptor);
    
    // 增加这两行设置超时时间和保持连接
    client->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    client->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    
    qDebug() << "新的HTTP连接来自:" << client->peerAddress().toString() << ":" << client->peerPort();

    connect(client, &QTcpSocket::readyRead, this, &HttpServer::readClient);
    connect(client, &QTcpSocket::disconnected, this, &HttpServer::discardClient);
}
QString HttpServer::findHeaderIgnoreCase(const QMap<QString, QString>& headers, const QString& name) const {
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) {
            return it.value();
        }
    }
    return QString();
}
void HttpServer::readClient()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) {
        qWarning() << "socket为空，无法读取请求";
        return;
    }

    try {
        if (socket->canReadLine()) {
            QString requestLine = QString(socket->readLine()).trimmed();
            QStringList tokens = requestLine.split(QRegularExpression("[ \r\n][ \r\n]*"));
            
            qDebug() << "收到HTTP请求来自:" << socket->peerAddress().toString() 
                     << "请求:" << requestLine;
            
            // 解析请求行
            if (tokens.length() < 2) {
                qWarning() << "无效的HTTP请求格式";
                sendErrorResponse(socket, 400, "Bad Request");
                return;
            }
            
            QString method = tokens[0];
            QStringList pathParts = tokens[1].split("?");
            QString path = pathParts[0];
            
            // 解析查询参数
            QMap<QString, QString> query;
            if (pathParts.size() > 1) {
                QUrlQuery urlQuery(pathParts[1]);
                QList<QPair<QString, QString>> items = urlQuery.queryItems();
                for (const auto& item : items) {
                    query[item.first] = item.second;
                }
            }
            
            // 解析HTTP头部
            QMap<QString, QString> headers;
            while (socket->canReadLine()) {
                QString line = socket->readLine().trimmed();
                if (line.isEmpty()) break; // 头部结束
                
                int separatorIndex = line.indexOf(":");
                if (separatorIndex > 0) {
                    QString key = line.left(separatorIndex).trimmed();
                    QString value = line.mid(separatorIndex + 1).trimmed();
                    headers[key] = value;
                    qDebug() << "  头部:" << key << "=" << value;
                }
            }
            
            // 读取请求体 - 这部分是重要的修改
            QByteArray body;
            if (method == "POST" || method == "PUT") {
                // 检查Content-Length头部 - 注意大小写不敏感匹配
                QString contentLengthValue;
                for (auto it = headers.begin(); it != headers.end(); ++it) {
                    if (it.key().toLower() == "content-length") {
                        contentLengthValue = it.value();
                        qDebug() << "找到Content-Length头部:" << contentLengthValue;
                        break;
                    }
                }
                
                if (!contentLengthValue.isEmpty()) {
                    bool ok;
                    int contentLength = contentLengthValue.toInt(&ok);
                    if (ok && contentLength > 0) {
                        qDebug() << "预期内容长度:" << contentLength;
                        
                        // 循环读取数据直到达到内容长度或超时
                        int bytesReceived = 0;
                        QTime timeout = QTime::currentTime().addSecs(30); // 30秒超时
                        
                        while (bytesReceived < contentLength) {
                            // 检查是否有可读数据
                            if (socket->bytesAvailable() == 0) {
                                // 等待更多数据
                                if (!socket->waitForReadyRead(1000) || QTime::currentTime() > timeout) {
                                    qWarning() << "等待请求体数据超时或连接中断";
                                    break;
                                }
                            }
                            
                            // 读取可用数据
                            QByteArray chunk = socket->read(qMin(socket->bytesAvailable(), contentLength - bytesReceived));
                            if (chunk.isEmpty()) {
                                qWarning() << "读取请求体失败 - 接收到空数据块";
                                continue;
                            }
                            
                            body.append(chunk);
                            bytesReceived += chunk.size();
                            
                            qDebug() << "已读取" << bytesReceived << "字节,共" << contentLength << "字节";
                        }
                        
                        qDebug() << "请求体读取完成,总大小:" << body.size() << "字节";
                        
                        // 检查multipart/form-data请求
                        QString contentType;
                        for (auto it = headers.begin(); it != headers.end(); ++it) {
                            if (it.key().toLower() == "content-type") {
                                contentType = it.value();
                                break;
                            }
                        }
                        
                        if (contentType.contains("multipart/form-data") && !body.isEmpty()) {
                            qDebug() << "检测到multipart/form-data请求,长度:" << body.size();
                            
                            // 提取boundary
                            int boundaryPos = contentType.indexOf("boundary=");
                            if (boundaryPos > 0) {
                                QString boundary = contentType.mid(boundaryPos + 9);
                                if (boundary.startsWith("\"") && boundary.endsWith("\"")) {
                                    boundary = boundary.mid(1, boundary.length() - 2);
                                }
                                QString boundaryMarker = "--" + boundary;
                                QByteArray boundaryBytes = boundaryMarker.toUtf8();
                                
                                qDebug() << "表单边界:" << boundaryMarker;
                                
                                // 查找"name=\"pdf\""或类似标记
                                QByteArray formData = body;
                                int pdfPos = formData.indexOf("name=\"pdf\"");
                                if (pdfPos <= 0) {
                                    pdfPos = formData.indexOf("name=\"file\"");
                                }
                                
                                if (pdfPos > 0) {
                                    qDebug() << "找到PDF表单字段,位置:" << pdfPos;
                                    
                                    // 查找头部结束位置（双换行）
                                    int headerEnd = formData.indexOf("\r\n\r\n", pdfPos);
                                    if (headerEnd > 0) {
                                        int dataStart = headerEnd + 4;
                                        
                                        // 查找结束边界
                                        int dataEnd = formData.indexOf(boundaryBytes, dataStart);
                                        if (dataEnd > dataStart) {
                                            // 调整结束位置（去掉前导的\r\n）
                                            dataEnd -= 2;
                                            
                                            // 提取PDF数据
                                            QByteArray pdfData = formData.mid(dataStart, dataEnd - dataStart);
                                            qDebug() << "成功提取PDF数据,大小:" << pdfData.size() << "字节";
                                            
                                            // 更新body为提取的PDF数据
                                            body = pdfData;
                                        } else {
                                            qWarning() << "无法找到表单数据结束位置";
                                        }
                                    } else {
                                        qWarning() << "无法找到表单数据头部结束位置";
                                    }
                                } else {
                                    qWarning() << "未找到PDF/file表单字段,前200字节:";
                                    qDebug() << formData.left(200);
                                }
                            } else {
                                qWarning() << "无法从Content-Type提取boundary";
                            }
                        }
                    } else {
                        qWarning() << "无效的Content-Length值:" << contentLengthValue;
                    }
                } else {
                    qWarning() << "POST请求缺少Content-Length头部";
                }
            }
            
            // 创建请求对象并处理
            RequestHandler::HttpRequest request;
            request.method = method;
            request.path = path;
            request.headers = headers;
            request.query = query;
            request.body = body;
            
            // 修正Content-Length头
            if (!body.isEmpty()) {
                request.headers["Content-Length"] = QString::number(body.size());
            }
            
            // 处理请求并获取响应
            RequestHandler::HttpResponse response;
            try {
                qDebug() << "处理请求，路由键:" << path;
                response = m_requestHandler.handleRequest(request);
            } catch (...) {
                qCritical() << "处理请求时发生未捕获的异常";
                sendErrorResponse(socket, 500, "Internal Server Error");
                return;
            }
            
            // 发送HTTP响应
            sendResponse(socket, response);
        } else {
            qDebug() << "套接字还没有准备好读取一行,等待...";
            socket->waitForReadyRead(1000);
        }
    } catch (const std::exception& e) {
        qCritical() << "处理客户端请求时发生异常:" << e.what();
        sendErrorResponse(socket, 500, "Internal Server Error");
    } catch (...) {
        qCritical() << "处理客户端请求时发生未知异常";
        sendErrorResponse(socket, 500, "Internal Server Error");
    }
}
void HttpServer::discardClient()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        qDebug() << "连接关闭，客户端:" << socket->peerAddress().toString();
        
        // 断开连接 - 如果尚未断开
        if (socket->state() != QTcpSocket::UnconnectedState) {
            socket->disconnectFromHost();
        }
        
        // 安全删除，而不是直接删除
        socket->deleteLater();
    }
}

void HttpServer::sendResponse(QTcpSocket* socket, const RequestHandler::HttpResponse& response)
{
    if (!socket || socket->state() != QTcpSocket::ConnectedState) {
        qWarning() << "无法发送响应：套接字无效或未连接";
        return;
    }
    
    try {
        // 创建一个临时的QTextStream对象，不要使用类成员变量
        QTextStream os(socket);
        os.setAutoDetectUnicode(true);
        
        // 状态行
        os << "HTTP/1.1 " << response.statusCode << " " << response.statusMessage << "\r\n";
        
        // 公共头部
        os << "Date: " << QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date) << "\r\n";
        os << "Server: QtHttpServer\r\n";
        os << "Content-Type: " << response.contentType << "\r\n";
        os << "Content-Length: " << response.content.size() << "\r\n";
        
        // 添加CORS头部
        os << "Access-Control-Allow-Origin: *\r\n";
        os << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        os << "Access-Control-Allow-Headers: Content-Type\r\n";
        
        // 自定义头部
        for (auto it = response.headers.constBegin(); it != response.headers.constEnd(); ++it) {
            os << it.key() << ": " << it.value() << "\r\n";
        }
        
        // 空行标志头部结束
        os << "\r\n";
        os.flush();
        
        // 发送响应内容
        if (response.content.size() > 0) {
            qint64 bytesWritten = socket->write(response.content);
            if (bytesWritten != response.content.size()) {
                qWarning() << "写入的字节数与内容长度不匹配:" 
                          << bytesWritten << "vs" << response.content.size();
            }
            socket->flush();
        }
        
        qDebug() << "响应已发送: 状态码" << response.statusCode 
                << ", 内容长度" << response.content.size() << "字节";
                
        // 安全地断开连接
        // 不要在这里删除socket，而是使用deleteLater
        socket->disconnectFromHost();
    } catch (const std::exception& e) {
        qCritical() << "发送响应时发生异常:" << e.what();
    } catch (...) {
        qCritical() << "发送响应时发生未知异常";
    }
}

void HttpServer::sendErrorResponse(QTcpSocket* socket, int statusCode, const QString& message)
{
    if (!socket || socket->state() != QTcpSocket::ConnectedState) {
        return;
    }
    
    try {
        RequestHandler::HttpResponse errorResponse;
        errorResponse.statusCode = statusCode;
        errorResponse.statusMessage = message;
        errorResponse.contentType = "application/json";
        
        // 创建简单的错误JSON
        QJsonObject errorObj;
        errorObj["error"] = true;
        errorObj["message"] = message;
        errorObj["status"] = statusCode;
        QJsonDocument doc(errorObj);
        errorResponse.content = doc.toJson();
        
        // 添加CORS头部
        errorResponse.headers.insert("Access-Control-Allow-Origin", "*");
        
        sendResponse(socket, errorResponse);
    } catch (...) {
        qCritical() << "发送错误响应时发生异常";
        socket->disconnectFromHost();
    }
}