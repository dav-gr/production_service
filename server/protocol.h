#ifndef SERVER_PROTOCOL_H
#define SERVER_PROTOCOL_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <optional>

namespace server {

// ============================================================================
// Message type constants
// ============================================================================
namespace MessageType {
    constexpr const char* Scan   = "scan";
    constexpr const char* Action = "action";
    constexpr const char* Login  = "login";
    constexpr const char* Logout = "logout";
}

// ============================================================================
// Incoming request (unified for all message types)
// ============================================================================
struct Request {
    QString type;          // "scan", "action", "login", "logout"
    QJsonObject data;      // type-specific payload

    static std::optional<Request> fromJson(const QByteArray& json,
                                            QString* errorOut = nullptr) {
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
        req.type = obj.value("type").toString();
        if (req.type.isEmpty()) {
            // Backward compat: check "action" field for login/logout
            req.type = obj.value("action").toString();
        }
        req.data = obj;  // whole object is accessible
        if (req.type.isEmpty()) {
            if (errorOut) *errorOut = "Missing 'type' field";
            return std::nullopt;
        }
        return req;
    }

    QString getString(const QString& key, const QString& def = {}) const {
        return data.value(key).toString(def);
    }
};

// ============================================================================
// ScanResponse (Server -> App)
// ============================================================================
struct ScanResponse {
    QString     entityType;        // "item", "box", "pallet"
    QJsonObject context;           // entity context (state + parent info)
    QJsonArray  availableActions;

    QByteArray toJson() const {
        QJsonObject obj;
        obj["type"]              = "scan_response";
        obj["entity_type"]       = entityType;
        obj["context"]           = context;
        obj["available_actions"] = availableActions;
        return QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    }
};

// ============================================================================
// ActionResponse (Server -> App)
// ============================================================================
struct ActionResponse {
    bool    success = false;
    QString message;
    QJsonObject data;

    QByteArray toJson() const {
        QJsonObject obj;
        obj["type"]    = "action_response";
        obj["success"] = success;
        obj["message"] = message;
        if (!data.isEmpty()) obj["data"] = data;
        return QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    }

    static ActionResponse ok(const QString& msg, const QJsonObject& d = {}) {
        return ActionResponse{true, msg, d};
    }
    static ActionResponse error(const QString& msg, const QJsonObject& d = {}) {
        return ActionResponse{false, msg, d};
    }
};

// ============================================================================
// LoginResponse (backward compat shape for login/logout)
// ============================================================================
struct LoginResponse {
    bool success = false;
    QString message;
    QJsonObject data;

    QByteArray toJson() const {
        QJsonObject obj;
        obj["status"]  = success ? "success" : "error";
        obj["message"] = message;
        if (!data.isEmpty()) obj["data"] = data;
        return QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    }

    static LoginResponse ok(const QString& msg, const QJsonObject& d = {}) {
        return LoginResponse{true, msg, d};
    }
    static LoginResponse error(const QString& msg) {
        return LoginResponse{false, msg, {}};
    }
};

} // namespace server

#endif // SERVER_PROTOCOL_H
