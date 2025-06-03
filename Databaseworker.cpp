#include "Databaseworker.h"
#include <QSqlRecord>       // SQL记录支持
#include <QJsonArray>       // JSON数组
#include <QJsonObject>      // JSON对象
#include <QJsonValue>       // JSON值
#include <QJsonDocument>    // JSON文档
#include <QRandomGenerator> // Qt6随机数生成器
#include <QMutexLocker>     // 互斥锁
#include <QSqlQuery>
#include <QSqlError>


DatabaseWorker::DatabaseWorker(QObject *parent) : QObject(parent) {}

bool DatabaseWorker::connect(const QString &host, int port,
                            const QString &user, const QString &password,
                            const QString &dbName) {
    // 生成唯一连接名称
    QString connName = QString("connection_%1")
                       .arg(QRandomGenerator::global()->generate(), 0, 16);
    
    m_db = QSqlDatabase::addDatabase("QMYSQL", connName);
    m_db.setHostName(host);
    m_db.setPort(port);
    m_db.setUserName(user);
    m_db.setPassword(password);
    m_db.setDatabaseName(dbName);
    
    if (!m_db.open()) {
        //qWarning() << "Database connection error:" << m_db.lastError().text();
        return false;
    }
    return true;
}

QJsonArray DatabaseWorker::queryData(const QString &sql) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    QJsonArray result;

    qDebug() << "执行SQL语句:" << sql;

    if (query.exec(sql)) {
        qDebug() << "查询执行成功.";
        const QSqlRecord record = query.record();
        int rowCount = 0;

        while (query.next()) {
            rowCount++;
            QJsonObject obj;
            for (int i = 0; i < record.count(); ++i) { 
                QString fieldName = record.fieldName(i);
                QVariant value = query.value(i);
                qDebug() << "  字段:" << fieldName << "值:" << value.toString();
                obj.insert(fieldName, QJsonValue::fromVariant(value));
            }
            result.append(obj);
        }
        qDebug() << "查询返回行数:" << rowCount;
    } else {
        qWarning() << "查询执行失败:" << query.lastError().text() << "SQL:" << sql;
    }
    
    // 打印结果的JSON表示形式
    QJsonDocument doc(result);
    qDebug() << "查询结果:" << doc.toJson(QJsonDocument::Compact);
    
    return result;
}
