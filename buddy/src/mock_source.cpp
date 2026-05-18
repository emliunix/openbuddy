#include "mock_source.h"
#include <cstdio>

MockSource::MockSource(ThreadQueue<DaemonMsg>* q_in, ThreadQueue<BuddyMsg>* q_out)
    : in_(q_in), out_(q_out) {
    entries_ = {"10:42 git push", "10:41 yarn test", "10:39 reading file..."};
}

void MockSource::update(uint32_t now_ms) {
    // Consume any BuddyMsgs (permission responses, etc.)
    consume_outgoing();

    if (!connected_) {
        connected_ = true;
        connected_at_ = now_ms;
        in_->push(TimeSync{1775731234, -25200});
        in_->push(OwnerCmd{"owner", "Felix"});
        emit_heartbeat();
        last_hb_ = now_ms;
        return;
    }

    // Heartbeat every 2s
    if (now_ms - last_hb_ >= 2000) {
        emit_heartbeat();
        last_hb_ = now_ms;
    }

    // Scenario changes every 6s
    if (now_ms >= next_scenario_change_) {
        advance_scenario(now_ms);
    }
}

void MockSource::set_prompt(const std::string& tool, const std::string& hint) {
    prompt_ = Prompt{"req_" + std::to_string(rand()), tool, hint};
    waiting_ = 1;
}

void MockSource::clear_prompt() {
    prompt_ = std::nullopt;
    waiting_ = 0;
}

void MockSource::emit_heartbeat() {
    std::string msg;
    if (waiting_ > 0) {
        msg = "approve: " + prompt_.value().tool;
    } else if (running_ > 0) {
        msg = std::to_string(running_) + " session" + (running_ > 1 ? "s" : "") + " running";
    } else if (total_ > 0) {
        msg = std::to_string(total_) + " session" + (total_ > 1 ? "s" : "") + " idle";
    } else {
        msg = "connected";
    }

    in_->push(Heartbeat{
        entries_,
        msg,
        prompt_,
        running_,
        tokens_,
        tokens_today_,
        total_,
        waiting_
    });
}

void MockSource::emit_turn() {
    in_->push(TurnEvent{"turn", "assistant", R"([{"type":"text","text":"hello"}])"});
}

void MockSource::consume_outgoing() {
    if (!out_) return;
    auto msg = out_->pop();
    while (msg) {
        std::visit([&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, PermissionCmd>) {
                if (m.decision == "once") {
                    printf("[mock] approved %s\n", m.id.c_str());
                } else {
                    printf("[mock] denied %s\n", m.id.c_str());
                }
                prompt_ = std::nullopt;
                waiting_ = 0;
                emit_heartbeat();
            }
        }, *msg);
        msg = out_->pop();
    }
}

void MockSource::advance_scenario(uint32_t now) {
    next_scenario_change_ = now + 6000;
    scenario_ = (scenario_ + 1) % 6;

    switch (scenario_) {
        case 0: // idle
            running_ = 0; waiting_ = 0; total_ = 1;
            prompt_ = std::nullopt;
            break;
        case 1: // busy
            running_ = 2; waiting_ = 0; total_ = 2;
            prompt_ = std::nullopt;
            tokens_ += 1500;
            tokens_today_ += 1500;
            break;
        case 2: // attention (prompt)
            running_ = 1; waiting_ = 1; total_ = 2;
            set_prompt("Bash", "rm -rf /tmp/foo");
            break;
        case 3: // prompt cleared (approved)
            running_ = 1; waiting_ = 0; total_ = 2;
            prompt_ = std::nullopt;
            emit_turn();
            break;
        case 4: // busy again
            running_ = 3; waiting_ = 0; total_ = 3;
            prompt_ = std::nullopt;
            tokens_ += 2000;
            tokens_today_ += 2000;
            break;
        case 5: // back to idle
            running_ = 0; waiting_ = 0; total_ = 1;
            prompt_ = std::nullopt;
            break;
    }
}
