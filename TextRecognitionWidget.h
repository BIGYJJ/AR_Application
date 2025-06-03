#ifndef TEXTRECOGNITIONWIDGET_H
#define TEXTRECOGNITIONWIDGET_H

#include <QWidget>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QPixmap>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>
#include <QDebug>
#include <QPainter>
#include <QCameraDevice>
#include <QMediaDevices>
#include <QCameraFormat>
#include <QBuffer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QCryptographicHash>
#include <QUrlQuery>
#include <QTime>
#include <QWebSocket>
#include <QAudioInput>
#include <QAudioSource>
#include <QMediaDevices>
#include <QComboBox>
#include <QCheckBox>
#include <QList>
#include <QMessageAuthenticationCode>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QElapsedTimer>
#include "CameraResourceManager.h"  // 添加中央摄像头管理器
#include "WebSocketConnectionHandler.h" // 引入WebSocket处理器

// 火山引擎API配置结构
struct VolcanoEngineConfig {
    QString apiKey;
    QString endpoint;
    QString model;
};

class TextRecognitionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TextRecognitionWidget(QWidget *parent = nullptr);
    ~TextRecognitionWidget();
    void startRecognition();    // 开始识别和WebSocket连接
    void stopRecognition();     // 停止识别和WebSocket连接
signals:
    void backButtonClicked();

private slots:
    void processFrame(const QVideoFrame &frame);
    void onBackButtonClicked();
    void performImageRecognition(const QString &prompt = "");
    void handleImageRecognitionResponse(QNetworkReply *reply);
    void onMicrophoneDeviceChanged(int index); // 麦克风设备更改处理
    void onTimerTimeout();
    void checkMicrophoneInactivity();
    
    // WebSocket处理器相关槽
    void handleRecognizedText(const QString &text);
    void handleTranslatedText(const QString &text, const QString &originalText);
    void handleConnectionStateChanged(ConnectionState state);
    void handleConnectionFailed(const QString &errorMessage);
    void handleWebSocketLog(const QString &message, bool isError);
    void handleWebSocketConnected();
    void handleWebSocketDisconnected();
    void showEvent(QShowEvent *event);
    void onCameraAllocated(bool success, int cameraIndex);
private:
    void setupUi();
    void setupCamera();
    void setupVolcanoEngineAPI();
    void setupAudioRecording(bool autoConnect = false);
    void setupMicrophoneSelection();
    void setupWebSocketHandler();
    
    QByteArray imageToBase64(const QImage &image);
    void displayRecognitionText(const QString &text);
    
    // 网络和WebSocket诊断
    void logNetworkReachability();
    void testApiAvailability();
    
    // 音频处理和检测
    bool detectSpeech(const QByteArray& audioData);
    void resetAudioConnection();
    void initAudioSource(const QAudioDevice &device);
    void connectToWebSocket();
    
    // 组件
    QPushButton *backButton;
    QLabel *m_recognitionTextDisplay;
    QCamera *m_camera;
    QMediaCaptureSession *m_captureSession;
    QVideoSink *m_videoSink;
    QImage m_currentFrame;
    QTimer *m_recognitionTimer;
    QTimer *m_micInactivityTimer; // 检测麦克风不活动的定时器
    QStringList m_recognizedTexts;
    
    // 网络请求
    QNetworkAccessManager *m_networkManager;
    bool m_isProcessingRequest;
    QTime m_lastRequestTime;
    int m_minRequestInterval;
    
    // 火山引擎API配置
    VolcanoEngineConfig m_volcanoConfig;
    
    // 语音识别相关变量
    WebSocketConnectionHandler *m_webSocketHandler; // WebSocket处理器
    QAudioSource *m_audioSource;
    QBuffer m_audioBuffer;
    bool m_isRecording;
    QString m_recognizedVoiceText;
    QString m_translatedVoiceText;
    QComboBox *m_sourceLanguageComboBox;
    QComboBox *m_targetLanguageComboBox;
    QComboBox *m_microphoneComboBox;      // 麦克风选择下拉框
    QLabel *m_statusLabel;
    QTimer *m_audioProcessTimer;
    QTime m_lastVoiceActivityTime;
    bool m_hasVoiceResult;
    bool m_isChineseTarget;
    
    // 音频处理相关
    QElapsedTimer m_silenceTimer;
    bool m_hasSpeech;
    int m_silenceThreshold;
    QTimer* m_silenceDetectionTimer;
    QList<QAudioDevice> m_audioInputDevices;
    int m_currentAudioDeviceIndex;
    
    // 辅助方法
    bool shouldTriggerImageRecognition();
    void resetVoiceActivity();
    void refreshAudioDeviceList();
    QString getSourceLanguageCode() const;
    QString getTargetLanguageCode() const;
    void processAudioChunk();

    bool m_autoStarted; // 跟踪页面是否已自动启动

    void testWebSocketApiAvailability();
    void testImageRecognitionApiAvailability();
};

#endif // TEXTRECOGNITIONWIDGET_H