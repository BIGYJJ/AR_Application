#include "Translate.h"
#include <QDebug>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QDateTime>
#include <QCryptographicHash>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QJsonArray>
#include <QAudioFormat>


const QString APP_SECRET  = "6oFULWPILuGRS43WNZHQcKNhIAKXJmud"; // 替换为您的推理接入点ID
const QString API_KEY = "18d5ce83dbec2560"; // 从环境变量获取ARK_API_KEY
const QString WS_URL = "wss://openapi.youdao.com/stream_speech_trans";

// 优化的音频参数
// 简化音频参数，确保与API要求一致
const int SAMPLE_RATE = 16000;
const int CHANNELS = 1;
const int BITS_PER_SAMPLE = 16;
const int CHUNK_DURATION_MS = 40; // 使用短时间的音频块，提高实时性
const int CHUNK_SIZE = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8) * CHUNK_DURATION_MS / 1000;
const int VAD_THRESHOLD = 200; // 根据实际环境调整的语音活动检测阈值
TranslatePage::TranslatePage(const QString &iconPath, QWidget *parent)
    : QWidget(parent), audioSource(nullptr), isRecording(false), accumulatedText(""), currentSequence(0)
{
    // 设置固定窗口尺寸
    setFixedSize(1000, 600);

    // 初始化数据库
    if (!initDatabase()) {
        qDebug() << "数据库初始化失败，将继续但不支持数据库功能";
    }

    // 初始化网络管理器
    networkManager = new QNetworkAccessManager(this);

    // 返回按钮
    backButton = new QPushButton("返回", this);
    backButton->setFixedSize(100, 40);
    backButton->move(20, 500);

    // 使用新的处理方法
    connect(backButton, &QPushButton::clicked, this, &TranslatePage::backButtonClickedHandler);

    // 录音按钮
    recordButton = new QPushButton("开始录音", this);
    recordButton->setFixedSize(100, 40);
    recordButton->move(20, 20);
    connect(recordButton, &QPushButton::clicked, this, &TranslatePage::onRecordButtonClicked);

    // 创建统一文本框 - 替换原来的两个文本框
    unifiedTextEdit = new QTextEdit(this);
    unifiedTextEdit->setReadOnly(true);
    unifiedTextEdit->setGeometry(180, 60, 620, 500);  // 扩大宽度覆盖原来的两个文本框区域
    unifiedTextEdit->setLineWrapMode(QTextEdit::WidgetWidth);
    
    // 设置黑色背景和绿色字体
    QPalette p = unifiedTextEdit->palette();
    p.setColor(QPalette::Base, Qt::black);
    p.setColor(QPalette::Text, Qt::green);
    unifiedTextEdit->setPalette(p);
    
    // 设置字体
    QFont font("Consolas", 12);  // 使用等宽字体以便对齐
    unifiedTextEdit->setFont(font);

    // 目标语言选择
    QLabel *languageLabel = new QLabel("目标语言:", this);
    languageLabel->move(20, 80);
    languageComboBox = new QComboBox(this);
    languageComboBox->move(20, 100);
    languageComboBox->setFixedSize(100, 30);
    
    // 添加更多语言选项
    languageComboBox->addItem("英语", "en");
    languageComboBox->addItem("中文", "zh-CHS");
    languageComboBox->addItem("日语", "ja");
    languageComboBox->addItem("韩语", "ko");
    languageComboBox->addItem("法语", "fr");
    languageComboBox->addItem("西班牙语", "es");

    // 源语言选择
    QLabel *sourceLanguageLabel = new QLabel("源语言:", this);
    sourceLanguageLabel->move(20, 140);
    sourceLanguageComboBox = new QComboBox(this);
    sourceLanguageComboBox->move(20, 160);
    sourceLanguageComboBox->setFixedSize(100, 30);
    
    // 添加源语言选项
    sourceLanguageComboBox->addItem("中文", "zh-CHS");
    sourceLanguageComboBox->addItem("英语", "en");
    sourceLanguageComboBox->addItem("日语", "ja");
    sourceLanguageComboBox->addItem("韩语", "ko");
    sourceLanguageComboBox->addItem("自动检测", "auto");

    // 状态标签
    statusLabel = new QLabel("状态：未连接", this);
    statusLabel->setGeometry(20, 210, 200, 30);

    // 初始化设备组合框
    deviceComboBox = new QComboBox(this);
    deviceComboBox->setGeometry(20, 250, 150, 30);
    
    QLabel *deviceLabel = new QLabel("录音设备:", this);
    deviceLabel->move(20, 230);

    inputDevices = QMediaDevices::audioInputs();
    for (const QAudioDevice &device : inputDevices) {
        deviceComboBox->addItem(device.description());
    }
    connect(deviceComboBox, &QComboBox::currentIndexChanged, this, &TranslatePage::onDeviceChanged);

    // 初始化WebSocket
    webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(webSocket, &QWebSocket::connected, this, &TranslatePage::onConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &TranslatePage::onDisconnected);
    connect(webSocket, &QWebSocket::disconnected, this, [this]() {
        webSocketIsClosed = true;
        statusLabel->setText("状态：已断开");
        qDebug() << "WebSocket已完全关闭";
    });
    connect(webSocket, &QWebSocket::textMessageReceived, this, &TranslatePage::onMessageReceived);
    connect(webSocket, &QWebSocket::errorOccurred, this, &TranslatePage::onError);

    // 初始化定时器，用于定期发送音频数据
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TranslatePage::onTimerTimeout);

    if (inputDevices.isEmpty()) {
        QMessageBox::warning(this, "警告", "未找到可用的录音设备！");
        recordButton->setEnabled(false);
    }
    else {
        // 初始化音频录制器
        initAudioRecorder(inputDevices.first());
    }

    // 静默超时30分钟（1800000ms）
    idleTimer = new QTimer(this);
    idleTimer->setSingleShot(true);
    connect(idleTimer, &QTimer::timeout, this, [this](){
        QMessageBox::information(this, "超时", "静默超时，连接已关闭");
        webSocket->close();
    });

    // 最大连接时长2小时（7200000ms）
    maxDurationTimer = new QTimer(this);
    maxDurationTimer->setSingleShot(true);
    connect(maxDurationTimer, &QTimer::timeout, this, [this](){
        QMessageBox::information(this, "超时", "已达到最大连接时长");
        webSocket->close();
    });

    connect(webSocket, &QWebSocket::sslErrors, [=](const QList<QSslError> &errors){
        qDebug() << "SSL Errors:";
        for (const QSslError &error : errors) {
            qDebug() << "- " << error.errorString();
        }
        // 在开发环境下可以忽略 SSL 错误
        webSocket->ignoreSslErrors();
    });
}

