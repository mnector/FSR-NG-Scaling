#include "config_reader.h"
#include <fstream>
#include <sstream>

namespace fsrng {

ConfigReader::ConfigReader(std::string path) : path_(std::move(path)) {
    Load();
}

std::string ConfigReader::Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool ConfigReader::Load() {
    std::lock_guard<std::mutex> lk(mutex_);
    data_.clear();

    std::ifstream in(path_);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        // Strip comments.
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '[') continue; // section header

        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = Trim(trimmed.substr(0, eq));
        std::string value = Trim(trimmed.substr(eq + 1));
        data_[key] = value;
    }
    return true;
}

void ConfigReader::Reload() { Load(); }

std::string ConfigReader::GetString(const std::string& key, const std::string& def) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = data_.find(key);
    return it == data_.end() ? def : it->second;
}

int ConfigReader::GetInt(const std::string& key, int def) const {
    std::string v = GetString(key);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

float ConfigReader::GetFloat(const std::string& key, float def) const {
    std::string v = GetString(key);
    if (v.empty()) return def;
    try { return std::stof(v); } catch (...) { return def; }
}

bool ConfigReader::GetBool(const std::string& key, bool def) const {
    std::string v = GetString(key);
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "True";
}

void ConfigReader::SetString(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mutex_);
    data_[key] = value;

    // Rewrite the file preserving section structure is complex; instead we do a
    // targeted in-place update of the existing line if present.
    std::ifstream in(path_);
    std::string line, out;
    bool replaced = false;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos && Trim(line.substr(0, eq)) == key) {
            // Preserve inline comment.
            auto hash = line.find('#');
            std::string comment;
            if (hash != std::string::npos) comment = "  //" + line.substr(hash);
            out += key + "=" + value + comment + "\n";
            replaced = true;
        } else {
            out += line + "\n";
        }
    }
    in.close();

    if (!replaced) out += key + "=" + value + "\n";

    std::ofstream o(path_, std::ios::trunc);
    o << out;
}

} // namespace fsrng
