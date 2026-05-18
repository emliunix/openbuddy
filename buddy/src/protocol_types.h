#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

// Wire protocol types.
// Plain aggregates with nlohmann/json serialization support.
// See docs/PROTOCOL.md for field meanings.
//
// Optional fields per upstream spec (REFERENCE.md):
//   - Heartbeat.prompt: optional, absent when no permission pending
//   - All other heartbeat fields: always present in full heartbeats, but
//     partial heartbeats may omit keys; fallback to previous value (upstream
//     semantics in data.h:_applyJson).

struct Prompt {
    std::string id;
    std::string tool;
    std::string hint;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Prompt, id, tool, hint)

struct Heartbeat {
    bool     completed = false; // one-shot: true for one heartbeat after a turn finishes
    std::vector<std::string> entries;
    std::string msg;
    std::optional<Prompt> prompt;
    uint32_t running = 0;
    uint32_t tokens = 0;
    uint32_t tokens_today = 0;
    uint32_t total = 0;
    uint32_t waiting = 0;
};

inline void to_json(nlohmann::json& j, const Heartbeat& h) {
    j = nlohmann::json{
        {"completed", h.completed},
        {"entries", h.entries},
        {"msg", h.msg},
        {"running", h.running},
        {"tokens", h.tokens},
        {"tokens_today", h.tokens_today},
        {"total", h.total},
        {"waiting", h.waiting}
    };
    if (h.prompt) j["prompt"] = *h.prompt;
}
inline void from_json(const nlohmann::json& j, Heartbeat& h) {
    // Field fallback: if key is missing, keep current value (don't overwrite with default)
    if (j.contains("completed")) j.at("completed").get_to(h.completed);
    else h.completed = false; // default to false for compatibility with impls that omit it
    if (j.contains("entries")) j.at("entries").get_to(h.entries);
    if (j.contains("msg")) j.at("msg").get_to(h.msg);
    if (j.contains("prompt") && !j.at("prompt").is_null())
        h.prompt = j.at("prompt").get<Prompt>();
    else if (j.contains("prompt") && j.at("prompt").is_null())
        h.prompt = std::nullopt;
    // else: keep previous prompt value (fallback semantics)
    if (j.contains("running")) j.at("running").get_to(h.running);
    if (j.contains("tokens")) {
        // Only update if actually a uint32_t (matches upstream type check)
        auto& t = j.at("tokens");
        if (t.is_number_unsigned() || (t.is_number() && t.get<int64_t>() >= 0))
            t.get_to(h.tokens);
    }
    if (j.contains("tokens_today")) j.at("tokens_today").get_to(h.tokens_today);
    if (j.contains("total")) j.at("total").get_to(h.total);
    if (j.contains("waiting")) j.at("waiting").get_to(h.waiting);
}

struct TurnEvent {
    std::string evt = "turn";
    std::string role;
    // content is the raw SDK content array (MIME-typed blocks: {type, text, ...})
    // stored as a serialized JSON string for forwarding without full schema binding
    std::string content;
};

inline void to_json(nlohmann::json& j, const TurnEvent& t) {
    j = nlohmann::json{{"evt", t.evt}, {"role", t.role}};
    if (!t.content.empty()) j["content"] = nlohmann::json::parse(t.content);
}
inline void from_json(const nlohmann::json& j, TurnEvent& t) {
    j.at("evt").get_to(t.evt);
    j.at("role").get_to(t.role);
    if (j.contains("content")) t.content = j.at("content").dump();
    else t.content.clear();
}

struct TimeSync {
    // Wire format: {"time": [epoch, tz_offset]}
    uint32_t epoch = 0;
    int32_t tz_offset = 0;
};

inline void to_json(nlohmann::json& j, const TimeSync& t) {
    j = nlohmann::json{{"time", {t.epoch, t.tz_offset}}};
}
inline void from_json(const nlohmann::json& j, TimeSync& t) {
    auto arr = j.at("time").get<std::vector<int64_t>>();
    t.epoch = static_cast<uint32_t>(arr[0]);
    t.tz_offset = static_cast<int32_t>(arr[1]);
}

struct OwnerCmd {
    std::string cmd = "owner";
    std::string name;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OwnerCmd, cmd, name)

struct GenericCmd {
    std::string cmd;
    std::optional<std::string> name;
};

