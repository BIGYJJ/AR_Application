#include "TextRecognitionWidget.h"
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QScrollArea>
#include <QRandomGenerator>
#include <cmath>

// 配置常量 - 移除WS_URL的定义，因为它已经在WebSocketConnectionHandler.h中定义
const int SAMPLE_RATE = 16000;
const int CHANNELS = 1;
const int BITS_PER_SAMPLE = 16;
const int CHUNK_DURATION_MS = 80; // 音频块处理间隔

TextRecognitionWidget::TextRecognitionWidget(QWidget *parent)
    : QWidget(parent)
    , m_camera(nullptr)
    , m_captureSession(nullptr)
    , m_videoSink(nullptr)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_isProcessingRequest(false)
    , m_lastRequestTime(QTime::currentTime())
    , m_minRequestInterval(5000) // 5秒最小间隔，避免过度调用API
    , m_webSocketHandler(new WebSocketConnectionHandler(this))
    , m_audioSource(nullptr)
    , m_isRecording(false)
    , m_hasVoiceResult(false)
    , m_isChineseTarget(false)
    , m_hasSpeech(false)
    , m_silenceThreshold(200)
    , m_currentAudioDeviceIndex(-1) // 默认使用第一个设备
    , m_autoStarted(false) // 初始化为未自动启动
    , m_cameraInitialized(false) // 添加此行，标记摄像头初始化状态
{
    setupVolcanoEngineAPI();
    setupUi();
    // DO NOT INITIALIZE CAMERA HERE
    // setupCamera(); - Remove this line
    setupMicrophoneSelection(); 
    setupWebSocketHandler();    
    setupAudioRecording(false); 
    printf("测试1\n");
    // 初始化静音检测计时器
    m_silenceDetectionTimer = new QTimer(this);
    m_silenceDetectionTimer->setInterval(500); // 500ms检查一次静音状态
    connect(m_silenceDetectionTimer, &QTimer::timeout, this, [this]() {
        if (m_isRecording && m_hasSpeech && m_silenceTimer.elapsed() > 2000) {
            // 如果有语音后静音超过2秒，尝试触发图像识别
            qDebug() << "检测到2秒静音，考虑触发图像识别";
            m_hasSpeech = false;
            if (m_hasVoiceResult && m_isChineseTarget) {
                checkMicrophoneInactivity();
            }
        }
    });
    
    // 初始化silence timer
    m_silenceTimer.start();
    
    // 连接网络响应信号
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &TextRecognitionWidget::handleImageRecognitionResponse);
    
    // 显示初始提示，告知用户需要点击开始
    m_recognitionTextDisplay->setText("<p style='color:green;'>"
                                     "系统已准备就绪<br><br>"
                                     "页面加载完成，现在可以开始使用<br><br>"
                                     "识别将在页面完全显示后自动开始<br>"
                                     "当翻译目标语言为中文时，会自动进行图像识别<br>"
                                     "识别结果将显示在此区域</p>");
    m_cameraInitialized = false;
}

void TextRecognitionWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    
    if (!m_cameraInitialized) {
        // Delay setup by a small amount to ensure UI is ready
        QTimer::singleShot(100, this, [this]() {
            setupCamera();
            m_cameraInitialized = true;
            // Start recognition after camera is ready
            startRecognition();
        });
    }
}
TextRecognitionWidget::~TextRecognitionWidget()
{
    // 停止所有活动
    stopRecognition();
    
    // 释放摄像头资源
    if (m_camera) {
        m_camera->stop();
        // 等待资源释放
        QThread::msleep(500);
    }
    
    // 通过摄像头管理器显式释放资源
    auto& cameraManager = CameraResourceManager::instance();
    cameraManager.releaseCamera("TextRecognition");
    
    if (m_audioSource) {
        m_audioSource->stop();
    }
    
    // 停止所有计时器
    if (m_audioProcessTimer) m_audioProcessTimer->stop();
    if (m_micInactivityTimer) m_micInactivityTimer->stop();
    if (m_silenceDetectionTimer) m_silenceDetectionTimer->stop();
    
    // 断开WebSocket连接
    if (m_webSocketHandler) {
        m_webSocketHandler->disconnectFromServer();
    }
}

void TextRecognitionWidget::setupVolcanoEngineAPI()
{
    // 设置火山引擎API相关配置
    m_volcanoConfig.apiKey = "80ef864a-e3ab-4aca-b0d1-bf469f3629a6";  // 替换为您实际的API Key
    m_volcanoConfig.endpoint = "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
    m_volcanoConfig.model = "doubao-1-5-vision-pro-32k-250115";
    
    qDebug() << "火山引擎图像识别API配置已完成";
}

