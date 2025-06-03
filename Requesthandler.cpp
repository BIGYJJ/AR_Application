#include "Requesthandler.h"
#include "NavigationDisplayWidget.h"
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QDateTime>

RequestHandler::RequestHandler(DatabaseWorker* dbWorker, QObject* parent) 
    : QObject(parent), 
      m_dbWorker(dbWorker),
      m_navigationWidget(nullptr),
      m_currentDirection("未设置"),
      m_currentDistance("未知"),
      m_navigationActive(false)
{
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^POST /api/execute-sql/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleExecuteSQL(req); }
        )
    );
    // 原有API路由 - 使用std::map的insert方法而不是QMap的insert方法
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^GET /api/data/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleGetData(req); }
        )
    );

    m_routes.insert(
        std::make_pair(
            QRegularExpression("^POST /api/data/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handlePostData(req); }
        )
    );
    
    // 新增导航相关API路由
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^GET /api/navigation/data/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleGetNavigationData(req); }
        )
    );
    
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^POST /api/navigation/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handlePostNavigationData(req); }
        )
    );
    
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^GET /api/navigation/register/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleRegisterNavigation(req); }
        )
    );
    
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^GET /api/navigation/unregister/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleUnregisterNavigation(req); }
        )
    );
    
    // 增加CORS支持的OPTIONS请求处理
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^OPTIONS", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ 
                HttpResponse response;
                response.headers.insert("Access-Control-Allow-Origin", "*");
                response.headers.insert("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                response.headers.insert("Access-Control-Allow-Headers", "Content-Type, Authorization");
                response.headers.insert("Access-Control-Max-Age", "86400"); // 24小时
                response.content = "";
                return response;
            }
        )
    );

    m_routes.insert(
        std::make_pair(
            QRegularExpression("^GET /api/page/switch/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleSwitchPage(req); }
        )
    );
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^GET /api/page/back/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleBackToMain(req); }
        )
    );

    m_routes.insert(
        std::make_pair(
            QRegularExpression("^POST /api/pdf/upload/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handleUploadPDF(req); }
        )
    );
    
    m_routes.insert(
        std::make_pair(
            QRegularExpression("^GET /api/pdf/control/?$", QRegularExpression::CaseInsensitiveOption),
            [this](const HttpRequest& req){ return handlePDFControl(req); }
        )
    );
}

