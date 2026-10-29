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
    if (!m_results.contains(key)) {
        return Status::Checking;
    }
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

static QString getFallbackHostForCountry(const QString &key)
{
    const QString code = key.trimmed().toUpper();
    static const QHash<QString, QString> hosts = {
        { QStringLiteral("NL"), QStringLiteral("www.google.nl") },
        { QStringLiteral("DE"), QStringLiteral("www.google.de") },
        { QStringLiteral("SE"), QStringLiteral("www.google.se") },
        { QStringLiteral("FI"), QStringLiteral("www.google.fi") },
        { QStringLiteral("US"), QStringLiteral("www.google.com") },
        { QStringLiteral("GB"), QStringLiteral("www.google.co.uk") },
        { QStringLiteral("UK"), QStringLiteral("www.google.co.uk") },
        { QStringLiteral("FR"), QStringLiteral("www.google.fr") },
        { QStringLiteral("PL"), QStringLiteral("www.google.pl") },
        { QStringLiteral("TR"), QStringLiteral("www.google.com.tr") },
        { QStringLiteral("KZ"), QStringLiteral("www.google.kz") },
        { QStringLiteral("CH"), QStringLiteral("www.google.ch") },
        { QStringLiteral("ES"), QStringLiteral("www.google.es") },
        { QStringLiteral("IT"), QStringLiteral("www.google.it") },
        { QStringLiteral("CA"), QStringLiteral("www.google.ca") },
        { QStringLiteral("SG"), QStringLiteral("www.google.com.sg") },
        { QStringLiteral("JP"), QStringLiteral("www.google.co.jp") },
        { QStringLiteral("UA"), QStringLiteral("www.google.com.ua") },
        { QStringLiteral("AT"), QStringLiteral("www.google.at") },
        { QStringLiteral("CZ"), QStringLiteral("www.google.cz") },
        { QStringLiteral("RO"), QStringLiteral("www.google.ro") },
        { QStringLiteral("EE"), QStringLiteral("www.google.ee") },
        { QStringLiteral("LV"), QStringLiteral("www.google.lv") },
        { QStringLiteral("LT"), QStringLiteral("www.google.lt") },
        { QStringLiteral("MD"), QStringLiteral("www.google.md") },
        { QStringLiteral("GE"), QStringLiteral("www.google.ge") },
        { QStringLiteral("AM"), QStringLiteral("www.google.am") },
        { QStringLiteral("AZ"), QStringLiteral("www.google.az") },
        { QStringLiteral("AE"), QStringLiteral("www.google.ae") },
        { QStringLiteral("IL"), QStringLiteral("www.google.co.il") },
        { QStringLiteral("BR"), QStringLiteral("www.google.com.br") },
        { QStringLiteral("AU"), QStringLiteral("www.google.com.au") },
        { QStringLiteral("IN"), QStringLiteral("www.google.co.in") },
        { QStringLiteral("NO"), QStringLiteral("www.google.no") },
        { QStringLiteral("DK"), QStringLiteral("www.google.dk") },
        { QStringLiteral("BE"), QStringLiteral("www.google.be") },
        { QStringLiteral("IE"), QStringLiteral("www.google.ie") },
        { QStringLiteral("PT"), QStringLiteral("www.google.pt") },
        { QStringLiteral("BG"), QStringLiteral("www.google.bg") },
        { QStringLiteral("GR"), QStringLiteral("www.google.gr") },
        { QStringLiteral("HU"), QStringLiteral("www.google.hu") },
        { QStringLiteral("SK"), QStringLiteral("www.google.sk") },
        { QStringLiteral("IS"), QStringLiteral("www.google.is") },
        { QStringLiteral("RS"), QStringLiteral("www.google.rs") }
    };
    if (hosts.contains(code)) {
        return hosts.value(code);
    }
    return QStringLiteral("www.google.com");
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
        host = getFallbackHostForCountry(key);
        port = 443;
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
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !m_activeProbes.contains(socket)) {
        return;
    }

    ProbeTask *task = m_activeProbes.take(socket);

    if (socketError == QAbstractSocket::ConnectionRefusedError) {
        // TCP RST received: the host is active and responded!
        int elapsed = static_cast<int>(task->stopwatch.elapsed());
        HealthInfo info;
        info.pingMs = (elapsed > 0) ? elapsed : 1;
        info.status = Status::Online;
        info.lastCheckedMs = QDateTime::currentMSecsSinceEpoch();
        m_results[task->key] = info;
        saveCache();
        emit pingUpdated(task->key, info.pingMs, Status::Online);
        cleanupProbe(task);
        if (m_activeProbes.isEmpty()) {
            emit checkAllFinished();
        }
        return;
    }

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
