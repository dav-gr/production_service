#ifndef SERVER_PROTOCOL_H
#define SERVER_PROTOCOL_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <optional>

namespace server {

// ============================================================================
// Request
// ============================================================================

struct Request {
    QString action;
    QJsonObject data;
    
    static std::optional<Request> fromJson(const QByteArray& json, QString* errorOut = nullptr) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
        
        if (parseError.error != QJsonParseError::NoError) {
            if (errorOut) *errorOut = "JSON parse error: " + parseError.errorString();
            return std::nullopt;
        }
        
        if (!doc.isObject()) {
            if (errorOut) *errorOut = "Request must be a JSON object";
            return std::nullopt;
        }
        
        QJsonObject obj = doc.object();
        
        Request req;
        req.action = obj.value("action").toString();
        req.data = obj.value("data").toObject();
        
        if (req.action.isEmpty()) {
            if (errorOut) *errorOut = "Missing 'action' field";
            return std::nullopt;
        }
        
        return req;
    }
    
    QString getString(const QString& key, const QString& def = {}) const {
        return data.value(key).toString(def);
    }
};

// ============================================================================
// Response
// ============================================================================

struct Response {
    bool success = false;
    QString message;
    QJsonObject data;
    
    QByteArray toJson() const {
        QJsonObject obj;
        obj["status"] = success ? "success" : "error";
        obj["message"] = message;
        if (!data.isEmpty()) {
            obj["data"] = data;
        }
        return QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    }
    
    static Response ok(const QString& msg, const QJsonObject& data = {}) {
        return Response{true, msg, data};
    }
    
    static Response error(const QString& msg, const QJsonObject& data = {}) {
        return Response{false, msg, data};
    }
};

} // namespace server

#endif // SERVER_PROTOCOL_H
