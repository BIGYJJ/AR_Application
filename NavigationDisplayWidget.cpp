#include "NavigationDisplayWidget.h"
#include <QVBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QApplication>
#include <QShowEvent>
#include <QHideEvent>
#include <QDateTime>

NavigationDisplayWidget::NavigationDisplayWidget(QWidget *parent)
    : QWidget(parent),
      m_serverRunning(false),
      m_currentDirection("未设置"),
      m_currentDistance("未知"),
      m_serverPort(8080),
      m_networkManager(new QNetworkAccessManager(this)),
      m_pollTimer(new QTimer(this))
{
    setupUI();

    // 设置轮询定时器
    connect(m_pollTimer, &QTimer::timeout, this, &NavigationDisplayWidget::pollNavData);
    
    // 处理网络请求完成信号
    connect(m_networkManager, &QNetworkAccessManager::finished, this, &NavigationDisplayWidget::handleNetworkReply);
    
    // 设置日志
    qDebug() << "NavigationDisplayWidget构造完成, 端口:" << m_serverPort << "线程ID:" << QThread::currentThreadId();
}

NavigationDisplayWidget::~NavigationDisplayWidget()
{
    stopServer();

    if (m_pollTimer) {
        m_pollTimer->stop();
    }
    
    qDebug() << "NavigationDisplayWidget已销毁";
}

void NavigationDisplayWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    
    // 窗口显示时启动服务器
    qDebug() << "NavigationDisplayWidget显示事件";
    startServer();
}

void NavigationDisplayWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    
    // 窗口隐藏时停止服务器
    qDebug() << "NavigationDisplayWidget隐藏事件";
    stopServer();
}

