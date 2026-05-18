#include "persistence.h"
#include "session.h"
#include "stats.h"
#include "log.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace persistence {

static std::string config_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = ".";
    return std::string(home) + "/.config/openbuddy";
}

const char* state_path() {
    static std::string path = config_dir() + "/state.json";
    return path.c_str();
}

static bool ensure_dir(const std::string& dir) {
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return true;
    // mkdir -p
    size_t pos = 0;
    while ((pos = dir.find('/', pos + 1)) != std::string::npos) {
        std::string sub = dir.substr(0, pos);
        mkdir(sub.c_str(), 0755);
    }
    return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool save(const AppState& app) {
    nlohmann::json j;
    j["version"] = 1;

    // Stats
    const Stats& s = stats();
    j["stats"]["nap_seconds"] = s.nap_seconds;
    j["stats"]["approvals"] = s.approvals;
    j["stats"]["denials"] = s.denials;
    j["stats"]["level"] = s.level;
    j["stats"]["tokens"] = s.tokens;

    // Settings
    const Settings& set = settings();
    j["settings"]["species_idx"] = set.species_idx;

    // Names
    j["owner_name"] = owner_name();
    j["pet_name"] = pet_name();

    std::string dir = config_dir();
    if (!ensure_dir(dir)) {
        LOG_ERROR("[persist] failed to create dir %s", dir.c_str());
        return false;
    }

    std::ofstream f(state_path());
    if (!f) {
        LOG_ERROR("[persist] failed to open %s", state_path());
        return false;
    }
    f << j.dump(2);
    LOG_INFO("[persist] saved to %s", state_path());
    return true;
}

bool load(AppState* app) {
    std::ifstream f(state_path());
    if (!f) return false;

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        LOG_ERROR("[persist] parse error: %s", e.what());
        return false;
    }

    if (j.contains("stats")) {
        Stats& s = stats();
        auto& sj = j["stats"];
        if (sj.contains("nap_seconds")) sj["nap_seconds"].get_to(s.nap_seconds);
        if (sj.contains("approvals")) sj["approvals"].get_to(s.approvals);
        if (sj.contains("denials")) sj["denials"].get_to(s.denials);
        if (sj.contains("level")) sj["level"].get_to(s.level);
        if (sj.contains("tokens")) sj["tokens"].get_to(s.tokens);
    }

    if (j.contains("settings")) {
        if (j["settings"].contains("species_idx"))
            settings().species_idx = j["settings"]["species_idx"].get<uint8_t>();
    }

    LOG_INFO("[persist] loaded from %s", state_path());
    return true;
}

} // namespace persistence