void TextRecognitionWidget::setupUi()
{
    // 创建主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);  // 移除边距
    
    // 创建文本显示区域 - 使用QTextEdit替代QLabel以支持滚动
    m_recognitionTextDisplay = new QLabel(this);
    m_recognitionTextDisplay->setAlignment(Qt::AlignCenter);
    m_recognitionTextDisplay->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_recognitionTextDisplay->setMinimumSize(640, 480);
    m_recognitionTextDisplay->setStyleSheet("background-color: black; color: green; font-size: 20px; padding: 20px;");
    m_recognitionTextDisplay->setWordWrap(true);
    m_recognitionTextDisplay->setTextFormat(Qt::RichText); // 支持富文本格式化
    
    // 使用QScrollArea包装以支持滚动
    QScrollArea *scrollArea = new QScrollArea(this);
    scrollArea->setWidget(m_recognitionTextDisplay);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet("background-color: black; border: none;");
    
    // 创建进度指示器（显示状态）
    m_statusLabel = new QLabel("状态: 正在初始化...", this);
    m_statusLabel->setStyleSheet("color: green; background-color: black; padding: 5px;");
    
    // 设置控制区域的布局
    QVBoxLayout *controlLayout = new QVBoxLayout();
    
    // 创建语言选择下拉框
    QHBoxLayout *languageLayout = new QHBoxLayout();
    
    QLabel *sourceLabel = new QLabel("源语言:", this);
    sourceLabel->setStyleSheet("color: green;");
    m_sourceLanguageComboBox = new QComboBox(this);
    m_sourceLanguageComboBox->addItem("自动检测", "auto");
    m_sourceLanguageComboBox->addItem("中文", "zh-CHS");
    m_sourceLanguageComboBox->addItem("英语", "en");
    m_sourceLanguageComboBox->addItem("日语", "ja");
    m_sourceLanguageComboBox->addItem("韩语", "ko");
    m_sourceLanguageComboBox->setStyleSheet("color: green; background-color: black; border: 1px solid green;");
    
    QLabel *targetLabel = new QLabel("目标语言:", this);
    targetLabel->setStyleSheet("color: green;");
    m_targetLanguageComboBox = new QComboBox(this);
    m_targetLanguageComboBox->addItem("中文", "zh-CHS");
    m_targetLanguageComboBox->addItem("英语", "en");
    m_targetLanguageComboBox->addItem("日语", "ja");
    m_targetLanguageComboBox->addItem("韩语", "ko");
    m_targetLanguageComboBox->setStyleSheet("color: green; background-color: black; border: 1px solid green;");
    
    // 处理语言变更
    connect(m_targetLanguageComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int index) {
                m_isChineseTarget = (m_targetLanguageComboBox->currentData().toString() == "zh-CHS");
                qDebug() << "目标语言切换为:" << m_targetLanguageComboBox->currentText() 
                        << " (中文目标=" << (m_isChineseTarget ? "是" : "否") << ")";
                
                // 如果WebSocket已连接，需要重新连接以更新语言设置
                if (m_webSocketHandler && m_webSocketHandler->isConnected()) {
                    qDebug() << "重新连接WebSocket以应用新的语言设置";
                    resetAudioConnection();
                }
            });
    
    // 默认选择中文作为目标语言
    m_targetLanguageComboBox->setCurrentIndex(0);
    m_isChineseTarget = true;
    
    languageLayout->addWidget(sourceLabel);
    languageLayout->addWidget(m_sourceLanguageComboBox);
    languageLayout->addWidget(targetLabel);
    languageLayout->addWidget(m_targetLanguageComboBox);
    languageLayout->addStretch();
    
    // 创建麦克风设备选择（稍后在setupMicrophoneSelection中填充）
    QHBoxLayout *microphoneLayout = new QHBoxLayout();
    QLabel *microphoneLabel = new QLabel("麦克风设备:", this);
    microphoneLabel->setStyleSheet("color: green;");
    m_microphoneComboBox = new QComboBox(this);
    m_microphoneComboBox->setMinimumWidth(200);
    m_microphoneComboBox->setStyleSheet("color: green; background-color: black; border: 1px solid green;");
    
    QPushButton *refreshMicButton = new QPushButton("刷新设备", this);
    refreshMicButton->setStyleSheet("QPushButton { color: green; background-color: black; border: 1px solid green; padding: 5px; }");
    connect(refreshMicButton, &QPushButton::clicked, this, &TextRecognitionWidget::refreshAudioDeviceList);
    
    microphoneLayout->addWidget(microphoneLabel);
    microphoneLayout->addWidget(m_microphoneComboBox);
    microphoneLayout->addWidget(refreshMicButton);
    microphoneLayout->addStretch();
    
    // 创建返回按钮
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    backButton = new QPushButton("返回", this);
    backButton->setStyleSheet("QPushButton { color: green; background-color: black; border: 1px solid green; padding: 5px; }");
    buttonLayout->addStretch();
    buttonLayout->addWidget(backButton);
    
    // 连接返回按钮信号
    connect(backButton, &QPushButton::clicked, this, &TextRecognitionWidget::onBackButtonClicked);
    
    // 将控制布局添加到主布局
    controlLayout->addLayout(languageLayout);
    controlLayout->addLayout(microphoneLayout);
    controlLayout->addLayout(buttonLayout);
    
    // 将所有组件添加到布局中
    mainLayout->addWidget(scrollArea);
    mainLayout->addWidget(m_statusLabel);
    mainLayout->addLayout(controlLayout);
    
    // 设置整体黑色背景
    setStyleSheet("background-color: black;");
    
    // 设置窗口属性
    setLayout(mainLayout);
    setWindowTitle("智能多模态翻译识别");
    resize(800, 600);
    
    // 创建定时器用于检测麦克风不活动
    m_micInactivityTimer = new QTimer(this);
    m_micInactivityTimer->setSingleShot(true);
    connect(m_micInactivityTimer, &QTimer::timeout, this, &TextRecognitionWidget::checkMicrophoneInactivity);
    
    // 显示初始提示，添加关于连接时间的说明
    m_recognitionTextDisplay->setText("<p style='color:green;'>"
                                     "系统正在启动，正在连接语音识别服务...<br><br>"
                                     "首次连接可能需要10-30秒，请耐心等待<br><br>"
                                     "连接成功后可对着麦克风说话<br>"
                                     "当翻译目标语言为中文时，会自动进行图像识别<br>"
                                     "识别结果将显示在此区域</p>");
}

// 设置麦克风选择功能
void TextRecognitionWidget::setupMicrophoneSelection()
{
    // 刷新音频设备列表
    refreshAudioDeviceList();
    
    // 连接信号槽
    connect(m_microphoneComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TextRecognitionWidget::onMicrophoneDeviceChanged);
}

