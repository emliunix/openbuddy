#include "protocol_types.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <cstring>
#include <iostream>

// Upstream raw JSON samples from REFERENCE.md and data.h validation.
// These must parse exactly to ensure wire compatibility.

static void test_heartbeat_full() {
    const char* raw = R"({
        "total": 3,
        "running": 1,
        "waiting": 1,
        "completed": false,
        "msg": "approve: Bash",
        "entries": ["10:42 git push", "10:41 yarn test", "10:39 reading file..."],
        "tokens": 184502,
        "tokens_today": 31200,
        "prompt": {"id": "req_abc123", "tool": "Bash", "hint": "rm -rf /tmp/foo"}
    })";
    // Note: "completed" is not in REFERENCE.md but is present in the reference
    // implementation (data.h parses it; derive() uses it). Always include it;
    // default false for compatibility with impls that omit it.
    
    auto j = nlohmann::json::parse(raw);
    auto hb = j.get<Heartbeat>();
    
    assert(hb.total == 3);
    assert(hb.running == 1);
    assert(hb.waiting == 1);
    assert(hb.completed == false);
    assert(hb.msg == "approve: Bash");
    assert(hb.entries.size() == 3);
    assert(hb.entries[0] == "10:42 git push");
    assert(hb.tokens == 184502);
    assert(hb.tokens_today == 31200);
    assert(hb.prompt.has_value());
    assert(hb.prompt->id == "req_abc123");
    assert(hb.prompt->tool == "Bash");
    assert(hb.prompt->hint == "rm -rf /tmp/foo");
    
    std::cout << "[PASS] heartbeat_full\n";
}

static void test_heartbeat_partial_fallback() {
    // Simulate receiving a partial heartbeat (only running changed).
    // Upstream semantics: missing fields keep their previous value.
    Heartbeat prev;
    prev.total = 5;
    prev.running = 2;
    prev.waiting = 1;
    prev.completed = true;
    prev.msg = "previous msg";
    prev.tokens = 100000;
    prev.tokens_today = 5000;
    
    const char* partial = R"({"running": 3, "msg": "busy"})";
    auto j = nlohmann::json::parse(partial);
    auto hb = prev;  // start with previous state
    from_json(j, hb);  // apply partial update
    
    // Changed fields
    assert(hb.running == 3);
    assert(hb.msg == "busy");
    
    // completed absent → resets to false (compatibility default, not fallback)
    assert(hb.completed == false);

    // Unchanged fields (fallback semantics)
    assert(hb.total == 5);          // NOT overwritten to 0
    assert(hb.waiting == 1);        // NOT overwritten to 0
    assert(hb.tokens == 100000);    // NOT overwritten to 0
    assert(hb.tokens_today == 5000);// NOT overwritten to 0
    
    std::cout << "[PASS] heartbeat_partial_fallback\n";
}

static void test_heartbeat_prompt_clear() {
    // Upstream: when "prompt" key is present but null, clear the prompt.
    Heartbeat prev;
    prev.prompt = Prompt{"req_old", "Tool", "hint"};
    
    const char* raw = R"({"total": 1, "prompt": null})";
    auto j = nlohmann::json::parse(raw);
    auto hb = prev;
    from_json(j, hb);
    
    assert(hb.total == 1);
    assert(!hb.prompt.has_value());  // Cleared because prompt was null
    
    std::cout << "[PASS] heartbeat_prompt_clear\n";
}

static void test_heartbeat_prompt_preserve() {
    // Upstream: when "prompt" key is missing entirely, preserve previous prompt.
    Heartbeat prev;
    prev.prompt = Prompt{"req_old", "Tool", "hint"};
    
    const char* raw = R"({"total": 1})";
    auto j = nlohmann::json::parse(raw);
    auto hb = prev;
    from_json(j, hb);
    
    assert(hb.total == 1);
    assert(hb.prompt.has_value());  // Preserved because prompt key was missing
    assert(hb.prompt->id == "req_old");
    
    std::cout << "[PASS] heartbeat_prompt_preserve\n";
}

static void test_heartbeat_tokens_type_check() {
    // Upstream: tokens only updated if key exists AND is uint32_t.
    // String or null should be ignored.
    Heartbeat prev;
    prev.tokens = 1000;
    
    const char* raw = R"({"tokens": "not_a_number"})";
    auto j = nlohmann::json::parse(raw);
    auto hb = prev;
    from_json(j, hb);
    
    // String should be ignored (not a valid uint32_t)
    // Note: nlohmann/json may throw or convert; our code checks is_number
    try {
        from_json(j, hb);
        // If we get here, it means the string was ignored
        assert(hb.tokens == 1000);
    } catch (...) {
        // If it throws, that's also acceptable (rejects bad data)
        std::cout << "[INFO] tokens_type_check: exception on string (acceptable)\n";
    }
    
    std::cout << "[PASS] heartbeat_tokens_type_check\n";
}

