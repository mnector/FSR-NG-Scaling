#pragma once
#include <string>
#include <map>
#include <mutex>

namespace fsrng {

// Reads/writes an INI file with live auto-reload. All access is thread-safe so
// the hotkey manager can reload settings.ini while the render loop runs.
class ConfigReader {
public:
    explicit ConfigReader(std::string path = "config/settings.ini");

    // Load from disk (thread-safe). Returns false if the file cannot be read.
    bool Load();

    // Force a re-read of the file on disk.
    void Reload();

    std::string GetString(const std::string& key, const std::string& def = "") const;
    int         GetInt(const std::string& key, int def = 0) const;
    float       GetFloat(const std::string& key, float def = 0.0f) const;
    bool        GetBool(const std::string& key, bool def = false) const;

    // Write a single key (used by future live-editing UIs). Persists to disk.
    void SetString(const std::string& key, const std::string& value);

    const std::string& path() const { return path_; }

private:
    std::string path_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> data_;

    static std::string Trim(const std::string& s);
};

} // namespace fsrng