// 设置WebSocket处理器
void TextRecognitionWidget::setupWebSocketHandler()
{
    // 连接WebSocketConnectionHandler的信号
    connect(m_webSocketHandler, &WebSocketConnectionHandler::connected, 
            this, &TextRecognitionWidget::handleWebSocketConnected);
    
    connect(m_webSocketHandler, &WebSocketConnectionHandler::disconnected, 
            this, &TextRecognitionWidget::handleWebSocketDisconnected);
    
    connect(m_webSocketHandler, &WebSocketConnectionHandler::textRecognized,
            this, &TextRecognitionWidget::handleRecognizedText);
    
    connect(m_webSocketHandler, &WebSocketConnectionHandler::textTranslated,
            this, &TextRecognitionWidget::handleTranslatedText);
    
    connect(m_webSocketHandler, &WebSocketConnectionHandler::stateChanged,
            this, &TextRecognitionWidget::handleConnectionStateChanged);
    
    connect(m_webSocketHandler, &WebSocketConnectionHandler::connectionFailed,
            this, &TextRecognitionWidget::handleConnectionFailed);
    
    connect(m_webSocketHandler, &WebSocketConnectionHandler::logMessage,
            this, &TextRecognitionWidget::handleWebSocketLog);
    
    // 设置音频格式
    m_webSocketHandler->setAudioFormat(SAMPLE_RATE, CHANNELS);
}

// 处理WebSocket连接成功
void TextRecognitionWidget::handleWebSocketConnected()
{
    qDebug() << "WebSocket成功连接";
    m_statusLabel->setText("状态: 语音识别服务已连接，请开始说话");
    
    // 更新UI提示
    m_recognitionTextDisplay->setText("<p style='color:green;'>"
                                    "✅ 语音识别服务已连接成功！<br><br>"
                                    "请对着麦克风说话...<br>"
                                    "当翻译目标语言为中文时，会自动进行图像识别<br>"
                                    "识别结果将显示在此区域</p>");
    
    // 延迟500毫秒后开始音频处理
    QTimer::singleShot(500, this, [this]() {
        // 确保缓冲区已打开并清空
        if (m_audioBuffer.isOpen()) {
            m_audioBuffer.buffer().clear();
            m_audioBuffer.seek(0);
        } else {
            m_audioBuffer.open(QIODevice::WriteOnly | QIODevice::Truncate);
        }
        
        // 重置语音状态
        resetVoiceActivity();
        
        // 开始录音
        m_audioSource->start(&m_audioBuffer);
        m_audioProcessTimer->start(CHUNK_DURATION_MS);
        m_isRecording = true;
        
        // 启动静音检测定时器
        m_silenceDetectionTimer->start();
        
        qDebug() << "开始录音和处理";
    });
}

// 处理WebSocket断开连接
void TextRecognitionWidget::handleWebSocketDisconnected()
{
    qDebug() << "WebSocket断开连接";
    m_statusLabel->setText("状态: 语音识别服务已断开");
    
    // 停止录音和处理
    if (m_isRecording) {
        qDebug() << "停止录音";
        m_audioSource->stop();
        m_audioProcessTimer->stop();
        m_silenceDetectionTimer->stop();
    }
}

// 刷新音频设备列表
void TextRecognitionWidget::refreshAudioDeviceList()
{
    qDebug() << "刷新音频设备列表...";
    
    // 临时保存当前选择的设备名称
    QString currentDeviceName;
    if (m_microphoneComboBox->count() > 0 && m_currentAudioDeviceIndex < m_microphoneComboBox->count()) {
        currentDeviceName = m_microphoneComboBox->currentText();
    }
    
    // 清空现有列表
    m_microphoneComboBox->clear();
    
    // 获取可用的音频输入设备
    m_audioInputDevices = QMediaDevices::audioInputs();
    
    if (m_audioInputDevices.isEmpty()) {
        qWarning() << "没有找到可用的录音设备!";
        m_microphoneComboBox->addItem("无可用设备", -1);
        m_microphoneComboBox->setEnabled(false);
        QMessageBox::warning(this, "警告", "未找到可用的录音设备，语音识别功能将不可用");
        return;
    }
    
    // 添加设备到下拉框
    int selectedIndex = 0;
    for (int i = 0; i < m_audioInputDevices.size(); ++i) {
        const QAudioDevice &device = m_audioInputDevices.at(i);
        QString deviceName = device.description();
        m_microphoneComboBox->addItem(deviceName, i);
        
        // 如果是之前选中的设备，则选中它
        if (deviceName == currentDeviceName) {
            selectedIndex = i;
        }
        
        qDebug() << "找到音频设备" << i << ":" << deviceName;
    }
    
    // 恢复之前选择的设备（如果存在）
    if (!currentDeviceName.isEmpty()) {
        m_microphoneComboBox->setCurrentIndex(selectedIndex);
    }
    
    m_microphoneComboBox->setEnabled(true);
}

// 麦克风设备更改处理
void TextRecognitionWidget::onMicrophoneDeviceChanged(int index)
{
    if (index < 0 || index >= m_audioInputDevices.size()) {
        qDebug() << "无效的设备索引:" << index;
        return;
    }
    
    m_currentAudioDeviceIndex = index;
    const QAudioDevice &selectedDevice = m_audioInputDevices.at(index);
    qDebug() << "切换麦克风设备到:" << selectedDevice.description();
    
    // 如果正在录音，需要重新初始化录音设备
    if (m_isRecording) {
        qDebug() << "正在重新初始化录音设备...";
        resetAudioConnection();
    } else {
        // 如果没有录音，只需要重新初始化音频源
        if (m_audioSource) {
            m_audioSource->stop();
            delete m_audioSource;
            m_audioSource = nullptr;
        }
        
        // 初始化新的音频源
        initAudioSource(selectedDevice);
    }
    
    // 更新状态
    m_statusLabel->setText("状态: 已切换麦克风设备 - " + selectedDevice.description());
}

