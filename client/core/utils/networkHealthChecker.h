#ifndef NETWORKHEALTHCHECKER_H
#define NETWORKHEALTHCHECKER_H

#include <QObject>
#include <QMap>
#include <QString>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QTimer>
#include <QPointer>
#include <QPair>

class NetworkHealthChecker : public QObject
{
    Q_OBJECT

public:
    enum class Status {
        Unknown = 0,
        Checking = 1,
        Online = 2,
        Offline = 3
    };
    Q_ENUM(Status)

    struct HealthInfo {
        int pingMs = -1;
        Status status = Status::Unknown;
        qint64 lastCheckedMs = 0;
    };

    explicit NetworkHealthChecker(QObject *parent = nullptr);
    ~NetworkHealthChecker() override;

    static NetworkHealthChecker *instance();

    int getPing(const QString &key) const;
    Status getStatus(const QString &key) const;

    void setTarget(const QString &key, const QString &host, int port = 443);
    bool hasTarget(const QString &key) const;

public slots:
    void checkPing(const QString &key);
    void checkAll();
    void cancelAll();

signals:
    void pingUpdated(const QString &key, int pingMs, NetworkHealthChecker::Status status);
    void checkAllFinished();

private slots:
    void onSocketConnected();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onSocketTimeout();

private:
    struct ProbeTask {
        QString key;
        QString host;
        int port;
        QPointer<QTcpSocket> socket;
        QPointer<QTimer> timeoutTimer;
        QElapsedTimer timer;
    };

    void startProbe(const QString &key, const QString &host, int port);
    void cleanupProbe(ProbeTask *task);
    void saveCache();
    void loadCache();

    static NetworkHealthChecker *s_instance;
    QMap<QString, HealthInfo> m_results;
    QMap<QString, QPair<QString, int>> m_targetHosts;
    QMap<QTcpSocket*, ProbeTask*> m_activeProbes;
};

#endif // NETWORKHEALTHCHECKER_H
