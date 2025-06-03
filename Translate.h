

#ifndef TRANSLATEPAGE_H
#define TRANSLATEPAGE_H

#include <QWidget>
#include <QWebSocket>
#include <QAudioInput>
#include <QAudioSource>
#include <QMediaDevices>
#include <QBuffer>
#include <QPushButton>
#include <QTextEdit>
#include <QComboBox>
#include <QLabel>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QMessageAuthenticationCode>
#include <QUrlQuery>
#include <QThread>
#include <QAudioFormat>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QCheckBox>
class TranslatePage : public QWidget
{
    Q_OBJECT

public:
    explicit TranslatePage(const QString &iconPath = "", QWidget *parent = nullptr);
    ~TranslatePage();

signals:
    void backButtonClicked();

public slots:
    void onRecordButtonClicked();
    void onDeviceChanged(int index);
    void onConnected();
    void onDisconnected();
    void onMessageReceived(const QString &message);
    void onError(QAbstractSocket::SocketError error);
    void processAudioChunk(bool isFinal = false);
    void updateUnifiedTextDisplay();  // 新增: 更新统一显示
    void backButtonClickedHandler();  // 处理返回按钮点击
private:
    // UI 元素
    QPushButton *backButton;
    QPushButton *recordButton;
    QComboBox *deviceComboBox;
    QComboBox *languageComboBox;
    QTextEdit *unifiedTextEdit;
 
    // 音频处理
    QAudioSource *audioSource;
    QBuffer audioBuffer;
    QList<QAudioDevice> inputDevices;
    bool isRecording;
    QTimer *timer;
    QString accumulatedText;
    int currentSequence;
    
    // 网络处理
    QWebSocket *webSocket;
    QNetworkAccessManager *networkManager;
    
    // 连接和认证
    void connectToWebSocket();
    QString generateAuthorization();
    
    // 音频处理
    void initAudioRecorder(const QAudioDevice &device);
    QByteArray normalizeAudio(const QByteArray &audioData);
    bool shouldProcessChunk(const QByteArray &audioData);
    
    // WebSocket通信
    void sendInitMessage();
    void sendAudioChunk(const QByteArray &chunk);
    void sendSessionUpdate();
    void sendAudioDone();
    
    // 辅助函数
    QString getTargetLanguageCode() const;
    QString getTimestamp();
    QString getDate();
    QString sha256Hex(const QString &str);
    QByteArray hmacSha256Raw(const QByteArray &key, const QByteArray &msg);
    QString hmacSha256(const QString &key, const QString &input);

    QTimer *idleTimer; // 静默超时计时器
    QTimer *maxDurationTimer; // 最大连接时长计时器
    void endSession();
    QMap<QString, QString> createRequestParams();
    QString getSourceLanguage() const;
    QString getTargetLanguage() const;
    void handleError(const QString &errorCode);
    void onTimerTimeout();
    QString generateYoudaoSign(const QString &q, const QString &salt, const QString &curtime);
    bool webSocketIsClosed = true;
    QString accumulatedRecognizedText;
    QString accumulatedTranslationText;
    void resetPage();
    bool isWebSocketConnecting = false;

    QByteArray preprocessAudio(const QByteArray &audioData);  // 新增：音频预处理
    QString getSourceLanguageCode() const;  // 新增：获取源语言代码
    QComboBox *sourceLanguageComboBox;  // 新增：源语言选择
    QLabel *statusLabel;                // 新增：状态标签
    void debugConnection();

     // 数据库相关
     bool initDatabase();
     void saveToDatabase();
     QString formatTextWithLineBreaks(const QString &text, int lineLength);
      // 数据库连接
    QSqlDatabase db;
    
};
#endif // TRANSLATEPAGE_H










