#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <filesystem>

namespace aggregation {

// ============================================================================
// Log levels
// ============================================================================

enum class LogLevel { 
    Info = 0, 
    Warning = 1, 
    Error = 2 
};

inline const char* to_string(LogLevel level)
{
    switch (level) {
    case LogLevel::Info:    return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error:   return "ERROR";
    }
    return "???";
}

// ============================================================================
// LogSession – creates the session folder, shared by all loggers in a run
//
// Folder structure:
//   logs/
//     2025-01-15_14-30-05/
//       system.log
//       item-scan.log
//       group-scan.log
//       box-scan.log
//       validation.log
//       modbus.log
//       db-export.log
//       ...
// ============================================================================

class LogSession {
public:
    /// Creates folder: base_path/YYYY-MM-DD_HH-MM-SS/
    explicit LogSession(const std::string& base_path = "logs")
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &time_t);
#else
        localtime_r(&time_t, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
        m_session_dir = base_path + "/" + oss.str();
        std::filesystem::create_directories(m_session_dir);
    }

    const std::string& sessionDir() const { return m_session_dir; }

    /// Set minimum console log level for ALL loggers in this session
    void setConsoleLogLevel(LogLevel level) { m_console_min_level = level; }
    LogLevel getConsoleLogLevel() const { return m_console_min_level; }

private:
    std::string m_session_dir;
    LogLevel m_console_min_level = LogLevel::Info;  // Default: show all levels
};

// ============================================================================
// Logger – one instance per logical component
//
// Thread safety model:
//   File writes   – NOT internally synchronized. Safe when:
//                   (a) logger used from a single thread, or
//                   (b) caller holds an external mutex (e.g. line_state::m_mutex)
//                   If neither applies, construct with thread_safe=true.
//   Console writes – always synchronized (shared static mutex).
//
// Testing:
//   Construct with timestamps=false for deterministic golden-output comparison.
//   Format without timestamps: [INFO] [item-scan] scan ABC123
//
// Console Log Level Filtering:
//   - Set minimum console level per logger: logger.setConsoleLogLevel(LogLevel::Warning)
//   - Set global minimum for all loggers: session.setConsoleLogLevel(LogLevel::Error)
//   - File logs ALWAYS write all levels regardless of console filter
// ============================================================================

class Logger {
public:
    /// Normal: logs to session_dir/name.log + console
    Logger(const std::string& name, const LogSession& session,
           bool thread_safe = false)
        : m_name(name)
        , m_timestamps(true)
        , m_thread_safe(thread_safe)
        , m_session(&session)
    {
        std::string path = session.sessionDir() + "/" + name + ".log";
        m_file.open(path, std::ios::out | std::ios::app);
        if (!m_file.is_open()) {
            std::cerr << "Logger: failed to open " << path << std::endl;
        }
    }

    /// Test: logs to specific file, optional timestamps
    Logger(const std::string& name, const std::string& file_path,
           bool timestamps = false, bool thread_safe = false)
        : m_name(name)
        , m_timestamps(timestamps)
        , m_thread_safe(thread_safe)
        , m_session(nullptr)
    {
        m_file.open(file_path, std::ios::out | std::ios::app);
    }

    ~Logger()
    {
        if (m_file.is_open()) {
            m_file.flush();
            m_file.close();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // ---- Convenience methods ----

    void info(const std::string& msg)    { log(LogLevel::Info, msg); }
    void warning(const std::string& msg) { log(LogLevel::Warning, msg); }
    void error(const std::string& msg)   { log(LogLevel::Error, msg); }

    // Variadic: logger.info("scan", barcode, "group", gid);
    template<typename... Args>
    void info(Args&&... args)    { log(LogLevel::Info, concat(std::forward<Args>(args)...)); }

    template<typename... Args>
    void warning(Args&&... args) { log(LogLevel::Warning, concat(std::forward<Args>(args)...)); }

    template<typename... Args>
    void error(Args&&... args)   { log(LogLevel::Error, concat(std::forward<Args>(args)...)); }

    // ---- Configuration ----

    void setConsoleEnabled(bool v) { m_console = v; }
    void setTimestampsEnabled(bool v) { m_timestamps = v; }
    
    /// Set minimum console log level for this logger only
    /// Example: logger.setConsoleLogLevel(LogLevel::Warning) 
    ///          -> only WARN and ERROR appear on console, INFO is suppressed
    void setConsoleLogLevel(LogLevel level) { 
        m_console_min_level = level; 
        m_use_local_level = true;
    }
    
    /// Use the session's global console log level instead of local override
    void useSessionConsoleLogLevel() { m_use_local_level = false; }
    
    LogLevel getConsoleLogLevel() const { 
        if (m_use_local_level) {
            return m_console_min_level;
        }
        return m_session ? m_session->getConsoleLogLevel() : LogLevel::Info;
    }
    
    const std::string& name() const { return m_name; }

private:
    void log(LogLevel level, const std::string& msg)
    {
        std::string line = formatLine(level, msg);

        // File write - ALWAYS write all levels to file
        if (m_file.is_open()) {
            if (m_thread_safe) {
                std::lock_guard<std::mutex> lk(m_file_mutex);
                writeToFile(line, level);
            } else {
                writeToFile(line, level);
            }
        }

        // Console write - filter by minimum level
        if (m_console && shouldLogToConsole(level)) {
            std::lock_guard<std::mutex> lk(consoleMutex());
            if (level == LogLevel::Error) {
                std::cerr << line << std::endl;
            } else {
                std::cout << line << std::endl;
            }
        }
    }

    bool shouldLogToConsole(LogLevel level) const
    {
        LogLevel min_level = getConsoleLogLevel();
        return static_cast<int>(level) >= static_cast<int>(min_level);
    }

    void writeToFile(const std::string& line, LogLevel level)
    {
        m_file << line << '\n';
        // Flush immediately on warning/error for crash safety
        if (level != LogLevel::Info) {
            m_file.flush();
        }
    }

    std::string formatLine(LogLevel level, const std::string& msg) const
    {
        // [2025-01-15 14:30:05.123] [INFO] [Scanner-1] item scanned ABC123
        std::ostringstream oss;
        if (m_timestamps) {
            oss << '[' << timestamp() << "] ";
        }
        oss << '[' << to_string(level) << "] "
            << '[' << m_name << "] "
            << msg;
        return oss.str();
    }

    static std::string timestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    // ---- Variadic concat helper ----

    template<typename T>
    static void appendTo(std::ostringstream& oss, T&& val)
    {
        oss << std::forward<T>(val);
    }

    template<typename T, typename... Rest>
    static void appendTo(std::ostringstream& oss, T&& val, Rest&&... rest)
    {
        oss << std::forward<T>(val) << ' ';
        appendTo(oss, std::forward<Rest>(rest)...);
    }

    template<typename... Args>
    static std::string concat(Args&&... args)
    {
        std::ostringstream oss;
        appendTo(oss, std::forward<Args>(args)...);
        return oss.str();
    }

    static std::mutex& consoleMutex()
    {
        static std::mutex mtx;
        return mtx;
    }

    std::string          m_name;
    std::ofstream        m_file;
    std::mutex           m_file_mutex;   // only used when m_thread_safe == true
    bool                 m_console{ true };
    bool                 m_timestamps;
    bool                 m_thread_safe;
    LogLevel             m_console_min_level{ LogLevel::Info };
    bool                 m_use_local_level{ false };
    const LogSession*    m_session;
};

} // namespace aggregation