TranslatePage::~TranslatePage()
{
    endSession();
    if (webSocket) {
        webSocket->close();
        delete webSocket;
    }
    if (audioSource) {
        audioSource->stop();
        delete audioSource;
    }
    if (timer) {
        timer->stop();
        delete timer;
    }
    
    // 确保关闭数据库连接
    if (db.isOpen()) {
        db.close();
    }
}

void TranslatePage::onConnected()
{
    isWebSocketConnecting = false;
    qDebug() << "WebSocket connected";
    statusLabel->setText("状态：已连接");
    
    // 输出当前的 WebSocket URL
    qDebug() << "Connected to:" << webSocket->request().url().toString();
    qDebug() << "WebSocket state: " << webSocket->state();
    
    // 确保连接后先发送配置
    QTimer::singleShot(100, this, &TranslatePage::sendSessionUpdate);
    
    // 在session.update发送后，开始录音和处理
    QTimer::singleShot(200, this, [this]() {
        if (isRecording) {
            audioSource->start(&audioBuffer);
            timer->start(40); // 先用小块确保连接稳定
            qDebug() << "Timer started: " << timer->isActive();
            qDebug() << "AudioSource started: " << audioSource->state();
            
            // 重置并启动超时计时器
            idleTimer->stop();
            idleTimer->start(1800000);
            maxDurationTimer->stop();
            maxDurationTimer->start(7200000);
        }
    });
}

void TranslatePage::onDisconnected()
{
    isWebSocketConnecting = false;
    qDebug() << "WebSocket disconnected";
    statusLabel->setText("状态：已断开");
    
    // 重置状态
    if(isRecording) {
        audioSource->stop();
        timer->stop();
        isRecording = false;
        recordButton->setText("开始录音");
    }
    // 停止计时器
    idleTimer->stop();
    maxDurationTimer->stop();
}