inline void to_json(nlohmann::json& j, const GenericCmd& c) {
    j = nlohmann::json{{"cmd", c.cmd}};
    if (c.name) j["name"] = *c.name;
}
inline void from_json(const nlohmann::json& j, GenericCmd& c) {
    j.at("cmd").get_to(c.cmd);
    if (j.contains("name") && !j.at("name").is_null())
        c.name = j.at("name").get<std::string>();
    else
        c.name = std::nullopt;
}

struct PermissionCmd {
    std::string cmd = "permission";
    std::string decision;
    std::string id;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PermissionCmd, cmd, decision, id)

struct StatusAck {
    std::string ack = "status";
    bool ok = true;
    // data is intentionally omitted for brevity; add StatusData when needed
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StatusAck, ack, ok)

struct PingCmd {
    std::string cmd = "ping";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PingCmd, cmd)

// Folder push
struct CharBegin {
    std::string cmd = "char_begin";
    std::string name;
    uint32_t total = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CharBegin, cmd, name, total)

struct FileMeta {
    std::string cmd = "file";
    std::string path;
    uint32_t size = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileMeta, cmd, path, size)

struct Chunk {
    std::string cmd = "chunk";
    std::string d; // base64
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Chunk, cmd, d)

struct FileEnd {
    std::string cmd = "file_end";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileEnd, cmd)

struct CharEnd {
    std::string cmd = "char_end";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CharEnd, cmd)

// Internal: emitted by transport layer on disconnect. Never serialized.
struct TransportDisconnect {
    std::string reason;
};

// Discriminated union for incoming messages
using DaemonMsg = std::variant<
    Heartbeat,
    TurnEvent,
    TimeSync,
    OwnerCmd,
    GenericCmd,
    CharBegin,
    FileMeta,
    Chunk,
    FileEnd,
    CharEnd,
    TransportDisconnect
>;

using BuddyMsg = std::variant<
    PermissionCmd,
    StatusAck,
    PingCmd
>;

// ------------------------------------------------------------------
// Variant serialization helpers
// ------------------------------------------------------------------

inline void to_json(nlohmann::json& j, const DaemonMsg& m) {
    std::visit([&j](const auto& v) { j = v; }, m);
}

inline void to_json(nlohmann::json& j, const BuddyMsg& m) {
    std::visit([&j](const auto& v) { j = v; }, m);
}

// Parse a DaemonMsg from JSON by inspecting fields.
//
// Upstream parsing cascade (data.h:_applyJson):
//   1. xferCommand(doc) — checks doc["cmd"]; if present and known, handles it
//   2. doc["time"]      — if array of size 2, handles time sync
//   3. everything else  — treated as heartbeat (fields read with fallback)
//
// Note: upstream NEVER checks for "evt". Turn events fall through to heartbeat
// and are silently ignored. We detect them here because they're documented in
// REFERENCE.md, but the device code upstream doesn't use them.
//
// Throws nlohmann::json::exception on failure.
inline DaemonMsg parse_daemon_msg(const nlohmann::json& j) {
    // Step 1: cmd field present -> command dispatch (matches xferCommand)
    if (j.contains("cmd")) {
        std::string cmd = j.at("cmd").get<std::string>();
        if (cmd == "owner") return j.get<OwnerCmd>();
        if (cmd == "char_begin") return j.get<CharBegin>();
        if (cmd == "file") return j.get<FileMeta>();
        if (cmd == "chunk") return j.get<Chunk>();
        if (cmd == "file_end") return j.get<FileEnd>();
        if (cmd == "char_end") return j.get<CharEnd>();
        // GenericCmd catches: status, name, unpair, permission, unknown cmds
        // Upstream: permission falls through (xferCommand returns false),
        // unknown cmds also fall through. Here we map them to GenericCmd.
        return j.get<GenericCmd>();
    }

    // Step 2: time field present -> TimeSync (matches data.h line 77)
    if (j.contains("time")) {
        return j.get<TimeSync>();
    }

    // Step 3: evt field present -> TurnEvent
    // NOT in upstream device code, but documented in REFERENCE.md
    if (j.contains("evt")) {
        return j.get<TurnEvent>();
    }

    // Step 4: fallback -> Heartbeat
    // Upstream treats everything else as heartbeat; unknown fields are ignored
    return j.get<Heartbeat>();
}
