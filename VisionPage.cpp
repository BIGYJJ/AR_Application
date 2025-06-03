#include "VisionPage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QBuffer>
#include <QImageReader>
#include <QStandardPaths>
#include <QDir>
#include <QUrlQuery>
#include <QDateTime>
#include <QTemporaryFile>
#include <QFileInfo>
#include <QPainter>
#include <QSqlQuery>
#include <QSqlError>

const QString API_URL = "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
const QString API_KEY = "80ef864a-e3ab-4aca-b0d1-bf469f3629a6"; // From the provided example
const QString MODEL_ID = "doubao-1-5-vision-pro-32k-250115"; // From the provided example
const QString DEFAULT_PROMPT = "这是什么场景?请简要描述。"; // Default prompt: "What is this scene? Please describe briefly."

VisionPage::VisionPage(const QString &iconPath, QWidget *parent)
    : QWidget(parent), camera(nullptr), imageCapture(nullptr), isCapturing(false), isProcessingRequest(false),
      overlayLabel(nullptr), overlayHideTimer(nullptr), audioSource(nullptr), isRecording(false),
      webSocket(nullptr), isWebSocketConnecting(false), webSocketIsClosed(true), currentSequence(0),
      recordingStarted(false), cameraResourceAvailable(false), allocatedCameraIndex(-1)
{
    // Set fixed window size
    setFixedSize(1000, 600);

    // Initialize database
    if (!initDatabase()) {
        qDebug() << "数据库初始化失败，将继续但不支持数据库功能";
    }

    // Initialize network manager
    networkManager = new QNetworkAccessManager(this);
    connect(networkManager, &QNetworkAccessManager::finished, this, &VisionPage::onApiRequestFinished);

    // Initialize API configuration
    apiUrl = API_URL;
    apiKey = API_KEY;
    modelId = MODEL_ID;
    prompt = DEFAULT_PROMPT;

    // Create UI components
    backButton = new QPushButton("返回", this);
    backButton->setFixedSize(100, 40);
    backButton->move(20, 500);
    connect(backButton, &QPushButton::clicked, this, &VisionPage::onBackButtonClicked);

    cameraButton = new QPushButton("开始捕获", this);
    cameraButton->setFixedSize(100, 40);
    cameraButton->move(20, 20);
    connect(cameraButton, &QPushButton::clicked, this, &VisionPage::onCameraButtonClicked);

    statusLabel = new QLabel("状态：未连接", this);
    statusLabel->setGeometry(20, 210, 200, 30);

    // Create camera device selection
    QLabel *cameraDeviceLabel = new QLabel("摄像头设备:", this);
    cameraDeviceLabel->move(20, 80);
    cameraDeviceComboBox = new QComboBox(this);
    cameraDeviceComboBox->setGeometry(20, 100, 150, 30);
    connect(cameraDeviceComboBox, &QComboBox::currentIndexChanged, this, &VisionPage::onDeviceChanged);
resourceRetryTimer = new QTimer(this);
    resourceRetryTimer->setSingleShot(true);
    resourceRetryTimer->setInterval(2000); // 2 seconds between retry attempts
    connect(resourceRetryTimer, &QTimer::timeout, this, &VisionPage::retryRequestCameraResource);
    
    // Connect to camera resource manager signals
    auto& cameraManager = CameraResourceManager::instance();
    connect(&cameraManager, &CameraResourceManager::cameraAllocated, 
            this, &VisionPage::onCameraResourceAllocated);
    connect(&cameraManager, &CameraResourceManager::cameraPreempted,
            this, &VisionPage::onCameraResourcePreempted);
    // Create audio device selection
    QLabel *audioDeviceLabel = new QLabel("音频设备:", this);
    audioDeviceLabel->move(20, 140);
    audioDeviceComboBox = new QComboBox(this);
    audioDeviceComboBox->setGeometry(20, 160, 150, 30);
    connect(audioDeviceComboBox, &QComboBox::currentIndexChanged, this, &VisionPage::onAudioDeviceChanged);

    // Create video widget for camera preview
    videoWidget = new QVideoWidget(this);
    videoWidget->setGeometry(180, 20, 640, 480);
    videoWidget->show();

    // Create result text display
    resultTextEdit = new QTextEdit(this);
    resultTextEdit->setReadOnly(true);
    resultTextEdit->setGeometry(830, 20, 150, 480);
    
    // Set black background and green text for result display
    QPalette p = resultTextEdit->palette();
    p.setColor(QPalette::Base, Qt::black);
    p.setColor(QPalette::Text, Qt::green);
    resultTextEdit->setPalette(p);
    
    // Set font
    QFont font("Consolas", 12);
    resultTextEdit->setFont(font);

    // Populate camera devices
    cameraDevices = QMediaDevices::videoInputs();
    for (const QCameraDevice &device : cameraDevices) {
        cameraDeviceComboBox->addItem(device.description());
    }

    // Populate audio devices
    audioDevices = QMediaDevices::audioInputs();
    for (const QAudioDevice &device : audioDevices) {
        audioDeviceComboBox->addItem(device.description());
    }

    // Initialize WebSocket for speech translation - but don't connect yet
    webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(webSocket, &QWebSocket::connected, this, &VisionPage::onWebSocketConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &VisionPage::onWebSocketDisconnected);
    connect(webSocket, &QWebSocket::textMessageReceived, this, &VisionPage::onWebSocketMessageReceived);
    connect(webSocket, &QWebSocket::errorOccurred, this, &VisionPage::onWebSocketError);
    connect(webSocket, &QWebSocket::disconnected, this, [this]() {
        webSocketIsClosed = true;
        statusLabel->setText("状态：语音服务已断开");
        qDebug() << "WebSocket已完全关闭";
    });
    connect(webSocket, &QWebSocket::sslErrors, [=](const QList<QSslError> &errors){
        qDebug() << "SSL Errors:";
        for (const QSslError &error : errors) {
            qDebug() << "- " << error.errorString();
        }
        webSocket->ignoreSslErrors();
    });

    // Setup timers - keep this later in initialization to avoid null references
    captureTimer = new QTimer(this);
    captureTimer->setInterval(3000); // 3 seconds between image captures
    connect(captureTimer, &QTimer::timeout, this, &VisionPage::onTimerTimeout);

    audioTimer = new QTimer(this);
    audioTimer->setInterval(40); // 40ms chunks for audio processing
    connect(audioTimer, &QTimer::timeout, this, &VisionPage::onAudioTimerTimeout);

    silenceTimer = new QTimer(this);
    silenceTimer->setSingleShot(true);
    silenceTimer->setInterval(SILENCE_TIMEOUT_MS); // 2 seconds of silence before triggering image capture
    connect(silenceTimer, &QTimer::timeout, this, &VisionPage::onSilenceTimerTimeout);

    // Idle and max duration timers (from TranslatePage)
    idleTimer = new QTimer(this);
    idleTimer->setSingleShot(true);
    connect(idleTimer, &QTimer::timeout, this, [this](){
        QMessageBox::information(this, "超时", "静默超时，语音服务已关闭");
        if (webSocket && webSocket->state() == QAbstractSocket::ConnectedState) {
            webSocket->close();
        }
    });

    maxDurationTimer = new QTimer(this);
    maxDurationTimer->setSingleShot(true);
    connect(maxDurationTimer, &QTimer::timeout, this, [this](){
        QMessageBox::information(this, "超时", "已达到最大连接时长");
        if (webSocket && webSocket->state() == QAbstractSocket::ConnectedState) {
            webSocket->close();
        }
    });

    // Initialize the first available camera and audio device if any
    if (!cameraDevices.isEmpty()) {
        initCamera(cameraDevices.first());
    } else {
        QMessageBox::warning(this, "警告", "未找到可用的摄像头设备！");
        cameraButton->setEnabled(false);
    }

    if (!audioDevices.isEmpty()) {
        initAudioRecorder(audioDevices.first());
    } else {
        QMessageBox::warning(this, "警告", "未找到可用的音频设备！");
    }

    qDebug() << "VisionPage constructed and ready. Waiting for explicit startRecording() call.";
}