void TranslatePage::sendSessionUpdate()
{
     // 根据有道API文档提供正确的会话启动消息
     QJsonObject sessionUpdate;
     sessionUpdate["type"] = "session.update";
     
     QJsonObject session;
     
     // 详细的音频格式
     QJsonObject audioFormat;
     audioFormat["encoding"] = "pcm";  // 修改为 "pcm" 而不是 "pcm16"
     audioFormat["sample_rate_hertz"] = SAMPLE_RATE;
     audioFormat["channels"] = CHANNELS;
     session["input_audio_format"] = audioFormat;
     
     // 简化模态为最基本选项
     session["modalities"] = QJsonArray({"text"});
     
     // 翻译参数
     QJsonObject translation;
     translation["source_language"] = getSourceLanguageCode();
     translation["target_language"] = getTargetLanguageCode();
     
     // 简化词汇表 - 先确保基本功能正常再添加高级特性
     QJsonObject addVocab;
     addVocab["hot_word_list"] = QJsonArray();
     addVocab["glossary_list"] = QJsonArray();
     translation["add_vocab"] = addVocab;
     
     session["input_audio_translation"] = translation;
     sessionUpdate["session"] = session;
     
     QJsonDocument doc(sessionUpdate);
     QString jsonStr = doc.toJson(QJsonDocument::Compact);
     
     webSocket->sendTextMessage(jsonStr);
     qDebug() << "Sent session update:" << jsonStr;
}

void TranslatePage::connectToWebSocket()
{
    if (webSocket->state() != QAbstractSocket::UnconnectedState) {
        qDebug() << "等待WebSocket完全关闭...";
        QTimer::singleShot(100, this, &TranslatePage::connectToWebSocket); // 等待 100ms 后重试
        return;
    }

    statusLabel->setText("状态：连接中...");
    
    QUrl url(WS_URL);
    QMap<QString, QString> paramsMap = createRequestParams();
    QUrlQuery query;
    for (auto it = paramsMap.begin(); it != paramsMap.end(); ++it) {
        query.addQueryItem(it.key(), it.value().toUtf8());
    }
    url.setQuery(query);
    
    qDebug() << "正在连接WebSocket URL:" << url.toString();
    
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    
    // 添加更多的头部信息
    request.setRawHeader("Connection", "Upgrade");
    request.setRawHeader("Upgrade", "websocket");
    
    webSocket->open(request);
    isWebSocketConnecting = true;
    
    // 设置连接超时
    QTimer::singleShot(10000, this, [this]() {
        if (isWebSocketConnecting) {
            webSocket->abort();
            statusLabel->setText("状态：连接超时");
            QMessageBox::warning(this, "警告", "WebSocket连接超时，请检查网络连接后重试");
            isWebSocketConnecting = false;
        }
    });
}

void TranslatePage::resetPage()
{
    if (isRecording) {
        audioSource->stop();
        timer->stop();
        audioBuffer.close();
        isRecording = false;
        recordButton->setText("开始录音");
        sendAudioDone();
        endSession();
    }

    // 关闭并重置 WebSocket
    if (webSocket) {
        webSocket->close();
        delete webSocket;
        webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(webSocket, &QWebSocket::connected, this, &TranslatePage::onConnected);
        connect(webSocket, &QWebSocket::disconnected, this, &TranslatePage::onDisconnected);
        connect(webSocket, &QWebSocket::textMessageReceived, this, &TranslatePage::onMessageReceived);
        connect(webSocket, &QWebSocket::errorOccurred, this, &TranslatePage::onError);
        connect(webSocket, &QWebSocket::disconnected, this, [this]() {
            webSocketIsClosed = true;
            statusLabel->setText("状态：已断开");
            qDebug() << "WebSocket已完全关闭";
        });
        connect(webSocket, &QWebSocket::sslErrors, [=](const QList<QSslError> &errors){
            webSocket->ignoreSslErrors();
        });
    }

    // 重置音频设备
    if (audioSource) {
        audioSource->stop();
        delete audioSource;
        audioSource = nullptr;
    }
    initAudioRecorder(inputDevices.first());

    // 重置基本状态
    currentSequence = 0;
    audioBuffer.close();
    timer->stop();
    
    // 更新状态
    statusLabel->setText("状态：未连接");
}