void NavigationDisplayWidget::setupUI()
{
    // 创建主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(15);
    
    // 标题标签
    m_titleLabel = new QLabel("AR导航显示", this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    
    // 创建方向图像标签
    m_directionImageLabel = new QLabel(this);
    m_directionImageLabel->setMinimumSize(150, 150);
    m_directionImageLabel->setAlignment(Qt::AlignCenter);
    updateDirectionImage("未设置");
    
    // 创建方向标签
    m_directionLabel = new QLabel("方向: 未设置", this);
    m_directionLabel->setAlignment(Qt::AlignCenter);
    QFont directionFont = m_directionLabel->font();
    directionFont.setPointSize(14);
    m_directionLabel->setFont(directionFont);
    
    // 创建返回按钮
    m_backButton = new QPushButton("返回", this);
    connect(m_backButton, &QPushButton::clicked, this, &NavigationDisplayWidget::onBackButtonClicked);
    
    QPushButton* testButton = new QPushButton("测试导航更新", this);
    connect(testButton, &QPushButton::clicked, this, [this]() {
        qDebug() << "手动测试导航更新";
        updateNavigation("测试方向", "测试距离");
    });
    
    // 创建距离标签
    m_distanceLabel = new QLabel("距离: 未知", this);
    m_distanceLabel->setAlignment(Qt::AlignCenter);
    QFont distanceFont = m_distanceLabel->font();
    distanceFont.setPointSize(14);
    m_distanceLabel->setFont(distanceFont);
    
    // 创建状态标签
    m_statusLabel = new QLabel("服务器状态: 未启动", this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    
    // 添加所有部件到布局
    mainLayout->addWidget(m_titleLabel);
    mainLayout->addWidget(m_directionImageLabel);
    mainLayout->addWidget(m_directionLabel);
    mainLayout->addWidget(m_distanceLabel);
    mainLayout->addWidget(m_statusLabel);
    mainLayout->addStretch(1);
    
    // 添加到布局，放在返回按钮前
    mainLayout->addWidget(testButton);
    mainLayout->addWidget(m_backButton, 0, Qt::AlignCenter);
    
    // 设置布局
    setLayout(mainLayout);
    
    // 设置窗口属性
    setWindowTitle("AR导航显示");
    setMinimumSize(400, 500);
}

void NavigationDisplayWidget::startServer()
{
    if (m_serverRunning) {
        qDebug() << "服务器已在运行中";
        return;
    }
    
    qDebug() << "启动导航服务，端口:" << m_serverPort;
    
    // 更新状态
    m_serverRunning = true;
    updateStatusDisplay("服务器已启动");
    
    // 启动轮询定时器
    m_pollTimer->start(5000); // 每5秒轮询一次
    
    // 设置显示信息
    updateDirectionImage("未设置");
    m_directionLabel->setText("方向: 未设置");
    m_distanceLabel->setText("距离: 未知");
    
    emit navigationUpdated("未设置", "未知");
}

void NavigationDisplayWidget::stopServer()
{
    if (!m_serverRunning) {
        qDebug() << "服务器未运行";
        return;
    }
    
    qDebug() << "停止导航服务";
    
    // 停止轮询定时器
    m_pollTimer->stop();
    
    // 更新状态
    m_serverRunning = false;
    updateStatusDisplay("服务器已停止");
    
    // 重置显示信息
    updateDirectionImage("未设置");
    m_directionLabel->setText("方向: 未设置");
    m_distanceLabel->setText("距离: 未知");
    
    emit navigationUpdated("未设置", "未知");
}

void NavigationDisplayWidget::pollNavData()
{
    // 如果服务器未运行，不进行轮询
    if (!m_serverRunning) {
        return;
    }
    
    qDebug() << "正在轮询导航数据，端口:" << m_serverPort;
    
    // 创建轮询请求
    QUrl url(QString("http://localhost:%1/api/navigation/data").arg(m_serverPort));
    QNetworkRequest request(url);
    
    // 添加超时设置
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Connection", "keep-alive");
    
    // 添加时间戳防止缓存
    url.setQuery(QString("t=%1").arg(QDateTime::currentMSecsSinceEpoch()));
    request.setUrl(url);
    
    // 发送请求
    QNetworkReply *reply = m_networkManager->get(request);
    
    // 处理错误
    connect(reply, &QNetworkReply::errorOccurred, [this, reply](QNetworkReply::NetworkError code) {
        QString errorString = reply->errorString();
        qDebug() << "网络错误:" << errorString;
        
        // 如果是连接关闭错误，不需要更新界面状态，这是HTTP正常行为
        if (errorString.contains("Connection closed")) {
            qDebug() << "服务器关闭了连接，这是正常现象";
        } else {
            updateStatusDisplay(QString("错误: %1").arg(errorString));
        }
    });
    
    // 处理完成信号已在构造函数中连接
}

void NavigationDisplayWidget::handleNetworkReply(QNetworkReply *reply)
{
    qDebug() << "收到网络响应";
    
    // 检查回复是否有效
    if (!reply) {
        qDebug() << "网络回复为空";
        return;
    }
    
    // 检查错误
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "网络错误:" << reply->errorString();
        // 连接关闭是HTTP的正常行为，不需要特别处理
        if (!reply->errorString().contains("Connection closed")) {
            updateStatusDisplay(QString("错误: %1").arg(reply->errorString()));
        }
        reply->deleteLater();
        return;
    }
    
    // 处理数据为空的情况
    if (reply->size() == 0) {
        qDebug() << "响应数据为空";
        reply->deleteLater();
        return;
    }
    
    // 获取响应数据
    QByteArray responseData = reply->readAll();
    
    // 输出响应数据（调试用）
    qDebug() << "响应数据:" << responseData;
    
    // 尝试解析JSON
    QJsonParseError jsonError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData, &jsonError);
    
    if (jsonError.error != QJsonParseError::NoError) {
        qDebug() << "JSON解析错误:" << jsonError.errorString();
        qDebug() << "原始数据:" << responseData;
        reply->deleteLater();
        return;
    }
    
    // 确保是一个JSON对象
    if (!jsonDoc.isObject()) {
        qDebug() << "响应不是有效的JSON对象";
        reply->deleteLater();
        return;
    }
    
    QJsonObject navDataObj = jsonDoc.object();
    
    // 检查错误字段
    if (navDataObj.contains("error") && navDataObj["error"].toBool()) {
        QString errorMessage = navDataObj["message"].toString();
        qDebug() << "API返回错误:" << errorMessage;
        updateStatusDisplay(QString("API错误: %1").arg(errorMessage));
        reply->deleteLater();
        return;
    }
    
    // 检查导航数据
    if (navDataObj.contains("direction") && navDataObj.contains("distance")) {
        QString direction = navDataObj["direction"].toString();
        QString distance = navDataObj["distance"].toString();
        bool active = navDataObj.contains("active") ? navDataObj["active"].toBool() : false;
        
        // 只有在状态为活动时才更新导航信息
        if (active) {
            qDebug() << "更新导航信息 - 方向:" << direction << "距离:" << distance;
            updateNavigation(direction, distance);
        } else {
            qDebug() << "导航未激活，不更新显示";
        }
    }
    
    // 更新状态显示
    updateStatusDisplay("数据已更新");
    
    // 释放网络回复对象
    reply->deleteLater();
}