VisionPage::~VisionPage()
{
    // Stop all active processes
    if (recordingStarted) {
        stopRecording();
    }
     releaseCameraResource();
    
    // Close WebSocket
    if (webSocket) {
        if (webSocket->state() == QAbstractSocket::ConnectedState) {
            webSocket->close();
        }
        delete webSocket;
        webSocket = nullptr;
    }
    
    // Stop audio recording
    if (audioSource) {
        audioSource->stop();
        delete audioSource;
        audioSource = nullptr;
    }
    
    // Stop camera
    if (camera) {
        camera->stop();
        delete camera;
        camera = nullptr;
    }
    
    // Stop all timers
    if (captureTimer) {
        captureTimer->stop();
        delete captureTimer;
        captureTimer = nullptr;
    }
    
    if (audioTimer) {
        audioTimer->stop();
        delete audioTimer;
        audioTimer = nullptr;
    }
    
    if (silenceTimer) {
        silenceTimer->stop();
        delete silenceTimer;
        silenceTimer = nullptr;
    }
    
    if (idleTimer) {
        idleTimer->stop();
        delete idleTimer;
        idleTimer = nullptr;
    }
    
    if (maxDurationTimer) {
        maxDurationTimer->stop();
        delete maxDurationTimer;
        maxDurationTimer = nullptr;
    }
    
    if (overlayHideTimer) {
        overlayHideTimer->stop();
        delete overlayHideTimer;
        overlayHideTimer = nullptr;
    }

      if (resourceRetryTimer) {
        resourceRetryTimer->stop();
        delete resourceRetryTimer;
        resourceRetryTimer = nullptr;
    }
}

// Camera Methods

void VisionPage::initCamera(const QCameraDevice &device)
{
    qDebug() << "VisionPage: 调用initCamera, 切换为使用资源管理器";
    
    // Find the index of this device
    int deviceIndex = -1;
    for (int i = 0; i < cameraDevices.size(); i++) {
        if (cameraDevices[i].id() == device.id()) {
            deviceIndex = i;
            break;
        }
    }
    
    if (deviceIndex >= 0) {
        // Request resource for this device
        requestCameraResource(deviceIndex);
    } else {
        qWarning() << "VisionPage: 未找到设备索引, 使用默认值0";
        requestCameraResource(0);
    }
}

void VisionPage::startCapturing()
{
    if (!camera) {
        qDebug() << "Error: camera is null in startCapturing";
        QMessageBox::warning(this, "错误", "摄像头未初始化");
        return;
    }
    
    if (!cameraResourceAvailable) {
        qDebug() << "Error: camera resource not available in startCapturing";
        QMessageBox::warning(this, "错误", "摄像头资源不可用");
        return;
    }

    isCapturing = true;
    cameraButton->setText("停止捕获");
    statusLabel->setText("状态：等待语音输入...");
    
    // We don't start the captureTimer here - it will be triggered by silenceTimer
    
    // Make sure silence timer is started
    if (silenceTimer && !silenceTimer->isActive()) {
        silenceTimer->start(SILENCE_TIMEOUT_MS);
    }
}

void VisionPage::stopCapturing()
{
    isCapturing = false;
    cameraButton->setText("开始捕获");
    captureTimer->stop();
    silenceTimer->stop();
    statusLabel->setText("状态：摄像头已就绪");
}

void VisionPage::onTimerTimeout()
{
    captureAndSendImage();
}

void VisionPage::captureAndSendImage()
{
    if (!camera || !imageCapture) {
        QMessageBox::warning(this, "错误", "摄像头未初始化");
        return;
    }

    // Create directory for saving images if it doesn't exist
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/VisionApp");
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // Generate filename for the captured image
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz");
    currentImagePath = dir.absolutePath() + "/" + timestamp + ".jpg";

    // Capture the image
    imageCapture->captureToFile(currentImagePath);
}

void VisionPage::onImageCaptured(int id, const QImage &image)
{
    // This is called when an image is captured but before it's saved to disk
    Q_UNUSED(id);
    Q_UNUSED(image);
}

void VisionPage::onImageSaved(int id, const QString &fileName)
{
    Q_UNUSED(id);
    
    // Add image to queue for processing
    pendingImages.enqueue(fileName);
    
    // Process the next image if not already processing
    if (!isProcessingRequest) {
        processNextImageInQueue();
    }
}

void VisionPage::processNextImageInQueue()
{
    if (pendingImages.isEmpty()) {
        isProcessingRequest = false;
        return;
    }
    
    isProcessingRequest = true;
    QString imagePath = pendingImages.dequeue();
    sendImageToApi(imagePath);
}