QMap<QString, QString> TranslatePage::createRequestParams()
{
    QString input = "";
    QString salt = QString::number(QRandomGenerator::global()->generate());
    QString curtime = QString::number(QDateTime::currentDateTime().toSecsSinceEpoch());
    
    // 按照官方要求的顺序拼接参数
    QString signStr = API_KEY + input + salt + curtime + APP_SECRET;
    QString sign = QCryptographicHash::hash(signStr.toUtf8(), QCryptographicHash::Sha256).toHex();

    // 移除可能导致问题的高级参数，只保留基本参数
    return {
        {"from", getSourceLanguageCode()},
        {"to", getTargetLanguageCode()},
        {"rate", QString::number(SAMPLE_RATE)},
        {"format", "wav"},
        {"channel", QString::number(CHANNELS)},
        {"version", "v1"},
        {"appKey", API_KEY},
        {"salt", salt},
        {"sign", sign},
        {"signType", "v4"},
        {"curtime", curtime}
        // 暂时移除needVad和vadOpt参数，等基本功能正常再添加
    };
}

QString TranslatePage::getSourceLanguageCode() const
{
    return sourceLanguageComboBox->currentData().toString();
}

QString TranslatePage::getTargetLanguageCode() const
{
    return languageComboBox->currentData().toString();
}

void TranslatePage::onRecordButtonClicked()
{
    if (!isRecording) {
        qDebug() << "Starting recording...";
        resetPage();
        audioBuffer.open(QIODevice::WriteOnly | QIODevice::Truncate);
        
        // 清除之前的文本，开始新的录音会话
        unifiedTextEdit->clear();
        accumulatedText = "";
        accumulatedRecognizedText = "";
        accumulatedTranslationText = "";
        currentSequence = 0;

        connectToWebSocket();

        audioSource->start(&audioBuffer);
        recordButton->setText("停止录音");
        isRecording = true;
        timer->start(40); // 每 40ms 处理一次音频块
        qDebug() << "Timer started: " << timer->isActive();
        qDebug() << "AudioSource started: " << audioSource->state();

        // 重置并启动超时计时器
        idleTimer->stop();
        idleTimer->start(1800000);
        maxDurationTimer->stop();
        maxDurationTimer->start(7200000);
    } else {
        // 停止录音逻辑
        audioSource->stop();
        timer->stop();
        audioBuffer.close();
        isRecording = false;
        recordButton->setText("开始录音");
        sendAudioDone();
        endSession();
        
        // 保存到数据库 - 只在完成录音时保存当前会话内容
        saveToDatabase();
    }
}

void TranslatePage::onDeviceChanged(int index)
{
    if(index < 0 || index >= inputDevices.size()) return;

    if(isRecording) {
        audioSource->stop();
        timer->stop();
        audioBuffer.close();
        isRecording = false;
        recordButton->setText("开始录音");
    }
    
    initAudioRecorder(inputDevices[index]);
}

void TranslatePage::sendAudioChunk(const QByteArray &chunk)
{
    if (webSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "WebSocket未连接，无法发送音频数据";
        return; 
    }
    
    // 发送音频块
    webSocket->sendBinaryMessage(chunk);
    
    // 重置闲置计时器
    idleTimer->stop();
    idleTimer->start(1800000);
}

// 新增：音频预处理
QByteArray TranslatePage::preprocessAudio(const QByteArray &audioData)
{
    return audioData;
}

void TranslatePage::sendAudioDone()
{
    if (webSocket->state() != QAbstractSocket::ConnectedState) return;
    
    // 告知API语音输入结束
    QJsonObject endMsg{{"end", "true"}};
    webSocket->sendTextMessage(QJsonDocument(endMsg).toJson());
    qDebug() << "发送结束标记";
}