void TextRecognitionWidget::setupCamera()
{
    qDebug() << "TextRecognitionWidget: 设置摄像头 (开始)";
    
    // 使用摄像头管理器获取摄像头资源
    auto& cameraManager = CameraResourceManager::instance();
    
    // 创建高优先级请求
    CameraRequest request;
    request.requesterId = "TextRecognition";
    request.priority = RequestPriority::High;
    request.preferredCameraIndex = 0;  // 明确使用video0
    request.exclusive = true;
    request.notifyTarget = this;
    request.notifyMethod = "onCameraAllocated";
    
    // 请求摄像头资源
    if (!cameraManager.requestCamera(request)) {
        qWarning() << "TextRecognitionWidget: 无法立即获取摄像头资源，等待分配";
        m_statusLabel->setText("状态: 等待摄像头资源...");
        return;
    }
    
    // 摄像头资源获取成功，继续设置
    onCameraAllocated(true, 0);  // 强制使用索引0
}

void TextRecognitionWidget::setupAudioRecording(bool autoConnect)
{
    // 创建音频处理定时器
    m_audioProcessTimer = new QTimer(this);
    connect(m_audioProcessTimer, &QTimer::timeout, this, &TextRecognitionWidget::onTimerTimeout);
    
    // 初始化音频源，使用当前选择的麦克风设备
    if (!m_audioInputDevices.isEmpty() && m_currentAudioDeviceIndex < m_audioInputDevices.size()) {
        initAudioSource(m_audioInputDevices.at(m_currentAudioDeviceIndex));
    }
    else if (!m_audioInputDevices.isEmpty()) {
        // 如果索引无效但有设备，使用第一个设备
        initAudioSource(m_audioInputDevices.first());
        m_currentAudioDeviceIndex = 0;
    }
    
    // 打开音频缓冲区
    m_audioBuffer.open(QIODevice::WriteOnly | QIODevice::Truncate);
    
    // 只在需要自动连接时执行连接序列
    if (autoConnect) {
        // 先检查网络状态，再尝试连接WebSocket
        QTimer::singleShot(1000, this, [this]() {
            qDebug() << "开始网络检查...";
            logNetworkReachability();
        });
    }
    
    // 初始化语音活动状态
    resetVoiceActivity();
}


// 添加回调方法，当摄像头资源分配完成时调用
void TextRecognitionWidget::onCameraAllocated(bool success, int cameraIndex)
{
    if (!success) {
        qWarning() << "TextRecognitionWidget: 摄像头资源分配失败";
        m_statusLabel->setText("状态: 无法获取摄像头资源");
        
        // 更新UI提示
        m_recognitionTextDisplay->setText("<p style='color:red;'>"
                                       "⚠️ 摄像头资源获取失败<br><br>"
                                       "可能的原因:<br>"
                                       "- 摄像头被其他应用占用<br>"
                                       "- 系统没有可用的摄像头<br>"
                                       "- 硬件问题</p>");
        return;
    }
    
    qDebug() << "TextRecognitionWidget: 摄像头资源分配成功，索引:" << cameraIndex;
    
    // 列出可用的摄像头 - 确认
    QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    
    for (int i = 0; i < cameras.size(); ++i) {
        qDebug() << "找到摄像头:" << i << "-" << cameras[i].description();
    }
    
    if (cameras.isEmpty()) {
        qWarning() << "没有找到可用摄像头!";
        QMessageBox::warning(this, "警告", "未找到可用的摄像头设备，图像识别功能将不可用");
        return;
    }
    
    // 确保索引在有效范围内
    if (cameraIndex >= cameras.size()) {
        cameraIndex = 0;
    }
    
    // 创建摄像头对象 - 使用指定索引
    m_camera = new QCamera(cameras[cameraIndex], this);
    qDebug() << "使用摄像头:" << cameras[cameraIndex].description();
    
    // 设置摄像头格式和分辨率
    QCameraFormat bestFormat;
    int highestResolution = 0;
    bool formatFound = false;
    
    // 查找支持的最高分辨率格式
    const QList<QCameraFormat> formats = cameras[cameraIndex].videoFormats();
    for (const QCameraFormat &format : formats) {
        const QSize resolution = format.resolution();
        const int pixels = resolution.width() * resolution.height();
        
        // 查找分辨率适中的格式
        if (pixels > highestResolution && pixels <= 1280*720) {
            highestResolution = pixels;
            bestFormat = format;
            formatFound = true;
        }
    }
    
    if (formatFound) {
        qDebug() << "设置摄像头格式:" << bestFormat.resolution().width() << "x" 
                << bestFormat.resolution().height() << "帧率:" << bestFormat.maxFrameRate();
        m_camera->setCameraFormat(bestFormat);
    }
    
    // 创建捕获会话
    m_captureSession = new QMediaCaptureSession(this);
    m_captureSession->setCamera(m_camera);
    
    // 创建视频接收器
    m_videoSink = new QVideoSink(this);
    m_captureSession->setVideoSink(m_videoSink);
    
    // 连接视频帧信号
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, &TextRecognitionWidget::processFrame);
    
    // 开始摄像头
    m_camera->start();
    qDebug() << "摄像头已启动";
    
    m_statusLabel->setText("状态: 摄像头已启动");
}