void VisionPage::sendImageToApi(const QString &imagePath)
{
    QFile imageFile(imagePath);
    if (!imageFile.exists()) {
        qDebug() << "Error: Image file does not exist: " << imagePath;
        isProcessingRequest = false;
        processNextImageInQueue();
        return;
    }

    // Convert image to base64
    QByteArray imageBase64 = imageToBase64(imagePath);
    if (imageBase64.isEmpty()) {
        qDebug() << "Error: Failed to convert image to base64";
        isProcessingRequest = false;
        processNextImageInQueue();
        return;
    }

    // Store the current image path for the response handler
    currentImagePath = imagePath;

    // Prepare API request
    QNetworkRequest request{QUrl{apiUrl}};  // Using brace initialization to avoid vexing parse
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(apiKey).toUtf8());

    // Store the current prompt before sending (for logging and database)
    QString currentPrompt = accumulatedRecognizedText;
    
    // Debug output for prompt
    qDebug() << "Using prompt for image recognition: " << currentPrompt;
    
    // Construct the JSON payload
    QJsonObject mainObject;
    mainObject["model"] = modelId;

    QJsonArray messagesArray;
    QJsonObject messageObject;
    messageObject["role"] = "user";

    QJsonArray contentArray;
    
    // Add image content
    QJsonObject imageContent;
    imageContent["type"] = "image_url";
    
    QJsonObject imageUrlObject;
    // Use data URL format for the image
    QString dataUrl = "data:image/jpeg;base64," + QString(imageBase64);
    imageUrlObject["url"] = dataUrl;
    
    imageContent["image_url"] = imageUrlObject;
    contentArray.append(imageContent);
    
    // Add text prompt
    QJsonObject textContent;
    textContent["type"] = "text";
    textContent["text"] = currentPrompt;
    contentArray.append(textContent);
    
    messageObject["content"] = contentArray;
    messagesArray.append(messageObject);
    mainObject["messages"] = messagesArray;

    QJsonDocument jsonDoc(mainObject);
    QByteArray jsonData = jsonDoc.toJson();

    // Send the request
    statusLabel->setText("状态：正在分析图像");
    networkManager->post(request, jsonData);
    
    // Clear the accumulated translation AFTER we've used it in the request
    accumulatedTranslationText = "";
    updateTranslationDisplay();
}