// 更新的消息处理函数，提高解析效率
void TranslatePage::onMessageReceived(const QString &message)
{
    // qDebug() << "Received message:" << message; // 可以注释以减少日志量
    
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        qDebug() << "Invalid JSON received";
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // 检查是否有错误码
    if (obj.contains("errorCode")) {
        QString errorCode = obj["errorCode"].toString();
        if (errorCode != "0") {
            handleError(errorCode);
            return; // 出错时停止处理
        }
    }

    // 检查消息类型
    if (obj.contains("type")) {
        QString type = obj["type"].toString();
        
        if (type == "session.started") {
            qDebug() << "会话已开始";
            return;
        }
        
        if (type == "session.done") {
            // 会话结束，显示最终的识别和翻译结果
            updateUnifiedTextDisplay();
            return;
        }
    }
    
    // 处理识别和翻译结果
    if (obj.contains("result")) {
        QJsonObject resultObj = obj["result"].toObject();
        bool hasNewContent = false;
        
        // 处理翻译结果 - 累积而不是替换
        if (resultObj.contains("tranContent")) {
            QString tranContent = resultObj["tranContent"].toString().trimmed();
            if (!tranContent.isEmpty()) {
                // 检查是否是新内容，避免重复追加同样的内容
                if (!accumulatedTranslationText.endsWith(tranContent)) {
                    // 如果当前内容为空，直接设置；否则，追加空格和新内容
                    if (accumulatedTranslationText.isEmpty()) {
                        accumulatedTranslationText = tranContent;
                    } else {
                        // 根据句子结束标点判断是否需要添加分隔符
                        QChar lastChar = accumulatedTranslationText.at(accumulatedTranslationText.length() - 1);
                        if (lastChar == '.' || lastChar == '?' || lastChar == '!' || 
                            lastChar == '.' || lastChar == '?' || lastChar == '!') {
                            accumulatedTranslationText += " " + tranContent;
                        } else {
                            accumulatedTranslationText += tranContent;
                        }
                    }
                    hasNewContent = true;
                }
            }
        }
        
        // 处理原文识别结果 - 累积而不是替换
        if (resultObj.contains("context")) {
            QString context = resultObj["context"].toString().trimmed();
            if (!context.isEmpty()) {
                // 检查是否是新内容，避免重复追加同样的内容
                if (!accumulatedRecognizedText.endsWith(context)) {
                    // 如果当前内容为空，直接设置；否则，追加空格和新内容
                    if (accumulatedRecognizedText.isEmpty()) {
                        accumulatedRecognizedText = context;
                    } else {
                        // 根据句子结束标点判断是否需要添加分隔符
                        QChar lastChar = accumulatedRecognizedText.at(accumulatedRecognizedText.length() - 1);
                        if (lastChar == '.' || lastChar == '?' || lastChar == '!' || 
                            lastChar == '.' || lastChar == '?' || lastChar == '!') {
                            accumulatedRecognizedText += " " + context;
                        } else {
                            accumulatedRecognizedText += context;
                        }
                    }
                    hasNewContent = true;
                }
            }
        }
        
        // 如果有新内容，更新显示
        if (hasNewContent) {
            updateUnifiedTextDisplay();
        }
    }
}



void TranslatePage::handleError(const QString &errorCode)
{
    QMap<QString, QString> errorMap = {
        {"0", "成功"},
        {"101", "缺少必填参数"},
        {"102", "不支持的语言类型"},
        {"103", "翻译文本过长"},
        {"104", "不支持的API类型"},
        {"105", "不支持的签名类型"},
        {"106", "不支持的响应类型"},
        {"107", "不支持的传输加密类型"},
        {"108", "应用ID无效"},
        {"109", "batchLog格式不正确"},
        {"110", "签名错误"},
        {"111", "无语音数据"},
        {"112", "服务器处理异常"},
        {"113", "查询服务器失败"},
        {"114", "获取结果超时"},
        {"116", "无翻译结果"},
        {"201", "解密失败"},
        {"202", "签名检验失败"},
        {"203", "访问IP地址不在可访问IP列表"},
        {"205", "请求的接口与应用的接口类型不一致"},
        {"206", "因为时间戳无效导致签名校验失败"},
        {"207", "重放请求"},
        {"301", "辞典查询失败"},
        {"302", "翻译查询失败"},
        {"303", "服务端的其它异常"},
        {"304", "会话不存在或已过期"},
        {"305", "会话超时"},
        {"401", "账户已欠费"},
        {"402", "offlinesdk不可用"},
        {"411", "访问频率受限"},
        {"412", "长请求过于频繁"}
    };
    
    QString msg = errorMap.value(errorCode, "未知错误");
    qDebug() << "接收到错误码:" << errorCode << ", 错误信息:" << msg;
    
    QMessageBox::critical(this, "错误", QString("错误码：%1\n%2").arg(errorCode).arg(msg));
    
    // 特定错误的处理
    if (errorCode == "110" || errorCode == "202" || errorCode == "206") {
        QMessageBox::information(this, "签名错误", 
            "请检查APP_KEY和APP_SECRET是否正确，时间戳是否有效。");
    }
    else if (errorCode == "304" || errorCode == "305") {
        QMessageBox::information(this, "会话错误", 
            "会话不存在或已超时，将重新连接。");
        
        QTimer::singleShot(500, this, [this]() {
            resetPage();
            if (isRecording) {
                connectToWebSocket();
            }
        });
    }
}