// 初始化音频源
void TextRecognitionWidget::initAudioSource(const QAudioDevice &device)
{
    qDebug() << "初始化音频源，设备:" << device.description();
    
    // 初始化音频格式
    QAudioFormat format;
    format.setSampleRate(SAMPLE_RATE);
    format.setChannelCount(CHANNELS);
    format.setSampleFormat(QAudioFormat::Int16);
    
    qDebug() << "请求的音频格式:";
    qDebug() << "- 采样率:" << format.sampleRate();
    qDebug() << "- 声道数:" << format.channelCount();
    qDebug() << "- 采样格式:" << format.sampleFormat();
    
    // 检查格式是否支持
    if (device.isFormatSupported(format)) {
        // 如果之前有音频源，先清理
        if (m_audioSource) {
            m_audioSource->stop();
            delete m_audioSource;
            m_audioSource = nullptr;
        }
        
        // 创建新的音频源
        m_audioSource = new QAudioSource(device, format, this);
        
        // 设置较大的缓冲区以确保音频数据不会丢失
        int bufferSize = format.bytesForDuration(2000000); // 增加到2秒的缓冲区
        m_audioSource->setBufferSize(bufferSize);
        qDebug() << "设置音频缓冲区大小:" << bufferSize << "字节";
    } else {
        qWarning() << "请求的音频格式不受设备支持:" << device.description();
        QMessageBox::warning(this, "错误", 
                            QString("设备 [%1] 不支持格式：%2kHz/%3声道/PCM16")
                            .arg(device.description())
                            .arg(SAMPLE_RATE / 1000)
                            .arg(CHANNELS));
    }
}

// 音频连接重置方法
void TextRecognitionWidget::resetAudioConnection() 
{
    qDebug() << "重置音频连接...";
    
    // 停止录音和相关定时器
    if (m_isRecording) {
        if (m_audioSource) {
            m_audioSource->stop();
        }
        if (m_audioProcessTimer) {
            m_audioProcessTimer->stop();
        }
    }
    
    // 使用WebSocketConnectionHandler重置连接
    if (m_webSocketHandler) {
        m_webSocketHandler->resetConnection();
    }
    
    // 停止所有相关计时器
    m_silenceDetectionTimer->stop();
    m_micInactivityTimer->stop();
    
    // 等待连接关闭后重新连接
    QTimer::singleShot(1000, this, [this]() {
        // 重置缓冲区
        m_audioBuffer.close();
        m_audioBuffer.open(QIODevice::WriteOnly | QIODevice::Truncate);
        
        // 重新初始化音频源
        if (m_currentAudioDeviceIndex < m_audioInputDevices.size()) {
            initAudioSource(m_audioInputDevices.at(m_currentAudioDeviceIndex));
        }
        
        // 重置语音状态
        resetVoiceActivity();
        
        // 重新连接WebSocket
        m_isRecording = true; // 设置为true以便连接成功后自动开始录音
        connectToWebSocket();
    });
}

void TextRecognitionWidget::processFrame(const QVideoFrame &frame)
{
    // 仅捕获图像，不显示视频流
    QImage image = frame.toImage();
    
    if (!image.isNull()) {
        // 保存当前帧用于OCR识别
        m_currentFrame = image.copy();
    }
}

void TextRecognitionWidget::onBackButtonClicked()
{ qDebug() << "返回按钮点击 - 清理资源并关闭连接";
    // 主动停止所有识别活动
    stopRecognition();
    
    // 停止摄像头
    if (m_camera) {
        if (m_camera->isActive()) {
            m_camera->stop();
            qDebug() << "摄像头已停止";
        }
        
        // 等待资源释放
        QThread::msleep(500);
    }
    
    // 通过摄像头管理器释放资源
    auto& cameraManager = CameraResourceManager::instance();
    cameraManager.releaseCamera("TextRecognition");
    
    // 停止音频录制
    if (m_audioSource) {
        m_audioSource->stop();
    }
    
    // 停止所有计时器
    if (m_audioProcessTimer) m_audioProcessTimer->stop();
    if (m_micInactivityTimer) m_micInactivityTimer->stop();
    if (m_silenceDetectionTimer) m_silenceDetectionTimer->stop();
    
    // 设置标志，避免重新连接
    m_isRecording = false;
    
    // 使用WebSocketConnectionHandler断开连接
    if (m_webSocketHandler) {
        m_webSocketHandler->disconnectFromServer();
        
        // 等待一小段时间确保连接已关闭
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    
    m_cameraInitialized = false;
    // 发送返回信号
    emit backButtonClicked();
}

void TextRecognitionWidget::performImageRecognition(const QString &prompt)
{
    if (m_isProcessingRequest || m_currentFrame.isNull() ||
        m_lastRequestTime.msecsTo(QTime::currentTime()) < m_minRequestInterval) {
        return;
    }
    
    m_isProcessingRequest = true;
    m_lastRequestTime = QTime::currentTime();
    
    // 更新状态
    m_statusLabel->setText("状态: 正在识别图像...");
    qDebug() << "准备发送图像识别请求...";
    
    // 将当前帧转换为Base64编码
    QByteArray base64Image = imageToBase64(m_currentFrame);
    
    // 构建请求 - 确保使用完全匹配的URL
    QNetworkRequest request(QUrl("https://ark.cn-beijing.volces.com/api/v3/chat/completions"));
    
    // 设置请求头 - 保持简单并匹配成功的curl命令
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_volcanoConfig.apiKey).toUtf8());
    
    // 构建请求体 - 严格按照API文档和成功的curl请求格式
    QJsonObject requestBody;
    requestBody["model"] = "doubao-1-5-vision-pro-32k-250115"; // 确保使用正确的模型ID
    
    QJsonArray messages;
    QJsonObject message;
    message["role"] = "user";
    
    QJsonArray content;
    
    // 添加图像部分
    QJsonObject imageUrlObj;
    imageUrlObj["type"] = "image_url";
    
    QJsonObject urlObj;
    // 使用base64格式
    urlObj["url"] = QString("data:image/jpeg;base64,%1").arg(QString(base64Image));
    imageUrlObj["image_url"] = urlObj;
    
    content.append(imageUrlObj);
    
    // 添加文本提示
    QJsonObject textObj;
    textObj["type"] = "text";
    QString promptText = prompt.isEmpty() ? "请描述这个图像" : prompt;
    textObj["text"] = promptText;
    
    content.append(textObj);
    
    message["content"] = content;
    messages.append(message);
    
    requestBody["messages"] = messages;
    
    // 转换为JSON并输出用于调试
    QJsonDocument doc(requestBody);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);
    
    qDebug() << "图像识别请求体:" << payload;
    
    // 发送请求
    m_networkManager->post(request, payload);
    qDebug() << "图像识别请求已发送";
}