QByteArray VisionPage::imageToBase64(const QString &imagePath)
{
    QFile file(imagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    QByteArray imageData = file.readAll();
    file.close();

    return imageData.toBase64();
}

void VisionPage::onApiRequestFinished(QNetworkReply *reply)
{
    QString result;
    
    if (reply->error() != QNetworkReply::NoError) {
        statusLabel->setText("状态：API请求失败");
        result = "API错误: " + reply->errorString();
    } else {
        // Read and parse the response
        QByteArray responseData = reply->readAll();
        QJsonDocument jsonResponse = QJsonDocument::fromJson(responseData);
        
        if (jsonResponse.isNull() || !jsonResponse.isObject()) {
            statusLabel->setText("状态：无效的响应");
            result = "无效的API响应";
        } else {
            // Process the response
            result = extractResultFromResponse(jsonResponse);
            
            // Update status to show we're waiting for the next speech input
            statusLabel->setText("状态：等待语音输入...");
        }
    }

    // Update display with result
    updateResultDisplay(result);
    
    // Save to database
    saveToDatabase(currentImagePath, result);
    
    // Clean up and process next image
    reply->deleteLater();
    isProcessingRequest = false;
    processNextImageInQueue();
    
    // At this point, accumulatedTranslationText should already be empty (cleared in sendImageToApi)
    // We're now waiting for new speech input to generate a new prompt
    
    // Start the silence timer - but no image will be captured until a new prompt is available
    if (silenceTimer) {
        silenceTimer->start(SILENCE_TIMEOUT_MS);
    }
}

QString VisionPage::extractResultFromResponse(const QJsonDocument &response)
{
    QJsonObject responseObj = response.object();
    
    // Extract the response text from the API response
    if (responseObj.contains("choices") && responseObj["choices"].isArray()) {
        QJsonArray choices = responseObj["choices"].toArray();
        if (!choices.isEmpty() && choices[0].isObject()) {
            QJsonObject firstChoice = choices[0].toObject();
            if (firstChoice.contains("message") && firstChoice["message"].isObject()) {
                QJsonObject message = firstChoice["message"].toObject();
                if (message.contains("content") && message["content"].isString()) {
                    return message["content"].toString();
                }
            }
        }
    }
    
    return "无法解析API响应";
}

void VisionPage::updateResultDisplay(const QString &result)
{
    // Update text display
    resultTextEdit->setText(result);
    
    // Make sure the latest result is visible
    resultTextEdit->moveCursor(QTextCursor::End);
    resultTextEdit->ensureCursorVisible();
    
    // Also overlay text on video for better visibility
    overlayTextOnVideo(result);
}

void VisionPage::overlayTextOnVideo(const QString &text)
{
    // Create overlay label if it doesn't exist yet
    if (!overlayLabel) {
        overlayLabel = new QLabel(videoWidget);
        overlayLabel->setAlignment(Qt::AlignBottom | Qt::AlignLeft);
        overlayLabel->setWordWrap(true);
        overlayLabel->setStyleSheet(
            "QLabel { background-color: rgba(0, 0, 0, 160); color: white; "
            "padding: 10px; border-radius: 5px; font-size: 14px; }");
    }
    
    // Format text for display (limit length for readability)
    QString displayText = text;
    if (displayText.length() > 150) {
        displayText = displayText.left(147) + "...";
    }
    
    // Replace multiple newlines with single newline for compact display
    displayText.replace(QRegularExpression("\n{2,}"), "\n");
    
    // Update overlay text
    overlayLabel->setText(displayText);
    
    // Resize and position the overlay
    int labelWidth = videoWidget->width() - 20;
    overlayLabel->setMaximumWidth(labelWidth);
    overlayLabel->adjustSize();
    
    // Position at the bottom of the video widget
    overlayLabel->move(10, videoWidget->height() - overlayLabel->height() - 10);
    
    // Make sure overlay is visible
    overlayLabel->raise();
    overlayLabel->show();
    
    // Set up hide timer if it doesn't exist
    if (!overlayHideTimer) {
        overlayHideTimer = new QTimer(this);
        overlayHideTimer->setSingleShot(true);
        connect(overlayHideTimer, &QTimer::timeout, this, [this]() {
            if (this->overlayLabel) {
                this->overlayLabel->hide();
            }
        });
    }
    
    // Reset and start the timer to hide overlay after 5 seconds
    overlayHideTimer->stop();
    overlayHideTimer->start(5000);
}

// Audio Processing Methods (adapted from TranslatePage)

void VisionPage::initAudioRecorder(const QAudioDevice &device)
{
    QAudioFormat format;
    
    // Set basic audio format
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
        
        // Use default buffer size
        int bufferSize = format.bytesForDuration(500000); // 500ms buffer
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

void VisionPage::onAudioDeviceChanged(int index)
{
    if (index < 0 || index >= audioDevices.size()) return;

    if (isRecording) {
        audioSource->stop();
        audioTimer->stop();
        audioBuffer.close();
        isRecording = false;
        
        // If WebSocket is connected, send audio done and end session
        if (webSocket->state() == QAbstractSocket::ConnectedState) {
            sendAudioDone();
            endSession();
        }
    }
    
    initAudioRecorder(audioDevices[index]);
    
    // Reopen audio buffer and reconnect to WebSocket
    audioBuffer.open(QIODevice::WriteOnly | QIODevice::Truncate);
    connectToWebSocket();
}

void VisionPage::connectToWebSocket()
{
    qDebug() << "Attempting to connect to WebSocket...";
    
    if (!webSocket) {
        qDebug() << "Error: WebSocket is null in connectToWebSocket";
        return;
    }
    
    if (webSocket->state() != QAbstractSocket::UnconnectedState) {
        qDebug() << "等待WebSocket完全关闭...";
        QTimer::singleShot(100, this, &VisionPage::connectToWebSocket); // Retry after 100ms
        return;
    }

    if (statusLabel) {
        statusLabel->setText("状态：正在连接语音服务...");
    }
    
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
    
    // Add additional header information
    request.setRawHeader("Connection", "Upgrade");
    request.setRawHeader("Upgrade", "websocket");
    
    webSocket->open(request);
    isWebSocketConnecting = true;
    
    // Set connection timeout
    QTimer::singleShot(10000, this, [this]() {
        if (isWebSocketConnecting && webSocket) {
            webSocket->abort();
            if (statusLabel) {
                statusLabel->setText("状态：语音服务连接超时");
            }
            QMessageBox::warning(this, "警告", "WebSocket连接超时，请检查网络连接后重试");
            isWebSocketConnecting = false;
        }
    });
}

void VisionPage::onWebSocketConnected()
{
    isWebSocketConnecting = false;
    qDebug() << "WebSocket connected";
    statusLabel->setText("状态：语音服务已连接");
    
    // Output current WebSocket URL
    qDebug() << "Connected to:" << webSocket->request().url().toString();
    qDebug() << "WebSocket state: " << webSocket->state();
    
    // Ensure configuration is sent first after connection
    QTimer::singleShot(100, this, &VisionPage::sendSessionUpdate);
    
    // After sending session.update, start recording and processing
    QTimer::singleShot(200, this, [this]() {
        if (audioSource) {
            audioSource->start(&audioBuffer);
            isRecording = true;
            if (audioTimer) {
                audioTimer->start(40); // Start with small chunks to ensure stable connection
                qDebug() << "Timer started: " << audioTimer->isActive();
            }
            qDebug() << "AudioSource started: " << audioSource->state();
            
            // Reset and start timeout timers
            if (idleTimer) {
                idleTimer->stop();
                idleTimer->start(1800000); // 30 minutes idle timeout
            }
            
            if (maxDurationTimer) {
                maxDurationTimer->stop();
                maxDurationTimer->start(7200000); // 2 hours max duration
            }
            
            // Start silence timer
            if (silenceTimer) {
                silenceTimer->start(SILENCE_TIMEOUT_MS);
            }
        } else {
            qDebug() << "Error: audioSource is null in onWebSocketConnected";
        }
    });
}

void VisionPage::onWebSocketDisconnected()
{
    isWebSocketConnecting = false;
    qDebug() << "WebSocket disconnected";
    statusLabel->setText("状态：语音服务已断开");
    
    // Reset state
    if (isRecording) {
        audioSource->stop();
        audioTimer->stop();
        isRecording = false;
    }
    
    // Stop timers
    idleTimer->stop();
    maxDurationTimer->stop();
    silenceTimer->stop();
}

void VisionPage::sendSessionUpdate()
{
    // Provide correct session start message according to Youdao API documentation
    QJsonObject sessionUpdate;
    sessionUpdate["type"] = "session.update";
     
    QJsonObject session;
     
    // Detailed audio format
    QJsonObject audioFormat;
    audioFormat["encoding"] = "pcm";  // Changed to "pcm" instead of "pcm16"
    audioFormat["sample_rate_hertz"] = SAMPLE_RATE;
    audioFormat["channels"] = CHANNELS;
    session["input_audio_format"] = audioFormat;
     
    // Simplify modalities to basic options
    session["modalities"] = QJsonArray({"text"});
     
    // Translation parameters
    QJsonObject translation;
    translation["source_language"] = getSourceLanguageCode();
    translation["target_language"] = getTargetLanguageCode();
     
    // Simplify vocabulary - ensure basic functionality works before adding advanced features
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

QMap<QString, QString> VisionPage::createRequestParams()
{
    QString input = "";
    QString salt = QString::number(QRandomGenerator::global()->generate());
    QString curtime = QString::number(QDateTime::currentDateTime().toSecsSinceEpoch());
    
    // Concatenate parameters in the order required by official API
    QString signStr = SPEECH_API_KEY + input + salt + curtime + APP_SECRET;
    QString sign = QCryptographicHash::hash(signStr.toUtf8(), QCryptographicHash::Sha256).toHex();

    // Create and populate the map manually instead of using an initializer list
    QMap<QString, QString> params;
    params.insert("from", getSourceLanguageCode());
    params.insert("to", getTargetLanguageCode());
    params.insert("rate", QString::number(SAMPLE_RATE));
    params.insert("format", "wav");
    params.insert("channel", QString::number(CHANNELS));
    params.insert("version", "v1");
    params.insert("appKey", SPEECH_API_KEY);
    params.insert("salt", salt);
    params.insert("sign", sign);
    params.insert("signType", "v4");
    params.insert("curtime", curtime);
    
    return params;
}

QString VisionPage::getSourceLanguageCode() const
{
    // For simplicity, we'll use fixed language codes here
    return "zh-CHS"; // Chinese
}

QString VisionPage::getTargetLanguageCode() const
{
    // For simplicity, we'll use fixed language codes here
    return "en"; // English
}

QString VisionPage::generateYoudaoSign(const QString &q, const QString &salt, const QString &curtime)
{
    QString input = SPEECH_API_KEY + q + salt + curtime + APP_SECRET;
    return QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha256).toHex();
}

void VisionPage::onAudioTimerTimeout()
{
    processAudioChunk(false);
}

void VisionPage::processAudioChunk(bool isFinal)
{
    if (!isRecording || !audioSource) {
        return;
    }
    
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
    
    // Check if we should process this chunk (has audio)
    bool hasAudio = shouldProcessChunk(chunk);
    
    // If there's audio, reset the silence timer
    if (hasAudio) {
        if (silenceTimer) {
            silenceTimer->stop();
            silenceTimer->start(SILENCE_TIMEOUT_MS);
        }
        statusLabel->setText("状态：检测到语音输入");
    }
    
    // Send all audio data without VAD detection
    if (webSocket && webSocket->state() == QAbstractSocket::ConnectedState) {
        sendAudioChunk(chunk);
    } else {
        qDebug() << "WebSocket未连接，无法发送音频块";
    }
    
    // Reset idle timer
    if (idleTimer) {
        idleTimer->stop();
        idleTimer->start(1800000);
    }
}

bool VisionPage::shouldProcessChunk(const QByteArray& audioData)
{
    const int16_t* samples = reinterpret_cast<const int16_t*>(audioData.constData());
    int numSamples = audioData.size() / sizeof(int16_t);
    
    if (numSamples == 0) return false;
    
    // Calculate RMS energy
    double sumSquares = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        sumSquares += static_cast<double>(samples[i]) * samples[i];
    }
    
    double rms = std::sqrt(sumSquares / numSamples);
    
    // Use RMS energy to more accurately detect voice activity
    return rms > SILENCE_THRESHOLD;
}

void VisionPage::sendAudioChunk(const QByteArray &chunk)
{
    if (webSocket->state() != QAbstractSocket::ConnectedState) {
        qDebug() << "WebSocket未连接，无法发送音频数据";
        return; 
    }
    
    // Send audio chunk
    webSocket->sendBinaryMessage(chunk);
    
    // Reset idle timer
    idleTimer->stop();
    idleTimer->start(1800000);
}

void VisionPage::sendAudioDone()
{
    if (webSocket->state() != QAbstractSocket::ConnectedState) return;
    
    // Inform API that voice input is finished
    QJsonObject endMsg{{"end", "true"}};
    webSocket->sendTextMessage(QJsonDocument(endMsg).toJson());
    qDebug() << "发送结束标记";
}

void VisionPage::endSession()
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

void VisionPage::onWebSocketError(QAbstractSocket::SocketError error)
{
    qDebug() << "WebSocket错误代码:" << error;
    qDebug() << "错误详情:" << webSocket->errorString();
    
    // Update UI
    statusLabel->setText("状态：语音服务连接错误");
    
    // Handle specific errors
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
    
    // Try to reconnect after 3 seconds
    QTimer::singleShot(3000, this, [this]() {
        if (isRecording) {
            connectToWebSocket();
        }
    });
}



void VisionPage::updateTranslationDisplay()
{
    // Display recognized text and translation in the result text edit
    QString displayText = "";
    
    if (!accumulatedRecognizedText.isEmpty()) {
        displayText += "语音输入: " + accumulatedRecognizedText + "\n\n";
    }
    
    if (!accumulatedTranslationText.isEmpty()) {
        displayText += "提示语: " + accumulatedTranslationText;
    } else {
        displayText += "提示语: <等待语音输入>";
    }
    
    resultTextEdit->setText(displayText);
    
    // Scroll to bottom
    QTextCursor cursor = resultTextEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    resultTextEdit->setTextCursor(cursor);
}

void VisionPage::handleTranslationError(const QString &errorCode)
{
    // Create error map manually instead of using initializer lists
    QMap<QString, QString> errorMap;
    errorMap.insert("0", "成功");
    errorMap.insert("101", "缺少必填参数");
    errorMap.insert("102", "不支持的语言类型");
    errorMap.insert("103", "翻译文本过长");
    errorMap.insert("104", "不支持的API类型");
    errorMap.insert("105", "不支持的签名类型");
    errorMap.insert("106", "不支持的响应类型");
    errorMap.insert("107", "不支持的传输加密类型");
    errorMap.insert("108", "应用ID无效");
    errorMap.insert("109", "batchLog格式不正确");
    errorMap.insert("110", "签名错误");
    errorMap.insert("111", "无语音数据");
    errorMap.insert("112", "服务器处理异常");
    errorMap.insert("113", "查询服务器失败");
    errorMap.insert("114", "获取结果超时");
    errorMap.insert("116", "无翻译结果");
    errorMap.insert("201", "解密失败");
    errorMap.insert("202", "签名检验失败");
    errorMap.insert("203", "访问IP地址不在可访问IP列表");
    errorMap.insert("205", "请求的接口与应用的接口类型不一致");
    errorMap.insert("206", "因为时间戳无效导致签名校验失败");
    errorMap.insert("207", "重放请求");
    errorMap.insert("301", "辞典查询失败");
    errorMap.insert("302", "翻译查询失败");
    errorMap.insert("303", "服务端的其它异常");
    errorMap.insert("304", "会话不存在或已过期");
    errorMap.insert("305", "会话超时");
    errorMap.insert("401", "账户已欠费");
    errorMap.insert("402", "offlinesdk不可用");
    errorMap.insert("411", "访问频率受限");
    errorMap.insert("412", "长请求过于频繁");
    
    QString msg = errorMap.value(errorCode, "未知错误");
    qDebug() << "接收到错误码:" << errorCode << ", 错误信息:" << msg;
    
    QMessageBox::critical(this, "语音服务错误", QString("错误码：%1\n%2").arg(errorCode).arg(msg));
    
    // Handle specific errors
    if (errorCode == "110" || errorCode == "202" || errorCode == "206") {
        QMessageBox::information(this, "签名错误", 
            "请检查APP_KEY和APP_SECRET是否正确，时间戳是否有效。");
    }
    else if (errorCode == "304" || errorCode == "305") {
        QMessageBox::information(this, "会话错误", 
            "会话不存在或已超时，将重新连接。");
        
        QTimer::singleShot(500, this, [this]() {
            if (isRecording) {
                if (webSocket) {
                    webSocket->close();
                }
                connectToWebSocket();
            }
        });
    }
}

void VisionPage::onSilenceTimerTimeout()
{
    // Safety check to prevent crashes
    if (!camera || !imageCapture) {
        qDebug() << "Camera or imageCapture is null in onSilenceTimerTimeout";
        silenceTimer->start(SILENCE_TIMEOUT_MS);
        return;
    }

    // If capturing mode is active, not processing a request, AND we have a valid translation to use as prompt
    if (isCapturing && !isProcessingRequest && !accumulatedTranslationText.isEmpty()) {
        statusLabel->setText("状态：检测到静默，捕获图像...");
        qDebug() << "Silence detected, capturing image with prompt: " << accumulatedTranslationText;
        captureAndSendImage();
        
        // Don't restart the silence timer - it will be restarted when new speech is detected
    } else {
        qDebug() << "Silence detected but ";
        if (!isCapturing) {
            qDebug() << "capture mode is not active";
        } else if (isProcessingRequest) {
            qDebug() << "still processing previous request";
        } else if (accumulatedTranslationText.isEmpty()) {
            qDebug() << "no translation available to use as prompt";
            statusLabel->setText("状态：等待语音输入...");
        }
        
        // Restart the silence timer - we'll keep checking periodically
        silenceTimer->start(SILENCE_TIMEOUT_MS);
    }
}

// UI and Control Methods

void VisionPage::onDeviceChanged(int index)
{
    if (index < 0 || index >= cameraDevices.size()) return;

    // Release existing resource
    releaseCameraResource();
    
    // Request resource for new device
    requestCameraResource(index);
    
    // Update state
    if (recordingStarted && cameraResourceAvailable && !isCapturing) {
        startCapturing();
    }
}

void VisionPage::onBackButtonClicked()
{
    qDebug() << "Back button clicked, stopping recording and cleaning up resources...";
    
    // Stop all processes
    stopRecording();
    
    qDebug() << "Resources cleaned up, emitting backButtonClicked signal";
    emit backButtonClicked();
}

void VisionPage::onCameraButtonClicked()
{
    if (!isCapturing) {
        // Only handle camera button clicks if we've already started recording
        if (!recordingStarted) {
            QMessageBox::information(this, "提示", "请等待页面完全加载后再操作");
            return;
        }
        
        startCapturing();
    } else {
        stopCapturing();
    }
}

void VisionPage::resetPage()
{
   // Stop capturing
    if (isCapturing) {
        stopCapturing();
    }

    // Release camera resource
    releaseCameraResource();

    // Stop audio
    if (isRecording) {
        audioSource->stop();
        audioTimer->stop();
        audioBuffer.close();
        isRecording = false;
    }

    // Close and reset WebSocket
    if (webSocket) {
        webSocket->close();
        delete webSocket;
        webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(webSocket, &QWebSocket::connected, this, &VisionPage::onWebSocketConnected);
        connect(webSocket, &QWebSocket::disconnected, this, &VisionPage::onWebSocketDisconnected);
        connect(webSocket, &QWebSocket::textMessageReceived, this, &VisionPage::onWebSocketMessageReceived);
        connect(webSocket, &QWebSocket::errorOccurred, this, &VisionPage::onWebSocketError);
        connect(webSocket, &QWebSocket::disconnected, this, [this]() {
            webSocketIsClosed = true;
            statusLabel->setText("状态：语音服务已断开");
            qDebug() << "WebSocket已完全关闭";
        });
        connect(webSocket, &QWebSocket::sslErrors, [=](const QList<QSslError> &errors){
            webSocket->ignoreSslErrors();
        });
    }

    // Clear pending images
    pendingImages.clear();
    isProcessingRequest = false;

    // Clear display
    resultTextEdit->clear();
    if (overlayLabel) {
        overlayLabel->hide();
    }

    // Reset accumulated text
    accumulatedTranslationText = "";
    accumulatedRecognizedText = "";
    
    // Request camera resource again if needed
    if (recordingStarted && !cameraResourceAvailable) {
        requestCameraResource(0);
    }
    
    // Re-init audio if available
    if (!audioDevices.isEmpty()) {
        initAudioRecorder(audioDevices.first());
    }

    // Update status
    statusLabel->setText("状态：未连接");
    
    // Reset timers
    silenceTimer->stop();
    captureTimer->stop();
    idleTimer->stop();
    maxDurationTimer->stop();
    resourceRetryTimer->stop();
    
    // Reset sequence counter
    currentSequence = 0;
}

bool VisionPage::initDatabase()
{
    const QString connectionName = "vision_connection";
    
    if (QSqlDatabase::contains(connectionName)) {
        db = QSqlDatabase::database(connectionName);
    } else {
        db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        
        // Ensure data directory exists
        QDir dataDir(QDir::homePath() + "/.vision");
        if (!dataDir.exists()) {
            dataDir.mkpath(".");
        }
        
        // Set database file path
        db.setDatabaseName(dataDir.absolutePath() + "/vision.db");
        
        if (!db.open()) {
            qDebug() << "无法连接到数据库:" << db.lastError().text();
            return false;
        }
        qDebug() << "成功连接到数据库";
    }

    // Ensure table exists
    QSqlQuery query(db);
    QString createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS vision_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            image_path TEXT NOT NULL,
            recognition_result TEXT NOT NULL,
            prompt TEXT NOT NULL
        )
    )";
    
    if (!query.exec(createTableSQL)) {
        qDebug() << "创建表失败:" << query.lastError().text();
        return false;
    }

    return true;
}

