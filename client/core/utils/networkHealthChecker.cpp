#include "networkHealthChecker.h"

#include <QSettings>
#include <QDateTime>
#include <QDebug>

NetworkHealthChecker *NetworkHealthChecker::s_instance = nullptr;

namespace {
    constexpr int defaultProbeTimeoutMs = 2500;
    constexpr int defaultPort = 443;
    constexpr char settingsGroup[] = "NetworkHealthCheckerCache";
}

NetworkHealthChecker::NetworkHealthChecker(QObject *parent)
    : QObject(parent)
{
    if (!s_instance) {
        s_instance = this;
    }
    loadCache();
}

NetworkHealthChecker::~NetworkHealthChecker()
{
    cancelAll();
    saveCache();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

NetworkHealthChecker *NetworkHealthChecker::instance()
{
    if (!s_instance) {
        s_instance = new NetworkHealthChecker();
    }
    return s_instance;
}

int NetworkHealthChecker::getPing(const QString &key) const
{
    return m_results.value(key).pingMs;
}

NetworkHealthChecker::Status NetworkHealthChecker::getStatus(const QString &key) const
{
    return m_results.value(key).status;
}

void NetworkHealthChecker::setTarget(const QString &key, const QString &host, int port)
{
    if (key.isEmpty() || host.isEmpty()) {
        return;
    }
    int targetPort = (port > 0) ? port : defaultPort;
    m_targetHosts[key] = qMakePair(host, targetPort);

    QSettings settings;
    settings.beginGroup(settingsGroup);
    settings.setValue(QString("%1_host").arg(key), host);
    settings.setValue(QString("%1_port").arg(key), targetPort);
    settings.endGroup();
}

bool NetworkHealthChecker::hasTarget(const QString &key) const
{
    return m_targetHosts.contains(key);
}

void NetworkHealthChecker::checkPing(const QString &key)
{
    if (key.isEmpty()) {
        return;
    }

    QString host;
    int port = defaultPort;

    if (m_targetHosts.contains(key)) {
        auto pair = m_targetHosts.value(key);
        host = pair.first;
        port = pair.second;
    } else {
        // Fallback target: try regional or gateway node
        host = QString("%1.gw.amnezia.org").arg(key.toLower());
    }

    startProbe(key, host, port);
}

void NetworkHealthChecker::checkAll()
{
    for (auto it = m_targetHosts.constBegin(); it != m_targetHosts.constEnd(); ++it) {
        startProbe(it.key(), it.value().first, it.value().second);
    }
}

void NetworkHealthChecker::cancelAll()
{
    for (auto it = m_activeProbes.begin(); it != m_activeProbes.end(); ++it) {
        ProbeTask *task = it.value();
        if (task) {
            if (task->socket) {
                task->socket->abort();
                task->socket->deleteLater();
            }
            if (task->timeoutTimer) {
                task->timeoutTimer->stop();
                task->timeoutTimer->deleteLater();
            }
            delete task;
        }
    }
    m_activeProbes.clear();
}

void NetworkHealthChecker::startProbe(const QString &key, const QString &host, int port)
{
    // Update state to Checking
    HealthInfo info = m_results.value(key);
    info.status = Status::Checking;
    m_results[key] = info;
    emit pingUpdated(key, -1, Status::Checking);

    // Cancel existing active probe for this key if any
    for (auto it = m_activeProbes.begin(); it != m_activeProbes.end(); ++it) {
        if (it.value() && it.value()->key == key) {
            ProbeTask *existing = it.value();
            if (existing->socket) {
                existing->socket->abort();
                existing->socket->deleteLater();
            }
            if (existing->timeoutTimer) {
                existing->timeoutTimer->stop();
                existing->timeoutTimer->deleteLater();
            }
            delete existing;
            m_activeProbes.erase(it);
            break;
        }
    }

    ProbeTask *task = new ProbeTask();
    task->key = key;
    task->host = host;
    task->port = port;

    QTcpSocket *socket = new QTcpSocket(this);
    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);

    task->socket = socket;
    task->timeoutTimer = timer;

    m_activeProbes[socket] = task;

    connect(socket, &QTcpSocket::connected, this, &NetworkHealthChecker::onSocketConnected);
    connect(socket, &QAbstractSocket::errorOccurred, this, &NetworkHealthChecker::onSocketError);
    connect(timer, &QTimer::timeout, this, &NetworkHealthChecker::onSocketTimeout);

    task->timer.start();
    timer->start(defaultProbeTimeoutMs);
    socket->connectToHost(host, static_cast<quint16>(port));
}

