#pragma once
#include "protocol_types.h"
#include "thread_queue.h"
#include <string>

// Mock daemon message source for UI testing.
// Pushes DaemonMsg to the incoming queue (as if from TCP).
// Consumes BuddyMsg from the outgoing queue (as if to TCP).

class MockSource {
public:
    // q_in:  buddy's incoming queue (mock pushes here)
    // q_out: buddy's outgoing queue (mock consumes here)
    MockSource(ThreadQueue<DaemonMsg>* q_in, ThreadQueue<BuddyMsg>* q_out);

    // Call every frame (or on a timer). Generates messages.
    void update(uint32_t now_ms);

    // Force immediate state change for testing.
    void set_prompt(const std::string& tool, const std::string& hint);
    void clear_prompt();

private:
    ThreadQueue<DaemonMsg>* in_;
    ThreadQueue<BuddyMsg>* out_;

    uint32_t last_hb_ = 0;
    uint32_t connected_at_ = 0;
    bool connected_ = false;

    uint32_t tokens_ = 184502;
    uint32_t tokens_today_ = 31200;
    uint32_t running_ = 0;
    uint32_t waiting_ = 0;
    uint32_t total_ = 0;

    std::optional<Prompt> prompt_;
    std::vector<std::string> entries_;

    uint32_t next_scenario_change_ = 0;
    int scenario_ = 0;

    void emit_heartbeat();
    void emit_turn();
    void advance_scenario(uint32_t now);
    void consume_outgoing();
};
