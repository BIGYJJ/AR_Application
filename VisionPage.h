#ifndef VISIONPAGE_H
#define VISIONPAGE_H

#include <QWidget>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QImageCapture>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QTextEdit>
#include <QVideoWidget>
#include <QVideoSink>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QSqlDatabase>
#include <QQueue>
#include <QRegularExpression>
#include <QWebSocket>
#include <QAudioSource>
#include <QMediaDevices>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QUrlQuery>
#include <QDateTime>
#include <QRandomGenerator>
#include <QMessageBox>
#include <QThread>  // Added for QThread::msleep
#include "CameraResourceManager.h"  // Added for camera resource management

class VisionPage : public QWidget
{
    Q_OBJECT

public:
    explicit VisionPage(const QString &iconPath = "", QWidget *parent = nullptr);
    ~VisionPage();

    // Add public methods to start/stop functionality
    void startRecording();  // Start WebSocket, audio recording, and image capture
    void stopRecording();   // Stop all processes and clean up resources
    
    // Flag to track if recording has been started
    bool recordingStarted = false;

signals:
    void backButtonClicked();
    void cameraPreempted();  // Signal to indicate camera has been preempted

private slots:
    void onBackButtonClicked();
    void onCameraButtonClicked();
    void onDeviceChanged(int index);
    void onTimerTimeout();
    void onImageCaptured(int id, const QImage &image);
    void onImageSaved(int id, const QString &fileName);
    void onApiRequestFinished(QNetworkReply *reply);
    void updateResultDisplay(const QString &result);

    // Camera resource management slots - fixed signature to match signal
    void onCameraResourceAllocated(const QString& requesterId, int cameraIndex, bool success);
    void onCameraResourcePreempted(const QString& requesterId);
    
    // Audio translation slots (from TranslatePage)
    void onAudioDeviceChanged(int index);
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketMessageReceived(const QString &message);
    void onWebSocketError(QAbstractSocket::SocketError error);
    void onAudioTimerTimeout();
    void onSilenceTimerTimeout();

private:
    // UI components
    QPushButton *backButton;
    QPushButton *cameraButton;
    QComboBox *cameraDeviceComboBox;
    QComboBox *audioDeviceComboBox;
    QTextEdit *resultTextEdit;
    QVideoWidget *videoWidget;
    QLabel *statusLabel;
    QLabel *overlayLabel;
    QTimer *overlayHideTimer;

    // Camera and media components
    QCamera *camera;
    QMediaCaptureSession captureSession;
    QImageCapture *imageCapture;
    QList<QCameraDevice> cameraDevices;
    
    // Network components
    QNetworkAccessManager *networkManager;
    
    // Timer for periodic capture
    QTimer *captureTimer;
    
    // State variables
    bool isCapturing;
    QString currentImagePath;
    QQueue<QString> pendingImages;
    bool isProcessingRequest;
    bool cameraResourceAvailable;  // Track if camera resource is available
    int allocatedCameraIndex;      // Track which camera index was allocated
    
    // API configuration
    QString apiUrl;
    QString apiKey;
    QString modelId;
    QString prompt;

    // Database connection
    QSqlDatabase db;

    // Audio recording (from TranslatePage)
    QAudioSource *audioSource;
    QBuffer audioBuffer;
    QList<QAudioDevice> audioDevices;
    bool isRecording;
    QString accumulatedTranslationText;
    QString accumulatedRecognizedText;
    
    // WebSocket for translation
    QWebSocket *webSocket;
    bool isWebSocketConnecting;
    bool webSocketIsClosed;
    
    // Timers
    QTimer *audioTimer;
    QTimer *silenceTimer;
    QTimer *idleTimer;
    QTimer *maxDurationTimer;
    QTimer *resourceRetryTimer;  // Timer for retrying resource allocation
    
    int currentSequence;
    
    // Speech translation API config
    const QString APP_SECRET = "6oFULWPILuGRS43WNZHQcKNhIAKXJmud";
    const QString SPEECH_API_KEY = "18d5ce83dbec2560";
    const QString WS_URL = "wss://openapi.youdao.com/stream_speech_trans";
    const int SAMPLE_RATE = 16000;
    const int CHANNELS = 1;
    const int BITS_PER_SAMPLE = 16;
    const int SILENCE_THRESHOLD = 200;
    const int SILENCE_TIMEOUT_MS = 2000; // 2 seconds of silence before capturing image
    
    // Methods
    void initCamera(const QCameraDevice &device);
    void startCapturing();
    void stopCapturing();
    void captureAndSendImage();
    void sendImageToApi(const QString &imagePath);
    QByteArray imageToBase64(const QString &imagePath);
    void processApiResponse(const QJsonDocument &response);
    QString extractResultFromResponse(const QJsonDocument &response);
    void processNextImageInQueue();
    void overlayTextOnVideo(const QString &text);
    bool initDatabase();
    void saveToDatabase(const QString &imagePath, const QString &result);
    void resetPage();

    // Camera resource management methods
    bool requestCameraResource(int preferredIndex = 0);
    void releaseCameraResource();
    void safelyInitCamera(int cameraIndex);
    void safelyStopCamera();
    void retryRequestCameraResource();

    // Audio methods (from TranslatePage)
    void initAudioRecorder(const QAudioDevice &device);
    void connectToWebSocket();
    void sendSessionUpdate();
    void sendAudioChunk(const QByteArray &chunk);
    void processAudioChunk(bool isFinal = false);
    bool shouldProcessChunk(const QByteArray &audioData);
    void sendAudioDone();
    void endSession();
    void handleTranslationError(const QString &errorCode);
    QMap<QString, QString> createRequestParams();
    QString getSourceLanguageCode() const;
    QString getTargetLanguageCode() const;
    QString generateYoudaoSign(const QString &q, const QString &salt, const QString &curtime);
    void updateTranslationDisplay();
};

#endif // VISIONPAGE_H