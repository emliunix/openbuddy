#pragma once
#include "transport.h"
#include "protocol_types.h"
#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <string>
#include <optional>

// Mock transport for UI testing without network.
// Implements ITransport, generating simulated DaemonMsg events
// via the same callback path as AsioTransport.
//
// Scenario-based: each scenario is a sequence of workflow states
// that simulate realistic Claude Code sessions.

struct ScenarioStep {
    uint32_t duration_ms;           // how long this state lasts
    uint32_t running;               // tool calls running
    uint32_t waiting;               // sessions waiting on prompt
    uint32_t total;                 // total sessions
    std::vector<std::string> entries;  // transcript lines
    std::string msg;                // status message
    std::optional<Prompt> prompt;   // pending permission request
    bool emit_turn;                 // whether to emit a TurnEvent
    std::string turn_content;       // content for turn event
};

struct Scenario {
    const char* name;
    std::vector<ScenarioStep> steps;
};

class MockTransport : public ITransport {
public:
    explicit MockTransport(std::function<void(DaemonMsg)> on_msg);
    ~MockTransport() override;

    // ITransport
    void start() override;
    void send(const BuddyMsg& msg) override;
    bool is_connected() const override;

    // Non-copyable, non-movable (owns thread)
    MockTransport(const MockTransport&) = delete;
    MockTransport& operator=(const MockTransport&) = delete;
    MockTransport(MockTransport&&) = delete;
    MockTransport& operator=(MockTransport&&) = delete;

private:
    std::function<void(DaemonMsg)> on_msg_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;

    uint32_t tokens_ = 184502;
    uint32_t tokens_today_ = 31200;
    uint32_t running_ = 0;
    uint32_t waiting_ = 0;
    uint32_t total_ = 0;

    std::optional<Prompt> prompt_;
    std::vector<std::string> entries_;

    // Scenario state
    size_t scenario_idx_ = 0;
    size_t step_idx_ = 0;
    uint64_t step_deadline_ = 0;

    // Pre-defined scenarios
    static const std::vector<Scenario> SCENARIOS;

    void loop();
    void load_step(const ScenarioStep& step);
    void emit_heartbeat();
    void emit_turn(const std::string& content);
    void advance_step();
    void handle_buddy_msg(const BuddyMsg& msg);
};