void NavigationDisplayWidget::updateNavigation(const QString &direction, const QString &distance)
{
    qDebug() << "NavigationDisplayWidget::updateNavigation被调用 - 方向:" << direction 
             << "距离:" << distance;

    // 更新成员变量
    m_currentDirection = direction;
    m_currentDistance = distance;

    // 确保在主线程中更新UI
    QMetaObject::invokeMethod(this, [this, direction, distance]() {
        // 直接在lambda中操作UI元素，确保在主线程执行
        m_directionLabel->setText(QString("方向: %1").arg(direction));
        m_distanceLabel->setText(QString("距离: %1").arg(distance));

        // 更新方向图像
        updateDirectionImage(direction);

        // 刷新界面确保显示
        this->update();

        qDebug() << "UI更新完成 - 方向:" << direction << "距离:" << distance;
    }, Qt::QueuedConnection);

    // 发送信号
    emit navigationUpdated(direction, distance);
}

void NavigationDisplayWidget::updateDirectionImage(const QString &direction)
{
    // 创建方向箭头
    QPixmap arrowPixmap = createDirectionArrow(direction);
    
    // 设置到标签
    m_directionImageLabel->setPixmap(arrowPixmap);
}

void NavigationDisplayWidget::updateStatusDisplay(const QString &status)
{
    m_statusLabel->setText(QString("服务器状态: %1").arg(status));
}

void NavigationDisplayWidget::onBackButtonClicked()
{
    emit backButtonClicked();
}

QPixmap NavigationDisplayWidget::createDirectionArrow(const QString &direction)
{
    // 创建一个空白的图像
    QPixmap pixmap(150, 150);
    pixmap.fill(Qt::transparent);
    
    // 创建绘制器
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // 设置画笔
    QPen pen(Qt::black, 2);
    painter.setPen(pen);
    
    // 设置画刷
    QColor arrowColor(41, 128, 185); // 蓝色箭头
    painter.setBrush(arrowColor);
    
    // 绘制不同方向的箭头
    QPolygon arrow;
    if (direction == "直行") {
        // 向上箭头
        arrow << QPoint(75, 15) << QPoint(110, 60) << QPoint(90, 60)
              << QPoint(90, 135) << QPoint(60, 135) << QPoint(60, 60)
              << QPoint(40, 60);
    } else if (direction == "右转") {
        // 向右箭头
        arrow << QPoint(135, 75) << QPoint(90, 40) << QPoint(90, 60)
              << QPoint(25, 60) << QPoint(25, 90) << QPoint(90, 90)
              << QPoint(90, 110);
    } else if (direction == "左转") {
        // 向左箭头
        arrow << QPoint(15, 75) << QPoint(60, 40) << QPoint(60, 60)
              << QPoint(125, 60) << QPoint(125, 90) << QPoint(60, 90)
              << QPoint(60, 110);
    } else if (direction == "掉头") {
        // U型箭头
        painter.drawArc(30, 30, 90, 90, 0, 180 * 16);
        arrow << QPoint(30, 75) << QPoint(60, 45) << QPoint(60, 65)
              << QPoint(90, 65) << QPoint(90, 85) << QPoint(60, 85)
              << QPoint(60, 105);
    } else if (direction == "到达目的地") {
        // 终点标记
        painter.setBrush(Qt::red);
        painter.drawEllipse(50, 45, 50, 50);
        painter.setBrush(Qt::white);
        painter.drawEllipse(60, 55, 30, 30);
        painter.setBrush(Qt::red);
        painter.drawEllipse(70, 65, 10, 10);
        
        // 添加文字
        QFont font = painter.font();
        font.setPointSize(10);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRect(0, 105, 150, 20), Qt::AlignCenter, "目的地");
        
        // 结束绘制
        painter.end();
        return pixmap;
    } else {
        // 默认状态
        painter.drawEllipse(45, 45, 60, 60);
        painter.drawLine(75, 25, 75, 45);
        painter.drawLine(75, 105, 75, 125);
        painter.drawLine(25, 75, 45, 75);
        painter.drawLine(105, 75, 125, 75);
        
        // 添加文字
        QFont font = painter.font();
        font.setPointSize(10);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRect(0, 75, 150, 20), Qt::AlignCenter, "待命");
        
        // 结束绘制
        painter.end();
        return pixmap;
    }
    
    // 绘制箭头多边形
    painter.drawPolygon(arrow);
    
    // 结束绘制
    painter.end();
    
    return pixmap;
}