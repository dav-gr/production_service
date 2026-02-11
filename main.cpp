#include <QCoreApplication>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include "server/tcp_server.h"
#include "pubsub/event_publisher.h"

// ============================================================================
// Configuration
// ============================================================================

struct Config {
quint16 port = 8080;
quint16 pubsubPort = 9000;
    QString dbHost = "localhost";
    int dbPort = 5432;
    QString dbName = "prod_auto_dev";
    QString dbUser = "prod_auto_dev";
    QString dbPassword = "prod_auto_dev";
    int sessionMinutes = 480;
    int readTimeoutMsec = 5000;
    
    static Config load(const QString& path) {
        Config cfg;
        if (!QFileInfo::exists(path)) {
            qWarning() << "Config not found:" << path << "- using defaults";
            return cfg;
        }
        
        QSettings s(path, QSettings::IniFormat);
        
        s.beginGroup("Server");
        cfg.port = static_cast<quint16>(s.value("Port", 8080).toUInt());
        cfg.pubsubPort = static_cast<quint16>(s.value("PubSubPort", 9000).toUInt());
        cfg.readTimeoutMsec = s.value("ReadTimeoutMsec", 5000).toInt();
        s.endGroup();
        
        s.beginGroup("Database");
        cfg.dbHost = s.value("Host", "localhost").toString();
        cfg.dbPort = s.value("Port", 5432).toInt();
        cfg.dbName = s.value("Database", "prod_auto_dev").toString();
        cfg.dbUser = s.value("Username", "prod_auto_dev").toString();
        cfg.dbPassword = s.value("Password", "prod_auto_dev").toString();
        s.endGroup();
        
        s.beginGroup("Session");
        cfg.sessionMinutes = s.value("ExpirationMinutes", 480).toInt();
        s.endGroup();
        
        return cfg;
    }
};

// ============================================================================
// Logging
// ============================================================================

void logHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
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
    qInstallMessageHandler(logHandler);
    
    qInfo() << "=== PostProd Middleware v1.0 ===";
    
    // Config path
    QString cfgPath = QDir(app.applicationDirPath()).filePath("config.ini");
    for (int i = 1; i < argc - 1; ++i) {
        if (QString(argv[i]) == "--config") {
            cfgPath = argv[i + 1];
            break;
        }
    }
    
    Config cfg = Config::load(cfgPath);
    
    qInfo() << "Request/Response Port:" << cfg.port;
    qInfo() << "Pub/Sub Port:" << cfg.pubsubPort;
    qInfo() << "Database:" << cfg.dbHost << cfg.dbPort << cfg.dbName;
    
    // Create database config
    core::AppConfig dbConfig;
    dbConfig.host = cfg.dbHost;
    dbConfig.port = cfg.dbPort;
    dbConfig.database = cfg.dbName;
    dbConfig.user = cfg.dbUser;
    dbConfig.password = cfg.dbPassword;
    
    // Create and start request/response server
    server::TcpServer server;
    server.setReadTimeout(cfg.readTimeoutMsec);
    
    if (!server.connectDatabase(cfg.dbHost, cfg.dbPort, cfg.dbName, 
                                 cfg.dbUser, cfg.dbPassword)) {
        return 1;
    }
    
    server.setSessionExpiration(cfg.sessionMinutes);
    
    if (!server.startServer(cfg.port)) {
        return 1;
    }
    
    qInfo() << "=== Request/Response Server ready on port" << cfg.port << "===";
    
    // Create and start pub/sub event publisher
    pubsub::EventPublisher publisher(dbConfig);
    
    if (!publisher.start(cfg.pubsubPort)) {
        qCritical() << "Failed to start pub/sub publisher";
        return 1;
    }
    
    qInfo() << "=== Pub/Sub Publisher ready on port" << cfg.pubsubPort << "===";
    qInfo() << "=== All services ready ===";
    
    return app.exec();
}