// 添加专用调试方法，可用于测试连接是否正常
void TranslatePage::debugConnection()
{
    if (webSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "调试: WebSocket未连接";
        return;
    }
    
    // 发送心跳消息测试连接
    QJsonObject pingMsg;
    pingMsg["ping"] = "test";
    webSocket->sendTextMessage(QJsonDocument(pingMsg).toJson());
    qDebug() << "调试: 发送ping测试消息";
}

QString TranslatePage::generateYoudaoSign(const QString &q, const QString &salt, const QString &curtime)
{
    QString input = API_KEY + q + salt + curtime + APP_SECRET;
    return QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha256).toHex();
}

void TranslatePage::endSession()
{
   if (!webSocket || 
        webSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "连接已断开，无需发送session.done";
        return;
    }
    
    QJsonObject sessionDone;
    sessionDone["type"] = "session.done";
    QJsonDocument doc(sessionDone);
    webSocket->sendTextMessage(doc.toJson(QJsonDocument::Compact));
    qDebug() << "已发送会话终止信号";
    currentSequence = 0; 
}

void TranslatePage::onError(QAbstractSocket::SocketError error)
{
    qDebug() << "WebSocket错误代码:" << error;
    qDebug() << "错误详情:" << webSocket->errorString();
    
    // 更新UI
    statusLabel->setText("状态：连接错误");
    
    // 针对特定错误进行处理
    switch (error) {
        case QAbstractSocket::ConnectionRefusedError:
            QMessageBox::critical(this, "错误", "连接被拒绝: " + webSocket->errorString());
            break;
            
        case QAbstractSocket::RemoteHostClosedError:
            QMessageBox::warning(this, "警告", "远程主机关闭连接: " + webSocket->errorString());
            break;
            
        case QAbstractSocket::HostNotFoundError:
            QMessageBox::critical(this, "错误", "找不到主机: " + webSocket->errorString());
            break;
            
        case QAbstractSocket::SocketTimeoutError:
            QMessageBox::warning(this, "警告", "连接超时: " + webSocket->errorString());
            break;
            
        case QAbstractSocket::SslHandshakeFailedError:
            QMessageBox::warning(this, "警告", "SSL握手失败: " + webSocket->errorString());
            webSocket->ignoreSslErrors();
            break;
            
        default:
            QMessageBox::critical(this, "错误", "WebSocket错误: " + webSocket->errorString());
    }
    
    // 3秒后尝试重连
    QTimer::singleShot(3000, this, [this]() {
        if (isRecording) {
            connectToWebSocket();
        }
    });
}

void TranslatePage::onTimerTimeout()
{
    processAudioChunk(false);
}

void TranslatePage::processAudioChunk(bool isFinal)
{
    if (!isRecording) return;
    
    static qint64 lastPos = 0;
    qint64 currentPos = audioBuffer.pos();
    
    if (currentPos <= lastPos) {
        return;
    }
    
    QByteArray chunk = audioBuffer.buffer().mid(lastPos, currentPos - lastPos);
    lastPos = currentPos;
    
    if (chunk.isEmpty()) {
        qDebug() << "Audio chunk is empty, skipping processing";
        return;
    }
    
    // 不做VAD检测，直接发送所有音频数据
    if (webSocket->state() == QAbstractSocket::ConnectedState) {
        sendAudioChunk(chunk);
    } else {
        qDebug() << "WebSocket未连接，无法发送音频块";
    }
    
    // 重置闲置计时器
    idleTimer->stop();
    idleTimer->start(1800000);
}