void VisionPage::saveToDatabase(const QString &imagePath, const QString &result)
{
    if (!db.isOpen()) {
        if (!initDatabase()) {
            qDebug() << "无法保存到数据库: 数据库未连接";
            return;
        }
    }
    
    // Get current timestamp
    QDateTime currentTime = QDateTime::currentDateTime();
    
    // Use prompt if available or default prompt
    QString usedPrompt = accumulatedTranslationText.isEmpty() ? prompt : accumulatedTranslationText;
    
    // Insert data into database
    QSqlQuery query(db);
    query.prepare("INSERT INTO vision_records (timestamp, image_path, recognition_result, prompt) "
                  "VALUES (:timestamp, :image_path, :recognition_result, :prompt)");
    query.bindValue(":timestamp", currentTime.toString("yyyy-MM-dd HH:mm:ss"));
    query.bindValue(":image_path", imagePath);
    query.bindValue(":recognition_result", result);
    query.bindValue(":prompt", usedPrompt);
    
    if (!query.exec()) {
        qDebug() << "插入数据库失败:" << query.lastError().text();
    } else {
        qDebug() << "成功保存到数据库，ID:" << query.lastInsertId().toInt();
    }
}

void VisionPage::onWebSocketMessageReceived(const QString &message)
{
    // Skip processing if not recording
    if (!isRecording) {
        return;
    }

    // Parse JSON response
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        qDebug() << "Invalid JSON received";
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // Check for error code
    if (obj.contains("errorCode")) {
        QString errorCode = obj["errorCode"].toString();
        if (errorCode != "0") {
            handleTranslationError(errorCode);
            return; // Stop processing when error occurs
        }
    }

    // Check message type
    if (obj.contains("type")) {
        QString type = obj["type"].toString();
        
        if (type == "session.started") {
            qDebug() << "会话已开始";
            return;
        }
        
        if (type == "session.done") {
            // Session ended, display final recognition and translation results
            updateTranslationDisplay();
            return;
        }
    }
    
    // Process recognition and translation results
    if (obj.contains("result")) {
        QJsonObject resultObj = obj["result"].toObject();
        bool hasNewContent = false;
        
        // Process translation result - accumulate instead of replace
        if (resultObj.contains("tranContent")) {
            QString tranContent = resultObj["tranContent"].toString().trimmed();
            if (!tranContent.isEmpty()) {
                // Check if it's new content to avoid repeating the same content
                if (!accumulatedTranslationText.endsWith(tranContent)) {
                    // If current content is empty, set directly; otherwise, append space and new content
                    if (accumulatedTranslationText.isEmpty()) {
                        accumulatedTranslationText = tranContent;
                    } else {
                        // Determine whether to add separator based on punctuation
                        if (accumulatedTranslationText.length() > 0) {
                            QChar lastChar = accumulatedTranslationText.at(accumulatedTranslationText.length() - 1);
                            if (lastChar == '.' || lastChar == '?' || lastChar == '!' || 
                                lastChar == '.' || lastChar == '?' || lastChar == '!') {
                                accumulatedTranslationText += " " + tranContent;
                            } else {
                                accumulatedTranslationText += tranContent;
                            }
                        } else {
                            accumulatedTranslationText = tranContent;
                        }
                    }
                    hasNewContent = true;
                }
            }
        }
        
        // Process original text recognition result - accumulate instead of replace
        if (resultObj.contains("context")) {
            QString context = resultObj["context"].toString().trimmed();
            if (!context.isEmpty()) {
                // Check if it's new content to avoid repeating the same content
                if (!accumulatedRecognizedText.endsWith(context)) {
                    // If current content is empty, set directly; otherwise, append space and new content
                    if (accumulatedRecognizedText.isEmpty()) {
                        accumulatedRecognizedText = context;
                    } else {
                        // Determine whether to add separator based on punctuation
                        if (accumulatedRecognizedText.length() > 0) {
                            QChar lastChar = accumulatedRecognizedText.at(accumulatedRecognizedText.length() - 1);
                            if (lastChar == '.' || lastChar == '?' || lastChar == '!' || 
                                lastChar == '.' || lastChar == '?' || lastChar == '!') {
                                accumulatedRecognizedText += " " + context;
                            } else {
                                accumulatedRecognizedText += context;
                            }
                        } else {
                            accumulatedRecognizedText = context;
                        }
                    }
                    hasNewContent = true;
                }
            }
        }
        
        // If there's new content, update display
        if (hasNewContent) {
            updateTranslationDisplay();
            
            // Update status to show we have a prompt ready
            statusLabel->setText("状态：翻译中...");
            
            // Reset silence timer to ensure we don't capture before speech ends
            // This ensures we wait for 2 seconds of silence after the latest speech
            if (silenceTimer) {
                silenceTimer->stop();
                silenceTimer->start(SILENCE_TIMEOUT_MS);
            }
        }
    }
}// Add implementation of startRecording and stopRecording methods
void VisionPage::startRecording()
{
    qDebug() << "Starting recording and image capture...";
    
    // Check if recording has already been started to prevent multiple starts
    if (recordingStarted) {
        qDebug() << "Recording already started, ignoring request";
        return;
    }
    
    // First, request camera resource
    if (!cameraResourceAvailable) {
        bool requestSucceeded = requestCameraResource(0);
        if (!requestSucceeded) {
            qDebug() << "Failed to request camera resource, will retry automatically";
            statusLabel->setText("状态：正在等待摄像头资源...");
            // Resource allocation will be handled in onCameraResourceAllocated callback
        }
    }
    
    if (audioSource) {
        // Clear any previous accumulated text
        accumulatedRecognizedText = "";
        accumulatedTranslationText = "";
        resultTextEdit->clear();
        
        // Reset processing state
        pendingImages.clear();
        isProcessingRequest = false;
        
        // Open audio buffer
        audioBuffer.open(QIODevice::WriteOnly | QIODevice::Truncate);
        
        // Connect to WebSocket
        connectToWebSocket();
        
        // Start image capture mode if camera is available
        if (cameraResourceAvailable && !isCapturing) {
            startCapturing();
        }
        
        // Mark recording as started
        recordingStarted = true;
        
        statusLabel->setText("状态：正在启动语音服务...");
        qDebug() << "Recording started successfully";
    } else {
        qDebug() << "Failed to start recording: audioSource is null";
        QMessageBox::warning(this, "错误", "麦克风设备未初始化，无法启动录音");
    }
}

