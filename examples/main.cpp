#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDebug>
#include <QDateTime>

#include "client.h"

// ============================================================================
// Logging
// ============================================================================

void logHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    const char* lvl = "?";
    switch (type) {
        case QtDebugMsg:    lvl = "DBG"; break;
        case QtInfoMsg:     lvl = "INF"; break;
        case QtWarningMsg:  lvl = "WRN"; break;
        case QtCriticalMsg: lvl = "ERR"; break;
        case QtFatalMsg:    lvl = "FTL"; break;
    }
    fprintf(stderr, "[%s] [%s] %s\n", qPrintable(ts), lvl, qPrintable(msg));
    if (type == QtFatalMsg) abort();
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("pubsub_example_client");
    QCoreApplication::setApplicationVersion("1.0");

    qInstallMessageHandler(logHandler);

    // ---- Command-line parsing ----
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Example pub/sub client for production_service.\n"
        "Demonstrates both permanent and one-shot connection modes.\n\n"
        "Examples:\n"
        "  Permanent + full:\n"
        "    pubsub_example_client --host 127.0.0.1 --port 9100 --mode permanent --sub full\n\n"
        "  One-shot + notify_only:\n"
        "    pubsub_example_client --host 127.0.0.1 --port 9100 --mode one_shot --sub notify_only --callback-port 9200\n\n"
        "  Resume from event 42:\n"
        "    pubsub_example_client --host 127.0.0.1 --port 9100 --mode permanent --sub full --resume 42"
    );
    parser.addHelpOption();
    parser.addVersionOption();

    // Options
    QCommandLineOption hostOpt(
        QStringList() << "H" << "host",
        "Publisher server host (default: 127.0.0.1).",
        "host", "127.0.0.1"
    );
    QCommandLineOption portOpt(
        QStringList() << "p" << "port",
        "Publisher server port (default: 9100).",
        "port", "9100"
    );
    QCommandLineOption modeOpt(
        QStringList() << "m" << "mode",
        "Connection mode: permanent or one_shot (default: permanent).",
        "mode", "permanent"
    );
    QCommandLineOption subOpt(
        QStringList() << "s" << "sub",
        "Subscription mode: full or notify_only (default: full).",
        "sub", "full"
    );
    QCommandLineOption clientIdOpt(
        QStringList() << "i" << "id",
        "Client ID (default: example_client).",
        "id", "example_client"
    );
    QCommandLineOption resumeOpt(
        QStringList() << "r" << "resume",
        "Resume from event ID (default: 0 = all).",
        "event_id", "0"
    );
    QCommandLineOption callbackPortOpt(
        QStringList() << "c" << "callback-port",
        "Local callback port for one_shot mode (default: 9200).",
        "port", "9200"
    );

    parser.addOption(hostOpt);
    parser.addOption(portOpt);
    parser.addOption(modeOpt);
    parser.addOption(subOpt);
    parser.addOption(clientIdOpt);
    parser.addOption(resumeOpt);
    parser.addOption(callbackPortOpt);

    parser.process(app);

    // ---- Read values ----
    QString host = parser.value(hostOpt);
    quint16 port = static_cast<quint16>(parser.value(portOpt).toUShort());
    QString mode = parser.value(modeOpt);
    QString sub = parser.value(subOpt);
    QString clientId = parser.value(clientIdOpt);
    qint64 resumeFrom = parser.value(resumeOpt).toLongLong();
    quint16 callbackPort = static_cast<quint16>(parser.value(callbackPortOpt).toUShort());

    // ---- Validate ----
    if (mode != "permanent" && mode != "one_shot") {
        qCritical() << "Invalid mode:" << mode << "(must be 'permanent' or 'one_shot')";
        return 1;
    }
    if (sub != "full" && sub != "notify_only") {
        qCritical() << "Invalid subscription mode:" << sub << "(must be 'full' or 'notify_only')";
        return 1;
    }

    // ---- Banner ----
    qInfo() << "========================================";
    qInfo() << " Pub/Sub Example Client v1.0";
    qInfo() << "========================================";
    qInfo() << " Server:       " << host << ":" << port;
    qInfo() << " Mode:         " << mode;
    qInfo() << " Subscription: " << sub;
    qInfo() << " Client ID:    " << clientId;
    qInfo() << " Resume from:  " << resumeFrom;
    if (mode == "one_shot") {
        qInfo() << " Callback port:" << callbackPort;
    }
    qInfo() << "========================================";

    // ---- Create client ----
    ExampleClient client;

    QObject::connect(&client, &ExampleClient::connected, []() {
        qInfo() << ">>> Subscription active — waiting for events...";
    });

    QObject::connect(&client, &ExampleClient::disconnected, []() {
        qWarning() << ">>> Disconnected from server";
    });

    QObject::connect(&client, &ExampleClient::errorOccurred, [](const QString& err) {
        qCritical() << ">>> Error:" << err;
    });

    // ---- Connect ----
    if (mode == "permanent") {
        client.connectPermanent(host, port, clientId, sub, resumeFrom);
    } else {
        client.connectOneShot(host, port, clientId, sub, callbackPort, resumeFrom);
    }

    return app.exec();
}
