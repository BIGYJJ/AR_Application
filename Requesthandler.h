#ifndef REQUESTHANDLER_H
#define REQUESTHANDLER_H

#include <QObject>
#include <QRegularExpression>
#include <functional>
#include <QMap>
#include <QUrlQuery>
#include <QString>
#include <QByteArray>
#include "Databaseworker.h"
#include <QMutex>
// Forward declaration
class NavigationDisplayWidget;

// Custom comparator for QRegularExpression to be used in the routes map
struct QRegularExpressionCompare {
    bool operator()(const QRegularExpression& a, const QRegularExpression& b) const {
        return a.pattern() < b.pattern();
    }
};

class RequestHandler : public QObject
{
    Q_OBJECT
public:
    struct HttpRequest {
        QString method;
        QString path;
        QMap<QString, QString> headers;
        QMap<QString, QString> query;
        QByteArray body;
    };

    struct HttpResponse {
        int statusCode = 200;
        QString statusMessage = "OK";
        QMap<QString, QString> headers;
        QString contentType = "application/json";
        QByteArray content;
    };

    explicit RequestHandler(DatabaseWorker* dbWorker, QObject* parent = nullptr);

    // Handle HTTP requests
    HttpResponse handleRequest(const HttpRequest& request);
    HttpResponse handleSwitchPage(const HttpRequest& request);
    HttpResponse handleBackToMain(const HttpRequest& request);
    HttpResponse handleUploadPDF(const HttpRequest& request);
    HttpResponse handlePDFControl(const HttpRequest& request);
    // Register a navigation widget to receive updates
    void registerNavigationWidget(NavigationDisplayWidget* widget);
    
    // Unregister the navigation widget
    void unregisterNavigationWidget();
    
    // Check if navigation widget is active
    bool isNavigationWidgetActive() const;
    RequestHandler::HttpResponse createErrorResponse(int statusCode, const QString& message);
   // RequestHandler* getRequestHandler() const { return m_requestHandler; }
signals:
    // Signal to notify when navigation data is received
    void navigationDataReceived(const QString& direction, const QString& distance);
    void switchPageRequested(int pageIndex);
    void backToMainRequested();
    void pdfDataReceived(const QByteArray& pdfData);
    void pdfNextPage();
    void pdfPrevPage();
private:
    // Database worker
    DatabaseWorker* m_dbWorker;
    
    // Route handlers map with custom comparator
    std::map<QRegularExpression, 
             std::function<HttpResponse(const HttpRequest&)>, 
             QRegularExpressionCompare> m_routes;
    
    // Navigation widget reference
    NavigationDisplayWidget* m_navigationWidget;
    
    // Latest navigation data
    QString m_currentDirection;
    QString m_currentDistance;
    bool m_navigationActive;
    
    // Request handlers
    HttpResponse handleGetData(const HttpRequest& request);
    HttpResponse handlePostData(const HttpRequest& request);
    HttpResponse handleError(int code, const QString& message);
    
    // Navigation API handlers
    HttpResponse handleGetNavigationData(const HttpRequest& request);
    HttpResponse handlePostNavigationData(const HttpRequest& request);
    HttpResponse handleRegisterNavigation(const HttpRequest& request);
    HttpResponse handleUnregisterNavigation(const HttpRequest& request);
    HttpResponse handleExecuteSQL(const HttpRequest& request);
    QMutex m_mutex;
};

#endif // REQUESTHANDLER_H