void VisionPage::stopRecording()
{
   qDebug() << "Stopping all recording and capture processes...";
    
    // If recording was never started, no need to do anything
    if (!recordingStarted) {
        qDebug() << "Recording was never started, nothing to stop";
        return;
    }
    
    // Stop image capturing
    stopCapturing();
    
    // Stop audio recording
    if (isRecording && audioSource) {
        audioSource->stop();
        if (audioTimer) {
            audioTimer->stop();
        }
        audioBuffer.close();
        isRecording = false;
        
        // End WebSocket session
        if (webSocket && webSocket->state() == QAbstractSocket::ConnectedState) {
            sendAudioDone();
            endSession();
        }
    }
    
    // Release camera resource
    releaseCameraResource();
    
    // Clear any pending requests
    pendingImages.clear();
    isProcessingRequest = false;
    
    // Stop all timers
    if (silenceTimer) silenceTimer->stop();
    if (captureTimer) captureTimer->stop();
    if (idleTimer) idleTimer->stop();
    if (maxDurationTimer) maxDurationTimer->stop();
    if (overlayHideTimer) overlayHideTimer->stop();
    if (resourceRetryTimer) resourceRetryTimer->stop();
    
    // Reset recording started flag
    recordingStarted = false;
    
    // Update status
    statusLabel->setText("状态：已停止");
    qDebug() << "All recording processes stopped";
}
// New method to request camera resource
bool VisionPage::requestCameraResource(int preferredIndex)
{
    qDebug() << "VisionPage: 请求摄像头资源, 首选索引:" << preferredIndex;
    
    // Get the resource manager instance
    auto& cameraManager = CameraResourceManager::instance();
    
    // Create a resource request
    CameraRequest request;
    request.requesterId = "VisionPage";
    request.priority = RequestPriority::Normal;
    request.preferredCameraIndex = preferredIndex;
    request.exclusive = true;
    request.notifyTarget = this;
    request.notifyMethod = "onCameraResourceAllocated";
    
    // Make the request
    bool success = cameraManager.requestCamera(request);
    
    qDebug() << "VisionPage: 摄像头资源请求结果:" << success;
    
    // The actual result will be handled in onCameraResourceAllocated
    return success;
}