void NetworkHealthChecker::onSocketConnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !m_activeProbes.contains(socket)) {
        return;
    }

    ProbeTask *task = m_activeProbes.take(socket);
    int latency = static_cast<int>(task->timer.elapsed());

    HealthInfo info;
    info.pingMs = latency;
    info.status = Status::Online;
    info.lastCheckedMs = QDateTime::currentMSecsSinceEpoch();
    m_results[task->key] = info;

    emit pingUpdated(task->key, latency, Status::Online);

    cleanupProbe(task);

    if (m_activeProbes.isEmpty()) {
        emit checkAllFinished();
    }
}

void NetworkHealthChecker::onSocketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError)
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !m_activeProbes.contains(socket)) {
        return;
    }

    ProbeTask *task = m_activeProbes.take(socket);

    HealthInfo info;
    info.pingMs = -1;
    info.status = Status::Offline;
    info.lastCheckedMs = QDateTime::currentMSecsSinceEpoch();
    m_results[task->key] = info;

    emit pingUpdated(task->key, -1, Status::Offline);

    cleanupProbe(task);

    if (m_activeProbes.isEmpty()) {
        emit checkAllFinished();
    }
}

void NetworkHealthChecker::onSocketTimeout()
{
    QTimer *timer = qobject_cast<QTimer*>(sender());
    if (!timer) {
        return;
    }

    QTcpSocket *targetSocket = nullptr;
    ProbeTask *targetTask = nullptr;

    for (auto it = m_activeProbes.begin(); it != m_activeProbes.end(); ++it) {
        if (it.value() && it.value()->timeoutTimer == timer) {
            targetSocket = it.key();
            targetTask = it.value();
            m_activeProbes.erase(it);
            break;
        }
    }

    if (targetTask) {
        HealthInfo info;
        info.pingMs = -1;
        info.status = Status::Offline;
        info.lastCheckedMs = QDateTime::currentMSecsSinceEpoch();
        m_results[targetTask->key] = info;

        emit pingUpdated(targetTask->key, -1, Status::Offline);

        cleanupProbe(targetTask);
    }

    if (m_activeProbes.isEmpty()) {
        emit checkAllFinished();
    }
}

void NetworkHealthChecker::cleanupProbe(ProbeTask *task)
{
    if (!task) return;

    if (task->socket) {
        task->socket->abort();
        task->socket->deleteLater();
    }
    if (task->timeoutTimer) {
        task->timeoutTimer->stop();
        task->timeoutTimer->deleteLater();
    }
    delete task;
}

void NetworkHealthChecker::loadCache()
{
    QSettings settings;
    settings.beginGroup(settingsGroup);
    QStringList allKeys = settings.childKeys();
    for (const QString &k : allKeys) {
        if (k.endsWith("_host")) {
            QString code = k.left(k.length() - 5);
            QString host = settings.value(k).toString();
            int port = settings.value(QString("%1_port").arg(code), defaultPort).toInt();
            if (!host.isEmpty()) {
                m_targetHosts[code] = qMakePair(host, port);
            }
        }
    }
    settings.endGroup();
}

void NetworkHealthChecker::saveCache()
{
    QSettings settings;
    settings.beginGroup(settingsGroup);
    for (auto it = m_targetHosts.constBegin(); it != m_targetHosts.constEnd(); ++it) {
        settings.setValue(QString("%1_host").arg(it.key()), it.value().first);
        settings.setValue(QString("%1_port").arg(it.key()), it.value().second);
    }
    settings.endGroup();
}