bool TranslatePage::shouldProcessChunk(const QByteArray& audioData)
{
    const int16_t* samples = reinterpret_cast<const int16_t*>(audioData.constData());
    int numSamples = audioData.size() / sizeof(int16_t);
    
    if (numSamples == 0) return false;
    
    // 计算RMS能量
    double sumSquares = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        sumSquares += static_cast<double>(samples[i]) * samples[i];
    }
    
    double rms = std::sqrt(sumSquares / numSamples);
    
    // 使用RMS能量更准确地检测语音活动
    return rms > VAD_THRESHOLD;
}

void TranslatePage::initAudioRecorder(const QAudioDevice &device)
{
    QAudioFormat format;
    
    // 设置基本音频格式
    format.setSampleRate(SAMPLE_RATE);
    format.setChannelCount(CHANNELS);
    format.setSampleFormat(QAudioFormat::Int16);
    
    qDebug() << "音频格式配置:"
         << "SampleRate=" << format.sampleRate()
         << "Channels=" << format.channelCount()
         << "SampleFormat=" << format.sampleFormat();
    
    if (device.isFormatSupported(format)) {
        if (audioSource) {
            delete audioSource;
        }
        audioSource = new QAudioSource(device, format, this);
        
        // 使用默认缓冲区大小
        int bufferSize = format.bytesForDuration(500000); // 500ms的缓冲区
        audioSource->setBufferSize(bufferSize);
        qDebug() << "设置音频缓冲区大小:" << bufferSize << "字节";
        
    } else {
        QMessageBox::warning(this, "错误", 
            QString("设备 [%1] 不支持格式：%2kHz/%3声道/PCM16")
            .arg(device.description())
            .arg(SAMPLE_RATE / 1000)
            .arg(CHANNELS));
        return;
    }
}

QString TranslatePage::getTimestamp()
{
    return QString::number(QDateTime::currentDateTime().toSecsSinceEpoch());
}

QString TranslatePage::getDate()
{
    return QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd");
}

QString TranslatePage::sha256Hex(const QString& str)
{
    QByteArray hash = QCryptographicHash::hash(
        str.toUtf8(),
        QCryptographicHash::Sha256
    );
    return QString(hash.toHex()).toLower();
}

QByteArray TranslatePage::hmacSha256Raw(const QByteArray& key, const QByteArray& msg)
{
    return QMessageAuthenticationCode::hash(
        msg,
        key,
        QCryptographicHash::Sha256
    );
}

QString TranslatePage::hmacSha256(const QString& key, const QString& input)
{
    QByteArray hashKey = key.toUtf8();
    QByteArray hashData = input.toUtf8();
    
    QByteArray hash = QMessageAuthenticationCode::hash(
        hashData,
        hashKey,
        QCryptographicHash::Sha256
    );
    
    return QString(hash.toHex());
}

QString TranslatePage::generateAuthorization()
{
    // 对于火山引擎Ark API，我们使用Bearer Token认证
    // 从环境变量中获取API密钥
    QString apiKey = qEnvironmentVariable("ARK_API_KEY");
    if (apiKey.isEmpty()) {
        qDebug() << "警告: 未设置ARK_API_KEY环境变量";
        return QString();
    }
    
    return QString("Bearer %1").arg(apiKey);
}



// 修改 updateUnifiedTextDisplay 方法，每12个字符换行
void TranslatePage::updateUnifiedTextDisplay()
{
    // 格式化显示内容：翻译在上，原文在下
    QString formattedTranslation = formatTextWithLineBreaks(accumulatedTranslationText, 30);
    QString formattedRecognized = formatTextWithLineBreaks(accumulatedRecognizedText, 30);
    
    QString displayText = formattedTranslation + "\n\n" + formattedRecognized;
    
    // 更新文本框
    unifiedTextEdit->setText(displayText);
    
    // 滚动到底部
    QTextCursor cursor = unifiedTextEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    unifiedTextEdit->setTextCursor(cursor);
}