// 实现PDF上传处理方法
RequestHandler::HttpResponse RequestHandler::handleUploadPDF(const HttpRequest& request)
{
    qDebug() << "处理PDF上传请求，内容长度:" << request.body.size();
    
    // 打印所有头部
    qDebug() << "请求头:";
    for (auto it = request.headers.constBegin(); it != request.headers.constEnd(); ++it) {
        qDebug() << "  \"" << it.key() << "\" : \"" << it.value() << "\"";
    }
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    // 检查请求体
    if (request.body.isEmpty()) {
        qWarning() << "请求体为空! Content-Type: " << request.headers.value("content-type");
        return createErrorResponse(400, "PDF data is empty");
    }
    
    // 检查PDF文件头
    if (request.body.left(4) == QByteArray("%PDF")) {
        qDebug() << "检测到有效的PDF文件头,大小:" << request.body.size() << "字节";
    } else {
        qDebug() << "数据不是有效的PDF格式,前20字节:" << request.body.left(20).toHex();
    }
    
    // 发出信号通知PDFViewerPage接收PDF数据
    emit pdfDataReceived(request.body);
    
    // 创建成功响应
    QJsonObject resultObj;
    resultObj["success"] = true;
    resultObj["message"] = "PDF uploaded successfully";
    resultObj["size"] = request.body.size();
    resultObj["totalPages"] = 1; // 这里可以添加实际页数检测
    
    QJsonDocument doc(resultObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    return response;
}

// 实现PDF控制处理方法
RequestHandler::HttpResponse RequestHandler::handlePDFControl(const HttpRequest& request)
{
    qDebug() << "处理PDF控制请求";
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    QString action = request.query.value("action", "");
    QJsonObject resultObj;
    
    if (action == "next") {
        emit pdfNextPage();
        resultObj["success"] = true;
        resultObj["message"] = "Next page command sent";
    }
    else if (action == "prev") {
        emit pdfPrevPage();
        resultObj["success"] = true;
        resultObj["message"] = "Previous page command sent";
    }
    else {
        return createErrorResponse(400, "Invalid action parameter");
    }
    
    QJsonDocument doc(resultObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    return response;
}


// 添加对应的处理方法
RequestHandler::HttpResponse RequestHandler::handleExecuteSQL(const HttpRequest& request)
{
    qDebug() << "处理POST /api/execute-sql请求";
    
    // 解析请求体中的JSON数据
    QJsonDocument doc = QJsonDocument::fromJson(request.body);
    if (doc.isNull() || !doc.isObject()) {
        return createErrorResponse(400, "Invalid JSON data");
    }
    
    QJsonObject dataObj = doc.object();
    
    // 从JSON中获取SQL语句
    if (!dataObj.contains("sql")) {
        return createErrorResponse(400, "Missing SQL statement");
    }
    
    QString sql = dataObj["sql"].toString();
    
    // 安全检查：防止危险的SQL操作
    QString sqlLower = sql.toLower();
    if (sqlLower.contains("drop") || sqlLower.contains("truncate") || 
        (sqlLower.contains("delete") && !sqlLower.contains("where"))) {
        return createErrorResponse(403, "Potentially dangerous SQL operation not allowed");
    }
    
    // 执行SQL查询
    QJsonArray result;
    try {
        result = m_dbWorker->queryData(sql);
    } catch (std::exception& e) {
        qCritical() << "数据库查询失败:" << e.what();
        return createErrorResponse(500, "Database query failed");
    }
    
    // 构建响应
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    // 将结果转换为JSON
    QJsonDocument resultDoc(result);
    response.content = resultDoc.toJson(QJsonDocument::Compact);
    
    return response;
}

void RequestHandler::registerNavigationWidget(NavigationDisplayWidget* widget)
{
    QMutexLocker locker(&m_mutex);
    
    qDebug() << "RequestHandler::registerNavigationWidget - 开始, widget地址:" << widget;
    
    // 将旧的导航部件的指针保存为局部变量，避免直接覆盖可能在使用中的指针
    NavigationDisplayWidget* oldWidget = m_navigationWidget;
    m_navigationWidget = widget;
    
    qDebug() << "RequestHandler::registerNavigationWidget - 完成";
}

void RequestHandler::unregisterNavigationWidget()
{
    m_navigationWidget = nullptr;
    qDebug() << "导航显示部件已注销";
}

bool RequestHandler::isNavigationWidgetActive() const
{
    return m_navigationWidget != nullptr;
}

RequestHandler::HttpResponse RequestHandler::handleRequest(const HttpRequest& request)
{
    
    qDebug() << "处理请求，路由键:" << request.path;
    
    // 当请求是OPTIONS时，处理CORS预检请求
    if (request.method == "OPTIONS") {
        HttpResponse response;
        response.statusCode = 200;
        response.statusMessage = "OK";
        response.contentType = "text/plain";
        response.content = "";
        
        // 添加CORS头部
        response.headers.insert("Access-Control-Allow-Origin", "*");
        response.headers.insert("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        response.headers.insert("Access-Control-Allow-Headers", "Content-Type");
        
        return response;
    }
    
    // 原有的路由逻辑...
    HttpResponse response;
    
    // 匹配GET /api/data
    if (request.method == "GET" && request.path == "/api/data") {
        response = handleGetData(request);
    }
    // 其他路由逻辑...
    else if (request.method == "GET" && request.path == "/api/navigation/data") {
        response = handleGetNavigationData(request);
    }
    else if (request.method == "GET" && request.path == "/api/navigation/register") {
        response = handleRegisterNavigation(request);
    }
    else if (request.method == "GET" && request.path == "/api/page/switch") {
        response = handleSwitchPage(request);
    }
    else if (request.method == "GET" && request.path == "/api/page/back") {
        response = handleBackToMain(request);
    }
    else if (request.method == "GET" && request.path == "/api/navigation/unregister") {
        response = handleUnregisterNavigation(request);
    }
    else if (request.method == "POST" && request.path == "/api/navigation") {
        response = handlePostNavigationData(request);
    }
    else if (request.method == "POST" && request.path == "/api/pdf/upload") {
        response = handleUploadPDF(request);
    }
    else if (request.method == "POST" && request.path == "/api/execute-sql") {
        response = handleExecuteSQL(request);
    }
    else if (request.method == "GET" && request.path == "/api/pdf/control") {
        response = handlePDFControl(request);
    }
    else {
        // 如果没有匹配的路由，返回404
        response = createErrorResponse(404, "Not Found");
    }
    
    // 为所有响应添加CORS头部
    response.headers.insert("Access-Control-Allow-Origin", "*");
    response.headers.insert("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.headers.insert("Access-Control-Allow-Headers", "Content-Type");
    
    return response;
}

// 处理导航注册请求
RequestHandler::HttpResponse RequestHandler::handleRegisterNavigation(const HttpRequest& request)
{
    qDebug() << "处理导航注册请求，来自:" << request.headers.value("User-Agent", "未知");
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    QJsonObject resultObj;
    
    // 检查是否已注册导航部件
    // 使用互斥锁保护共享资源
    QMutexLocker locker(&m_mutex);
    
    if (m_navigationWidget) {
        m_navigationActive = true;
        
        resultObj["success"] = true;
        resultObj["message"] = "Navigation registered successfully";
        resultObj["widgetActive"] = true;
        resultObj["serverTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        
        qDebug() << "导航注册成功，部件地址:" << m_navigationWidget;
    } else {
        resultObj["success"] = false;
        resultObj["message"] = "Navigation widget not available";
        resultObj["widgetActive"] = false;
        resultObj["serverTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        
        qWarning() << "导航注册失败，部件不可用";
    }
    
    QJsonDocument doc(resultObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    // 添加CORS头部
    response.headers.insert("Access-Control-Allow-Origin", "*");
    response.headers.insert("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.headers.insert("Access-Control-Allow-Headers", "Content-Type");
    response.headers.insert("Access-Control-Max-Age", "86400"); // 24小时
    
    return response;
}
// 处理导航注销请求
RequestHandler::HttpResponse RequestHandler::handleUnregisterNavigation(const HttpRequest& request)
{
    qDebug() << "处理导航注销请求";
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    QJsonObject resultObj;
    
    // 检查是否已注册导航部件
    if (m_navigationWidget) {
        QString deviceID = request.query.contains("deviceID") ? request.query["deviceID"] : "";
        
        QMutexLocker locker(&m_mutex);
        
        // 假设NavigationDisplayWidget有unregisterDevice方法
        // 如果没有这个方法，需要修改或移除这行
        // m_navigationWidget->unregisterDevice(deviceID);
        
        resultObj["success"] = true;
        resultObj["message"] = "Device unregistered successfully";
    } else {
        resultObj["success"] = false;
        resultObj["message"] = "Navigation widget not available";
    }
    
    QJsonDocument doc(resultObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    return response;
}

// 处理POST导航请求
RequestHandler::HttpResponse RequestHandler::handlePostNavigationData(const HttpRequest& request)
{
    qDebug() << "处理导航数据提交请求，线程ID:" << QThread::currentThreadId() << "请求体:" << request.body;
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    QJsonObject resultObj;
    
    // 解析请求体中的JSON数据
    QJsonDocument doc = QJsonDocument::fromJson(request.body);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "无效的JSON数据:" << request.body;
        return createErrorResponse(400, "Invalid JSON data");
    }
    
    QJsonObject navData = doc.object();
    QString action = navData["action"].toString();
    
    qDebug() << "导航动作:" << action;
    
    if (action == "update_navigation") {
        if (!navData.contains("direction") || !navData.contains("distance")) {
            qWarning() << "缺少方向或距离字段";
            return createErrorResponse(400, "Missing direction or distance");
        }
        
        QString direction = navData["direction"].toString();
        QString distance = navData["distance"].toString();
        
        qDebug() << "更新导航 - 方向:" << direction << "距离:" << distance;
        
        QMutexLocker locker(&m_mutex);
        m_currentDirection = direction;
        m_currentDistance = distance;
        m_navigationActive = true;
        
        // 使用NavigationDisplayWidget的updateNavigation方法
        if (m_navigationWidget) {
            qDebug() << "调用NavigationDisplayWidget.updateNavigation - widget地址:" << m_navigationWidget;
            
            // 使用直接连接而非QueuedConnection
            bool success = QMetaObject::invokeMethod(m_navigationWidget, "updateNavigation", 
                                      Qt::DirectConnection,
                                      Q_ARG(QString, direction),
                                      Q_ARG(QString, distance));
            
            qDebug() << "QMetaObject::invokeMethod 结果:" << (success ? "成功" : "失败");
            
            // 在更新导航数据后添加信号
            emit navigationDataReceived(direction, distance);
            
            resultObj["success"] = true;
            resultObj["message"] = "Navigation data updated";
        } else {
            qWarning() << "导航部件不可用 (m_navigationWidget 为空)";
            resultObj["success"] = false;
            resultObj["message"] = "Navigation widget not available";
        }
    } 
    else if (action == "stop_navigation") {
        QMutexLocker locker(&m_mutex);
        m_navigationActive = false;
        m_currentDirection = "未设置";
        m_currentDistance = "未知";
        
        if (m_navigationWidget) {
            QMetaObject::invokeMethod(m_navigationWidget, "updateNavigation", 
                                    Qt::DirectConnection,
                                    Q_ARG(QString, m_currentDirection),
                                    Q_ARG(QString, m_currentDistance));
            
            // 同样在stop_navigation添加
            emit navigationDataReceived(m_currentDirection, m_currentDistance);
        }
        
        resultObj["success"] = true;
        resultObj["message"] = "Navigation stopped";
    }
    else {
        return createErrorResponse(400, "Unknown action: " + action);
    }
    
    QJsonDocument responseDoc(resultObj);
    response.content = responseDoc.toJson(QJsonDocument::Compact);
    response.headers.insert("Access-Control-Allow-Origin", "*");
    response.headers.insert("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.headers.insert("Access-Control-Allow-Headers", "Content-Type");
    
    return response;
}

// 处理GET导航数据请求
RequestHandler::HttpResponse RequestHandler::handleGetNavigationData(const HttpRequest& request)
{
    qDebug() << "处理GET导航数据请求";
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    QJsonObject resultObj;
    
    // 使用互斥锁保护共享资源
    QMutexLocker locker(&m_mutex);
    
    // 检查是否已注册导航部件
    if (m_navigationWidget) {
        // 获取当前导航数据
        QJsonObject navData;
        navData["direction"] = m_currentDirection;
        navData["distance"] = m_currentDistance;
        navData["active"] = m_navigationActive;
        navData["timestamp"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
        resultObj = navData;
    } else {
        resultObj["error"] = true;
        resultObj["message"] = "Navigation widget not available";
    }
    
    QJsonDocument doc(resultObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    // 添加CORS头部
    response.headers.insert("Access-Control-Allow-Origin", "*");
    response.headers.insert("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.headers.insert("Access-Control-Allow-Headers", "Content-Type");
    
    return response;
}

RequestHandler::HttpResponse RequestHandler::handleGetData(const HttpRequest& request)
{
    qDebug() << "处理GET /api/data请求";
    
    // 使用数据库工作器执行查询，获取translations表中的所有数据
    QJsonArray data;
    try {
        // 需要修改这里的SQL查询
        data = m_dbWorker->queryData("SELECT id, recognized_text, translated_text, timestamp AS translation_time FROM translations ORDER BY id DESC LIMIT 100");
    } catch (std::exception& e) {
        qCritical() << "数据库查询失败:" << e.what();
        return createErrorResponse(500, "Database query failed");
    }
    
    // 构建响应
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    // 将结果转换为JSON
    QJsonDocument doc(data);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    qDebug() << "查询结果大小:" << data.size() << "条记录";
    
    return response;
}

RequestHandler::HttpResponse RequestHandler::handlePostData(const HttpRequest& request)
{
    qDebug() << "处理POST /api/data请求";
    
    // 解析请求体中的JSON数据
    QJsonDocument doc = QJsonDocument::fromJson(request.body);
    if (doc.isNull() || !doc.isObject()) {
        return createErrorResponse(400, "Invalid JSON data");
    }
    
    QJsonObject dataObj = doc.object();
    
    // 从JSON中获取recognized_text和translated_text
    if (!dataObj.contains("recognized_text") || !dataObj.contains("translated_text")) {
        return createErrorResponse(400, "Missing required fields");
    }
    
    QString recognizedText = dataObj["recognized_text"].toString();
    QString translatedText = dataObj["translated_text"].toString();
    
    // 插入数据到数据库
    QJsonArray result;
    QString sql = QString("INSERT INTO translations (recognized_text, translated_text, timestamp) VALUES ('%1', '%2', NOW())")
                  .arg(recognizedText.replace("'", "''"))
                  .arg(translatedText.replace("'", "''"));
    
    try {
        result = m_dbWorker->queryData(sql);
    } catch (std::exception& e) {
        qCritical() << "数据库插入失败:" << e.what();
        return createErrorResponse(500, "Database insert failed");
    }
    
    // 构建响应
    HttpResponse response;
    response.statusCode = 201;
    response.statusMessage = "Created";
    response.contentType = "application/json; charset=utf-8";
    
    // 创建响应JSON
    QJsonObject respObj;
    respObj["success"] = true;
    respObj["message"] = "Data saved successfully";
    
    QJsonDocument respDoc(respObj);
    response.content = respDoc.toJson(QJsonDocument::Compact);
    
    return response;
}

RequestHandler::HttpResponse RequestHandler::createErrorResponse(int statusCode, const QString& message)
{
    HttpResponse response;
    response.statusCode = statusCode;
    response.statusMessage = message;
    response.contentType = "application/json; charset=utf-8";
    
    QJsonObject errorObj;
    errorObj["error"] = true;
    errorObj["message"] = message;
    
    QJsonDocument doc(errorObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    return response;
}

RequestHandler::HttpResponse RequestHandler::handleError(int code, const QString& message)
{
    HttpResponse response;
    response.statusCode = code;
    response.contentType = "application/json";
    response.content = QString(R"({"error": "%1"})").arg(message).toUtf8();
    response.headers.insert("Access-Control-Allow-Origin", "*");
    return response;
}


RequestHandler::HttpResponse RequestHandler::handleSwitchPage(const HttpRequest& request)
{
    qDebug() << "处理页面切换请求";
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    QJsonObject resultObj;
    
    // 获取要切换到的页面索引
    int pageIndex = -1;
    if (request.query.contains("index")) {
        bool ok = false;
        pageIndex = request.query["index"].toInt(&ok);
        if (!ok) {
            return createErrorResponse(400, "Invalid page index");
        }
    } else {
        return createErrorResponse(400, "Missing page index");
    }
    
    // 通过信号通知MainWindow切换页面
    emit switchPageRequested(pageIndex);
    
    resultObj["success"] = true;
    resultObj["message"] = "Page switch request sent";
    resultObj["pageIndex"] = pageIndex;
    
    QJsonDocument doc(resultObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    return response;
}

RequestHandler::HttpResponse RequestHandler::handleBackToMain(const HttpRequest& request)
{
    qDebug() << "处理返回主页请求";
    
    HttpResponse response;
    response.statusCode = 200;
    response.statusMessage = "OK";
    response.contentType = "application/json; charset=utf-8";
    
    QJsonObject resultObj;
    
    // 通过信号通知MainWindow返回主页
    emit backToMainRequested();
    
    resultObj["success"] = true;
    resultObj["message"] = "Back to main page request sent";
    
    QJsonDocument doc(resultObj);
    response.content = doc.toJson(QJsonDocument::Compact);
    
    return response;
}