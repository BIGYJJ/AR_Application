#ifndef DATABASEWORKER_H
#define DATABASEWORKER_H
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QMutex>

class DatabaseWorker : public QObject {
    Q_OBJECT
public:
    explicit DatabaseWorker(QObject *parent = nullptr);
    bool connect(const QString &host, int port, 
                const QString &user, const QString &password,
                const QString &dbName);
    QJsonArray queryData(const QString &sql);

private:
    QSqlDatabase m_db;
    QMutex m_mutex;
};
#endif // DATABASEWORKER_H