// New method to handle resource allocation callback
void VisionPage::onCameraResourceAllocated(const QString& requesterId, int cameraIndex, bool success)
{
    // Only handle our own allocation requests
    if (requesterId != "VisionPage") {
        return;
    }
    
    if (success) {
        qDebug() << "VisionPage: 摄像头资源分配成功, 索引:" << cameraIndex;
        cameraResourceAvailable = true;
        allocatedCameraIndex = cameraIndex;
        
        // Initialize the camera with allocated index
        safelyInitCamera(cameraIndex);
        
        // Update UI
        statusLabel->setText("状态：摄像头已就绪");
        cameraButton->setEnabled(true);
    } else {
        qDebug() << "VisionPage: 摄像头资源分配失败";
        cameraResourceAvailable = false;
        allocatedCameraIndex = -1;
        
        // Update UI
        statusLabel->setText("状态：摄像头资源不可用");
        cameraButton->setEnabled(false);
        
        // Schedule retry
        if (!resourceRetryTimer->isActive()) {
            resourceRetryTimer->start();
        }
    }
}

// New method to handle resource preemption
void VisionPage::onCameraResourcePreempted(const QString& requesterId)
{
    if (requesterId != "VisionPage") {
        return; // Not our request
    }
    
    qDebug() << "VisionPage: 摄像头资源被抢占";
    
    // Update state
    cameraResourceAvailable = false;
    
    // Stop the camera safely
    safelyStopCamera();
    
    // Update UI
    statusLabel->setText("状态：摄像头资源被其他应用抢占");
    cameraButton->setEnabled(false);
    
    // Notify user
    QMessageBox::warning(this, "摄像头不可用", 
        "摄像头资源已被其他应用程序抢占。\n识别功能将暂时不可用。");
    
    // Emit signal
    emit cameraPreempted();
    
    // Schedule retry if recording is still active
    if (recordingStarted && !resourceRetryTimer->isActive()) {
        resourceRetryTimer->start();
    }
}