void TextRecognitionWidget::handleImageRecognitionResponse(QNetworkReply *reply)
{
    m_isProcessingRequest = false;
    
    // 检查是否有错误
    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "图像识别请求错误:" << reply->errorString();
        m_statusLabel->setText("状态: 图像识别失败");
        reply->deleteLater();
        return;
    }
    
    // 读取响应数据
    QByteArray responseData = reply->readAll();
    reply->deleteLater();
    
    // 解析JSON响应
    QJsonDocument jsonResponse = QJsonDocument::fromJson(responseData);
    QJsonObject jsonObject = jsonResponse.object();
    
    if (jsonObject.contains("choices") && jsonObject["choices"].isArray()) {
        QJsonArray choices = jsonObject["choices"].toArray();
        if (!choices.isEmpty()) {
            QJsonObject firstChoice = choices.first().toObject();
            if (firstChoice.contains("message") && firstChoice["message"].isObject()) {
                QJsonObject message = firstChoice["message"].toObject();
                if (message.contains("content") && message["content"].isString()) {
                    QString recognizedText = message["content"].toString();
                    displayRecognitionText(recognizedText);
                    m_statusLabel->setText("状态: 图像识别完成");
                    return;
                }
            }
        }
    }
    
    // 如果无法解析预期格式的响应
    qWarning() << "无法解析图像识别响应";
    m_statusLabel->setText("状态: 无法解析图像识别结果");
}

QByteArray TextRecognitionWidget::imageToBase64(const QImage &image)
{
    // 转换成JPG格式并压缩以减小大小
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 80);  // 使用80%质量的JPG格式
    buffer.close();
    
    // 转换为Base64
    return byteArray.toBase64();
}

void TextRecognitionWidget::displayRecognitionText(const QString &text)
{
    // 更新识别文本显示
    m_recognitionTextDisplay->setText(text);
    
    // 将识别结果添加到历史列表
    m_recognizedTexts.append(text);
    
    // 限制保存的识别结果数量
    if (m_recognizedTexts.size() > 20) {
        m_recognizedTexts.removeFirst();
    }
}

// WebSocket相关处理方法
void TextRecognitionWidget::handleRecognizedText(const QString &text)
{
    if (text.isEmpty()) return;
    
    // 更新识别文本
    m_recognizedVoiceText = text;
    
    // 更新显示
    QString displayText = "语音识别:\n" + m_recognizedVoiceText + 
                         "\n\n翻译结果:\n" + m_translatedVoiceText;
    m_recognitionTextDisplay->setText(displayText);
    
    qDebug() << "语音识别更新: " << text;
}

void TextRecognitionWidget::handleTranslatedText(const QString &text, const QString &originalText)
{
    if (text.isEmpty()) return;
    
    // 更新翻译文本
    m_translatedVoiceText = text;
    
    // 更新标志
    m_hasVoiceResult = true;
    
    // 更新显示
    QString displayText = "语音识别:\n" + m_recognizedVoiceText + 
                         "\n\n翻译结果:\n" + m_translatedVoiceText;
    m_recognitionTextDisplay->setText(displayText);
    
    qDebug() << "翻译结果更新: " << text;
    
    // 重置麦克风不活动计时器
    m_micInactivityTimer->stop();
    m_micInactivityTimer->start(3000);
}

void TextRecognitionWidget::handleConnectionStateChanged(ConnectionState state)
{
    switch (state) {
        case ConnectionState::Disconnected:
            m_statusLabel->setText("状态: 未连接");
            break;
        case ConnectionState::Connecting:
            m_statusLabel->setText("状态: 正在连接...");
            break;
        case ConnectionState::Connected:
            m_statusLabel->setText("状态: 已连接");
            break;
        case ConnectionState::Closing:
            m_statusLabel->setText("状态: 正在断开连接...");
            break;
        case ConnectionState::Reconnecting:
            m_statusLabel->setText("状态: 正在重新连接...");
            break;
    }
}

void TextRecognitionWidget::handleConnectionFailed(const QString &errorMessage)
{
    m_statusLabel->setText("状态: 连接失败 - " + errorMessage);
    
    // 显示错误提示
    m_recognitionTextDisplay->setText("<p style='color:orange;'>"
                                     "⚠️ 连接失败: " + errorMessage + "<br><br>"
                                     "请尝试:<br>"
                                     "- 切换麦克风设备<br>"
                                     "- 检查网络连接<br>"
                                     "- 重新启动应用</p>");
}

void TextRecognitionWidget::handleWebSocketLog(const QString &message, bool isError)
{
    if (isError) {
        qWarning() << "WebSocket:" << message;
    } else {
        qDebug() << "WebSocket:" << message;
    }
}

void TextRecognitionWidget::connectToWebSocket()
{
    m_statusLabel->setText("状态: 正在连接语音识别服务...");
    
    // 使用WebSocketConnectionHandler连接
    m_webSocketHandler->connectToServer(
        getSourceLanguageCode(),
        getTargetLanguageCode()
    );
}