// 添加：格式化文本，每 n 个字符换行
QString TranslatePage::formatTextWithLineBreaks(const QString &text, int lineLength)
{
    if (text.isEmpty()) return text;
    
    const int MAX_LINE_LENGTH = 30; // 修改为30字符换行
    QString formattedText;
    int currentLineLength = 0;
    
    // 逐字符处理
    for (int i = 0; i < text.length(); ++i) {
        QChar currentChar = text.at(i);
        formattedText.append(currentChar);
        currentLineLength++;
        
        // 如果是句子结束符，考虑下一个词从新行开始
        if ((currentChar == '.' || currentChar == '?' || currentChar == '!' || 
             currentChar == '.' || currentChar == '?' || currentChar == '!') && 
             currentLineLength >= MAX_LINE_LENGTH * 0.7) { // 如果已达到行长度的70%
            if (i + 1 < text.length() && text.at(i + 1).isSpace()) {
                formattedText.append('\n');
                currentLineLength = 0;
                continue;
            }
        }
        
        // 如果是空格，考虑单词完整性
        if (currentChar.isSpace() && currentLineLength >= MAX_LINE_LENGTH - 5) {
            formattedText.append('\n');
            currentLineLength = 0;
            continue;
        }
        
        // 达到最大长度，换行
        if (currentLineLength >= MAX_LINE_LENGTH) {
            formattedText.append('\n');
            currentLineLength = 0;
        }
    }
    
    return formattedText;
}

// 初始化数据库
bool TranslatePage::initDatabase()
{
    const QString connectionName = "translation_page_mysql_connection";
    
    if (QSqlDatabase::contains(connectionName)) {
        db = QSqlDatabase::database(connectionName);
    } else {
        // 修改这里：使用 QMYSQL 驱动而不是 QSQLITE
        db = QSqlDatabase::addDatabase("QMYSQL", connectionName);
        
        // 设置 MySQL 连接参数
        db.setHostName("localhost");
        db.setPort(3306);
        db.setUserName("root");
        db.setPassword("MyStrongPassword123!");  // 使用与 main.cpp 相同的密码
        db.setDatabaseName("translation_db");
        
        if (!db.open()) {
            qDebug() << "无法连接到 MySQL 数据库:" << db.lastError().text();
            return false;
        }
        qDebug() << "成功连接到 MySQL 数据库";
    }

    // 确保表存在 - 注意这里改为 translations 表而不是 translation_records
    QSqlQuery query(db);
    QString createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS translations (
            id INTEGER PRIMARY KEY AUTO_INCREMENT,
            recognized_text TEXT NOT NULL,
            translated_text TEXT NOT NULL,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    )";
    
    if (!query.exec(createTableSQL)) {
        qDebug() << "创建表失败:" << query.lastError().text();
        return false;
    }

    return true;
}

// 添加: 保存当前翻译记录到数据库
void TranslatePage::saveToDatabase()
{
    if (!db.isOpen()) {
        if (!initDatabase()) {
            qDebug() << "无法保存到数据库: 数据库未连接";
            return;
        }
    }
    
    QString recognizedText = accumulatedRecognizedText.trimmed();
    QString translatedText = accumulatedTranslationText.trimmed();
    
    // 检查是否有内容可保存
    if (recognizedText.isEmpty() && translatedText.isEmpty()) {
        qDebug() << "没有内容可保存到数据库";
        return;
    }
    
    // 插入数据到 MySQL 数据库的 translations 表
    QSqlQuery query(db);
    query.prepare("INSERT INTO translations (recognized_text, translated_text, timestamp) "
                  "VALUES (:recognized_text, :translated_text, NOW())");
    query.bindValue(":recognized_text", recognizedText);
    query.bindValue(":translated_text", translatedText);
    
    if (!query.exec()) {
        qDebug() << "插入数据库失败:" << query.lastError().text();
    } else {
        qDebug() << "成功保存到 MySQL 数据库，ID:" << query.lastInsertId().toInt() 
                 << "，原文长度:" << recognizedText.length() 
                 << "，翻译长度:" << translatedText.length();
    }
}


// 修改: 返回按钮的处理
void TranslatePage::backButtonClickedHandler()
{
    if (isRecording) {
        audioSource->stop();
        if (timer) {
            timer->stop();
        }
        audioBuffer.close();
        isRecording = false;
        sendAudioDone();
        endSession();
    }
    
    // 保存当前会话内容到数据库，然后再返回
    if (!accumulatedRecognizedText.isEmpty() || !accumulatedTranslationText.isEmpty()) {
        saveToDatabase();
    }
    
    resetPage();
    unifiedTextEdit->clear();
    
    // 清除累积的文本内容
    accumulatedText = "";
    accumulatedRecognizedText = ""; 
    accumulatedTranslationText = "";
    
    emit backButtonClicked();
}