static void test_turn_event() {
    // Wire format: content is a JSON array (not a string field named "content_json").
    const char* raw = R"({"evt": "turn", "role": "assistant", "content": [{"type":"text","text":"hello"}]})";
    
    auto j = nlohmann::json::parse(raw);
    auto te = j.get<TurnEvent>();
    
    assert(te.evt == "turn");
    assert(te.role == "assistant");
    // content stored as serialized JSON string
    auto parsed = nlohmann::json::parse(te.content);
    assert(parsed.is_array());
    assert(parsed[0]["type"] == "text");
    assert(parsed[0]["text"] == "hello");
    
    std::cout << "[PASS] turn_event\n";
}

static void test_time_sync() {
    const char* raw = R"({"time": [1775731234, -25200]})";
    
    auto j = nlohmann::json::parse(raw);
    auto ts = j.get<TimeSync>();
    
    assert(ts.epoch == 1775731234);
    assert(ts.tz_offset == -25200);
    
    std::cout << "[PASS] time_sync\n";
}

static void test_owner_cmd() {
    const char* raw = R"({"cmd": "owner", "name": "Felix"})";
    
    auto j = nlohmann::json::parse(raw);
    auto oc = j.get<OwnerCmd>();
    
    assert(oc.cmd == "owner");
    assert(oc.name == "Felix");
    
    std::cout << "[PASS] owner_cmd\n";
}

static void test_permission_cmd() {
    const char* raw = R"({"cmd": "permission", "id": "req_abc123", "decision": "once"})";
    
    auto j = nlohmann::json::parse(raw);
    auto pc = j.get<PermissionCmd>();
    
    assert(pc.cmd == "permission");
    assert(pc.id == "req_abc123");
    assert(pc.decision == "once");
    
    std::cout << "[PASS] permission_cmd\n";
}

static void test_variant_parsing() {
    // Test parse_daemon_msg dispatches correctly
    auto hb_j = nlohmann::json::parse(R"({"total": 1, "msg": "test"})");
    auto te_j = nlohmann::json::parse(R"({"evt": "turn", "role": "user"})");
    auto ts_j = nlohmann::json::parse(R"({"time": [1, 2]})");
    auto oc_j = nlohmann::json::parse(R"({"cmd": "owner", "name": "X"})");
    
    auto hb = parse_daemon_msg(hb_j);
    auto te = parse_daemon_msg(te_j);
    auto ts = parse_daemon_msg(ts_j);
    auto oc = parse_daemon_msg(oc_j);
    
    assert(std::holds_alternative<Heartbeat>(hb));
    assert(std::holds_alternative<TurnEvent>(te));
    assert(std::holds_alternative<TimeSync>(ts));
    assert(std::holds_alternative<OwnerCmd>(oc));
    
    std::cout << "[PASS] variant_parsing\n";
}

static void test_serialization_roundtrip() {
    // Ensure serialize->deserialize preserves data
    Heartbeat original;
    original.total = 3;
    original.running = 1;
    original.waiting = 1;
    original.msg = "test msg";
    original.entries = {"a", "b"};
    original.tokens = 1000;
    original.tokens_today = 500;
    original.prompt = Prompt{"id1", "Tool", "hint"};
    
    nlohmann::json j = original;
    auto restored = j.get<Heartbeat>();
    
    assert(restored.total == original.total);
    assert(restored.running == original.running);
    assert(restored.waiting == original.waiting);
    assert(restored.msg == original.msg);
    assert(restored.entries == original.entries);
    assert(restored.tokens == original.tokens);
    assert(restored.tokens_today == original.tokens_today);
    assert(restored.prompt->id == original.prompt->id);
    
    std::cout << "[PASS] serialization_roundtrip\n";
}

int main() {
    std::cout << "=== Protocol Types Unit Tests ===\n";
    
    test_heartbeat_full();
    test_heartbeat_partial_fallback();
    test_heartbeat_prompt_clear();
    test_heartbeat_prompt_preserve();
    test_heartbeat_tokens_type_check();
    test_turn_event();
    test_time_sync();
    test_owner_cmd();
    test_permission_cmd();
    test_variant_parsing();
    test_serialization_roundtrip();
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