void TextRecognitionWidget::onTimerTimeout()
{
    // 只在录音状态下处理音频
    if (!m_isRecording) return;
    
    processAudioChunk();
}

// 音频数据处理方法
void TextRecognitionWidget::processAudioChunk()
{
    if (!m_isRecording) return;
    
    static qint64 lastPos = 0;
    qint64 currentPos = m_audioBuffer.pos();
    
    if (currentPos <= lastPos) {
        return;
    }
    
    QByteArray chunk = m_audioBuffer.buffer().mid(lastPos, currentPos - lastPos);
    lastPos = currentPos;
    
    if (chunk.isEmpty()) {
        return;
    }
    
    // 检测是否有语音活动
    bool hasSpeech = detectSpeech(chunk);
    
    if (hasSpeech) {
        m_hasSpeech = true;
        m_silenceTimer.restart(); // 检测到语音时重置静音计时器
        m_lastVoiceActivityTime = QTime::currentTime();
        
        // 重置麦克风不活动检测定时器
        m_micInactivityTimer->stop();
        m_micInactivityTimer->start(3000);
        
        // 使用WebSocketConnectionHandler发送音频数据
        if (m_webSocketHandler->isConnected()) {
            m_webSocketHandler->sendAudioData(chunk);
        }
    } else if (m_hasSpeech) {
        // 在语音结束后仍然发送几帧，以捕获尾音
        if (m_webSocketHandler->isConnected()) {
            m_webSocketHandler->sendAudioData(chunk);
        }
    }
}

// 语音检测方法
bool TextRecognitionWidget::detectSpeech(const QByteArray& audioData)
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
    
    // 使用动态阈值，根据检测到的环境噪声自适应调整
    return rms > m_silenceThreshold;
}

void TextRecognitionWidget::checkMicrophoneInactivity()
{
    // 如果已经有语音识别结果，并且目标语言是中文，触发图像识别
    if (m_hasVoiceResult && m_isChineseTarget) {
        qDebug() << "检测到麦克风不活动，触发图像识别";
        qDebug() << "使用翻译文本作为提示: " << m_recognizedVoiceText;
        
        // 更新状态
        m_statusLabel->setText("状态: 语音识别已完成，正在进行图像识别...");
        
        // 触发图像识别
        performImageRecognition(m_recognizedVoiceText);
        
        // 重置语音结果标志以防止多次触发
        m_hasVoiceResult = false;
    }
}

bool TextRecognitionWidget::shouldTriggerImageRecognition()
{
    // 此方法已被静音检测逻辑替代
    return false;
}

void TextRecognitionWidget::resetVoiceActivity()
{
    m_hasVoiceResult = false;
    m_hasSpeech = false;
    m_recognizedVoiceText = "";
    m_translatedVoiceText = "";
    m_lastVoiceActivityTime = QTime::currentTime();
    m_silenceTimer.restart();
}

void TextRecognitionWidget::logNetworkReachability()
{
    m_statusLabel->setText("状态: 正在检查网络连接...");
    
    // 检查基本网络连接
    QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    bool hasIPv4 = false;
    
    qDebug() << "网络接口状态:";
    foreach (const QHostAddress &address, addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && address != QHostAddress::LocalHost) {
            qDebug() << "- 接口:" << address.toString();
            hasIPv4 = true;
        }
    }
    
    if (!hasIPv4) {
        qDebug() << "警告: 没有找到IPv4地址，可能没有网络连接";
        m_statusLabel->setText("状态: 网络连接异常，请检查网络");
        m_recognitionTextDisplay->setText("<p style='color:red;'>"
                                         "⚠️ 网络连接异常<br><br>"
                                         "未检测到有效的网络连接<br>"
                                         "请确保设备已连接到互联网<br>"
                                         "然后退出并重新启动应用</p>");
        return;
    }
    
    // 使用通用网站测试基本网络连接
    QTcpSocket* socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::connected, [this, socket]() {
        qDebug() << "网络连接测试成功 - 可以访问外部网络";
        socket->disconnectFromHost();
        socket->deleteLater();
        
        // 连接成功后，测试WebSocket服务可用性（有道云API）
        testWebSocketApiAvailability();
    });
    
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred), 
            [this, socket](QAbstractSocket::SocketError error) {
        qDebug() << "网络连接测试失败:" << error;
        qDebug() << "错误详情:" << socket->errorString();
        
        m_statusLabel->setText("状态: 外部网络连接异常，将尝试直接连接服务");
        socket->deleteLater();
        
        // 即使测试失败，也尝试直接连接WebSocket
        QTimer::singleShot(1000, this, [this]() {
            connectToWebSocket();
        });
    });
    
    // 尝试连接到百度（更可靠的网络测试目标）
    qDebug() << "正在测试网络连接...";
    socket->connectToHost("www.baidu.com", 80);
}


// 分别测试WebSocket API可用性（有道云API）
void TextRecognitionWidget::testWebSocketApiAvailability()
{
    m_statusLabel->setText("状态: 正在测试语音识别服务连接...");
    
    // 创建HTTP GET请求测试有道云API服务器的可用性
    QNetworkRequest request(QUrl("https://openapi.youdao.com/"));
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    
    QNetworkReply *reply = m_networkManager->get(request);
    
    // 设置请求超时
    QTimer *timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    connect(timeoutTimer, &QTimer::timeout, [reply]() {
        if (reply && reply->isRunning()) {
            reply->abort();
        }
    });
    timeoutTimer->start(5000); // 5秒超时
    
    // 处理请求完成
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeoutTimer]() {
        timeoutTimer->stop();
        timeoutTimer->deleteLater();
        
        if (reply->error() == QNetworkReply::NoError) {
            qDebug() << "语音识别服务测试成功";
            m_statusLabel->setText("状态: 语音识别服务连接正常，正在建立连接...");
            
            // 测试成功后，再测试图像识别服务
            testImageRecognitionApiAvailability();
        } else {
            qDebug() << "语音识别服务测试失败:" << reply->errorString();
            m_statusLabel->setText("状态: 语音识别服务连接异常，尝试直接连接...");
            
            // 即使HTTP测试失败，也尝试WebSocket连接，可能只是HTTP端口被阻止
            QTimer::singleShot(1000, this, [this]() {
                connectToWebSocket();
                
                // 同时测试图像识别服务
                testImageRecognitionApiAvailability();
            });
        }
        
        reply->deleteLater();
    });
}