// New method to release camera resource
void VisionPage::releaseCameraResource()
{
    qDebug() << "VisionPage: 释放摄像头资源";
    
    // First, safely stop the camera
    safelyStopCamera();
    
    // Then release the resource
    auto& cameraManager = CameraResourceManager::instance();
    cameraManager.releaseCamera("VisionPage");
    
    // Update state
    cameraResourceAvailable = false;
    allocatedCameraIndex = -1;
}

// New method to safely init camera
void VisionPage::safelyInitCamera(int cameraIndex)
{
    qDebug() << "VisionPage: 安全初始化摄像头, 索引:" << cameraIndex;
    
    try {
        // Clean up existing camera if any
        if (camera) {
            camera->stop();
            delete camera;
            camera = nullptr;
        }
        
        // Hide the overlay label if it exists
        if (overlayLabel) {
            overlayLabel->hide();
        }
        
        // Get available cameras
        const auto cameras = QMediaDevices::videoInputs();
        
        // Check if the index is valid
        if (cameraIndex < 0 || cameraIndex >= cameras.size()) {
            qWarning() << "VisionPage: 无效的摄像头索引:" << cameraIndex;
            statusLabel->setText("状态：无效的摄像头索引");
            return;
        }
        
        // Get the camera device
        QCameraDevice device = cameras[cameraIndex];
        
        // Create new camera
        camera = new QCamera(device, this);
        
        // Setup capture session
        captureSession.setCamera(camera);
        captureSession.setVideoOutput(videoWidget);
        
        // Setup image capture
        imageCapture = new QImageCapture(this);
        captureSession.setImageCapture(imageCapture);
        
        // Connect signals
        connect(imageCapture, &QImageCapture::imageCaptured, this, &VisionPage::onImageCaptured);
        connect(imageCapture, &QImageCapture::imageSaved, this, &VisionPage::onImageSaved);
        
        // Start the camera for preview
        camera->start();
        statusLabel->setText("状态：摄像头已就绪");
        
    } catch (const std::exception& e) {
        qWarning() << "VisionPage: 初始化摄像头时发生异常:" << e.what();
        statusLabel->setText(QString("状态：摄像头初始化错误: %1").arg(e.what()));
    } catch (...) {
        qWarning() << "VisionPage: 初始化摄像头时发生未知异常";
        statusLabel->setText("状态：摄像头初始化错误");
    }
}

// New method to safely stop camera
void VisionPage::safelyStopCamera()
{
    qDebug() << "VisionPage: 安全停止摄像头";
    
    try {
        // Stop capturing if active
        if (isCapturing) {
            stopCapturing();
        }
        
        // Stop the camera
        if (camera) {
            if (camera->isActive()) {
                camera->stop();
            }
            
            // Wait briefly to ensure camera is fully stopped
            QThread::msleep(200);
            
            // Delete camera object
            delete camera;
            camera = nullptr;
        }
        
        // Clear image capture
        if (imageCapture) {
            delete imageCapture;
            imageCapture = nullptr;
        }
        
        qDebug() << "VisionPage: 摄像头已安全停止";
        
    } catch (const std::exception& e) {
        qWarning() << "VisionPage: 停止摄像头时发生异常:" << e.what();
    } catch (...) {
        qWarning() << "VisionPage: 停止摄像头时发生未知异常";
    }
}

// New method to retry requesting camera resource
void VisionPage::retryRequestCameraResource()
{
    qDebug() << "VisionPage: 重试请求摄像头资源";
    
    // Only retry if recording is active
    if (recordingStarted) {
        requestCameraResource(0); // Try default camera index
    }
}