// 测试图像识别API可用性（火山引擎API）
void TextRecognitionWidget::testImageRecognitionApiAvailability()
{
    // 火山引擎API不提供简单的服务可用性检查端点
    // 我们使用一个通用HTTP请求检查网络是否可以访问该域名
    
    // 构造简单的请求，不包含认证信息
    QUrl baseUrl(m_volcanoConfig.endpoint);
    QString host = baseUrl.host();
    if (host.isEmpty()) {
        qDebug() << "无法从端点URL提取主机名，跳过图像API测试";
        return;
    }
    
    QUrl testUrl("https://" + host);
    QNetworkRequest request(testUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    
    QNetworkReply *reply = m_networkManager->get(request);
    
    // 设置请求超时
    QTimer *timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    connect(timeoutTimer, &QTimer::timeout, [reply]() {
        if (reply && reply->isRunning()) {
            reply->abort();
        }
    });
    timeoutTimer->start(5000); // 5秒超时
    
    // 处理请求完成
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeoutTimer, host]() {
        timeoutTimer->stop();
        timeoutTimer->deleteLater();
        
        // 这里我们不关心返回的内容，只要能连接到服务器即可
        if (reply->error() == QNetworkReply::NoError || 
            reply->error() == QNetworkReply::ContentNotFoundError) { // 404也算连接成功
            qDebug() << "图像识别服务测试成功，可以访问" << host;
        } else {
            qDebug() << "图像识别服务测试失败:" << reply->errorString();
            qDebug() << "图像识别功能可能不可用";
        }
        
        reply->deleteLater();
    });
}

void TextRecognitionWidget::testApiAvailability()
{
    m_statusLabel->setText("状态: 正在测试网络连接...");
    
    // 创建HTTP GET请求测试有道服务器的可用性
    QNetworkRequest request(QUrl("https://openapi.youdao.com/"));
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    
    QNetworkReply *reply = m_networkManager->get(request);
    
    // 设置请求超时
    QTimer *timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    connect(timeoutTimer, &QTimer::timeout, [reply]() {
        if (reply && reply->isRunning()) {
            reply->abort();
        }
    });
    timeoutTimer->start(5000); // 5秒超时
    
    // 处理请求完成
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeoutTimer]() {
        timeoutTimer->stop();
        timeoutTimer->deleteLater();
        
        if (reply->error() == QNetworkReply::NoError) {
            qDebug() << "API服务器可达，HTTP测试成功";
            m_statusLabel->setText("状态: 服务器连接正常，正在建立语音服务...");
            
            // 成功后开始WebSocket连接
            connectToWebSocket();
        } else {
            qDebug() << "API服务器HTTP测试失败:" << reply->errorString();
            m_statusLabel->setText("状态: 服务器连接异常，尝试使用WebSocket连接...");
            
            // 即使HTTP测试失败，也尝试WebSocket连接，可能只是HTTP端口被阻止
            QTimer::singleShot(1000, this, [this]() {
                connectToWebSocket();
            });
        }
        
        reply->deleteLater();
    });
}

// 辅助方法
QString TextRecognitionWidget::getSourceLanguageCode() const
{
    return m_sourceLanguageComboBox->currentData().toString();
}

QString TextRecognitionWidget::getTargetLanguageCode() const
{
    return m_targetLanguageComboBox->currentData().toString();
}

// 实现开始识别方法
void TextRecognitionWidget::startRecognition()
{
    if (m_autoStarted) {
        qDebug() << "识别已经在运行中，忽略重复启动请求";
        return;
    }
    
    qDebug() << "开始语音识别...";
    m_statusLabel->setText("状态: 正在启动语音识别服务...");
    
    // 重置缓冲区
    if (m_audioBuffer.isOpen()) {
        m_audioBuffer.buffer().clear();
        m_audioBuffer.seek(0);
    } else {
        m_audioBuffer.open(QIODevice::WriteOnly | QIODevice::Truncate);
    }
    
    // 重置语音状态
    resetVoiceActivity();
    
    // 开始检查网络并连接WebSocket
    logNetworkReachability();
    
    m_autoStarted = true;
}

// 实现停止识别方法
void TextRecognitionWidget::stopRecognition()
{
    qDebug() << "停止语音识别...";
    
    // 停止所有计时器
    if (m_audioProcessTimer) m_audioProcessTimer->stop();
    if (m_micInactivityTimer) m_micInactivityTimer->stop();
    if (m_silenceDetectionTimer) m_silenceDetectionTimer->stop();
    
    // 停止音频录制
    if (m_audioSource) {
        m_audioSource->stop();
    }
    if (m_camera && m_camera->isActive()) {
        m_camera->stop();
        qDebug() << "摄像头已在停止识别中停止";
    }
    // 确保WebSocket连接关闭
    if (m_webSocketHandler) {
        // 这里强制断开连接
        m_webSocketHandler->disconnectFromServer();
        
        // 等待一小段时间确保连接关闭
        QEventLoop loop;
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
        loop.exec();
    }
    
    m_isRecording = false;
    m_autoStarted = false;
    
    m_statusLabel->setText("状态: 语音识别已停止");
}

