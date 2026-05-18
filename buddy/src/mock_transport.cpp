#include "mock_transport.h"
#include <cstdio>
#include <cstring>
#include <SDL.h>
#include "log.h"

// ------------------------------------------------------------------
// Scenario definitions
// ------------------------------------------------------------------

// Scenario 1: Code Review
// Claude reads files, asks to run linter, shows results
static const Scenario SCENARIO_CODE_REVIEW = {
    "Code Review",
    {
        // Step 0: Claude starts reading the codebase
        {3000, 1, 0, 1,
         {"reading src/main.cpp", "connected to Claude"},
         "1 session running", std::nullopt, false, ""},

        // Step 1: Found issues, asks to run eslint
        {4000, 1, 1, 1,
         {"found 3 style issues", "reading src/main.cpp"},
         "approve: Bash",
         Prompt{"req_001", "Bash", "eslint --fix src/"}, false, ""},

        // Step 2: Approved, running linter
        {3000, 1, 0, 1,
         {"approved eslint", "found 3 style issues"},
         "1 session running", std::nullopt, true,
         R"([{"type":"text","text":"Running eslint --fix... Fixed 3 issues in src/main.js, 1 in utils.js"}])"},

        // Step 3: Idle with summary
        {3000, 0, 0, 1,
         {"eslint done: 4 fixed", "approved eslint"},
         "1 session idle", std::nullopt, false, ""},
    }
};

// Scenario 2: Bug Hunt
// User reports crash, Claude traces stack, finds null pointer
static const Scenario SCENARIO_BUG_HUNT = {
    "Bug Hunt",
    {
        // Step 0: Investigating crash report
        {3000, 1, 0, 1,
         {"reading crash.log", "user: app crashes on login"},
         "1 session running", std::nullopt, false, ""},

        // Step 1: Asks to run test to reproduce
        {4000, 1, 1, 1,
         {"found null in auth.js:42", "reading crash.log"},
         "approve: Bash",
         Prompt{"req_002", "Bash", "npm test -- auth.test.js"}, false, ""},

        // Step 2: Reproduced, looking at code
        {3000, 1, 0, 1,
         {"test failed: null ref", "found null in auth.js:42"},
         "1 session running", std::nullopt, true,
         R"([{"type":"text","text":"Reproduced! The user object is null when token expires. Checking session handler..."}])"},

        // Step 3: Proposes fix, asks to run again
        {4000, 1, 1, 1,
         {"proposed fix in auth.js", "test failed: null ref"},
         "approve: Bash",
         Prompt{"req_003", "Bash", "npm test -- auth.test.js"}, false, ""},

        // Step 4: Tests pass
        {3000, 0, 0, 1,
         {"tests passing (8/8)", "proposed fix in auth.js"},
         "1 session idle", std::nullopt, true,
         R"([{"type":"text","text":"All tests pass. The fix adds a null check before accessing user.token."}])"},
    }
};

// Scenario 3: Feature Build
// Claude creates a new API endpoint with tests
static const Scenario SCENARIO_FEATURE_BUILD = {
    "Feature Build",
    {
        // Step 0: Planning the feature
        {3000, 1, 0, 1,
         {"designing /api/v2/users", "user: add user search"},
         "1 session running", std::nullopt, false, ""},

        // Step 1: Asks to create file
        {4000, 1, 1, 1,
         {"drafting userSearch.ts", "designing /api/v2/users"},
         "approve: Bash",
         Prompt{"req_004", "Bash", "cat > src/api/userSearch.ts << 'EOF'\nexport async..."}, false, ""},

        // Step 2: Creating tests
        {3000, 1, 0, 1,
         {"created userSearch.ts", "drafting userSearch.ts"},
         "1 session running", std::nullopt, true,
         R"([{"type":"text","text":"Created the search endpoint. Now writing tests..."}])"},

        // Step 3: Asks to run tests
        {4000, 1, 1, 1,
         {"writing userSearch.test.ts", "created userSearch.ts"},
         "approve: Bash",
         Prompt{"req_005", "Bash", "npm test -- userSearch.test.ts"}, false, ""},

        // Step 4: All green
        {3000, 0, 0, 1,
         {"12 tests passing", "writing userSearch.test.ts"},
         "1 session idle", std::nullopt, true,
         R"([{"type":"text","text":"Feature complete! /api/v2/users?search=query returns paginated results."}])"},
    }
};

// Scenario 4: Refactoring
// Claude renames variables, extracts functions
static const Scenario SCENARIO_REFACTORING = {
    "Refactoring",
    {
        // Step 0: Analyzing code
        {3000, 1, 0, 1,
         {"analyzing legacy.js", "user: clean up this file"},
         "1 session running", std::nullopt, false, ""},

        // Step 1: First rename operation
        {4000, 1, 1, 1,
         {"found 12 'data' variables", "analyzing legacy.js"},
         "approve: Bash",
         Prompt{"req_006", "Bash", "sed -i 's/\\bdata\\b/userData/g' src/legacy.js"}, false, ""},

        // Step 2: Extracting function
        {3000, 1, 0, 1,
         {"renamed variables", "found 12 'data' variables"},
         "1 session running", std::nullopt, true,
         R"([{"type":"text","text":"Renamed all ambiguous 'data' variables. Now extracting the validation logic..."}])"},

        // Step 3: Second permission for more changes
        {4000, 1, 1, 1,
         {"extracting validateUser()", "renamed variables"},
         "approve: Bash",
         Prompt{"req_007", "Bash", "sed -i 's/\\bdata\\b/userData/g' src/legacy.js"}, false, ""},

        // Step 4: Done
        {3000, 0, 0, 1,
         {"extracted 3 functions", "extracting validateUser()"},
         "1 session idle", std::nullopt, true,
         R"([{"type":"text","text":"Refactoring complete. Legacy.js reduced from 340 to 180 lines."}])"},
    }
};

// Scenario 5: Onboarding
// First-time user, Claude explores project structure
static const Scenario SCENARIO_ONBOARDING = {
    "Onboarding",
    {
        // Step 0: Exploring structure
        {3000, 1, 0, 1,
         {"ls -la project/", "first connection"},
         "1 session running", std::nullopt, false, ""},

        // Step 1: Reading package.json
        {3000, 1, 0, 1,
         {"reading package.json", "ls -la project/"},
         "1 session running", std::nullopt, true,
         R"([{"type":"text","text":"This is a React + TypeScript project with Vite. Main dependencies: react, react-router, zustand."}])"},

        // Step 2: Reading README
        {3000, 1, 0, 1,
         {"reading README.md", "reading package.json"},
         "1 session running", std::nullopt, true,
         R"([{"type":"text","text":"The README says this is a task management app. Let me check the component structure..."}])"},

        // Step 3: Summarizing to user
        {3000, 0, 0, 1,
         {"found 12 components", "reading README.md"},
         "1 session idle", std::nullopt, true,
         R"([{"type":"text","text":"Project overview: React app with components in src/components/, state in src/store/. Tests use vitest."}])"},
    }
};

// Scenario 6: Multi-session chaos
// Multiple tool calls running simultaneously
static const Scenario SCENARIO_MULTI_SESSION = {
    "Multi-Session",
    {
        // Step 0: Two sessions active
        {3000, 2, 0, 2,
         {"session A: reading docs", "session B: writing tests"},
         "2 tool calls running", std::nullopt, false, ""},

        // Step 1: Session B needs approval
        {4000, 2, 1, 2,
         {"session B: approve: Bash", "session A: reading docs"},
         "approve: Bash",
         Prompt{"req_008", "Bash", "git commit -am 'wip: auth refactor'"}, false, ""},

        // Step 2: Session C joins
        {3000, 3, 0, 3,
         {"session C: analyzing perf", "session B: commit done"},
         "3 tool calls running", std::nullopt, true,
         R"([{"type":"text","text":"New session: analyzing bundle size. Current: 240KB main, 80KB vendor."}])"},

        // Step 3: Session A now needs approval too
        {4000, 3, 1, 3,
         {"session A: approve: Edit", "session C: analyzing perf"},
         "approve: Edit",
         Prompt{"req_009", "Edit", "src/components/UserProfile.tsx"}, false, ""},

        // Step 4: Winding down
        {3000, 1, 0, 2,
         {"session C: done", "session A: editing UserProfile"},
         "1 session running", std::nullopt, true,
         R"([{"type":"text","text":"Sessions winding down. 2 idle, 1 running."}])"},
    }
};

const std::vector<Scenario> MockTransport::SCENARIOS = {
    SCENARIO_CODE_REVIEW,
    SCENARIO_BUG_HUNT,
    SCENARIO_FEATURE_BUILD,
    SCENARIO_REFACTORING,
    SCENARIO_ONBOARDING,
    SCENARIO_MULTI_SESSION,
};

// ------------------------------------------------------------------
// Implementation
// ------------------------------------------------------------------

MockTransport::MockTransport(std::function<void(DaemonMsg)> on_msg)
    : on_msg_(std::move(on_msg)) {
}

MockTransport::~MockTransport() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MockTransport::start() {
    connected_ = true;
    thread_ = std::thread([this] { loop(); });
}

void MockTransport::send(const BuddyMsg& msg) {
    handle_buddy_msg(msg);
}

bool MockTransport::is_connected() const {
    return connected_.load();
}

void MockTransport::loop() {
    LOG_INFO("[mock] loop started");
    // Initial handshake
    if (on_msg_) {
        LOG_INFO("[mock] sending initial handshake");
        on_msg_(TimeSync{1775731234, -25200});
        on_msg_(OwnerCmd{"owner", "Felix"});
    }

    uint32_t last_hb = SDL_GetTicks();

    // Load initial step (step 0)
    load_step(SCENARIOS[scenario_idx_].steps[step_idx_]);
    step_deadline_ = SDL_GetTicks() + SCENARIOS[scenario_idx_].steps[step_idx_].duration_ms;

    while (!stop_) {
        SDL_Delay(50);
        uint32_t now = SDL_GetTicks();

        // Heartbeat every 2s
        if (now - last_hb >= 2000) {
            emit_heartbeat();
            last_hb = now;
        }

        // Advance scenario step
        if (now >= step_deadline_) {
            advance_step();
            step_deadline_ = now + SCENARIOS[scenario_idx_].steps[step_idx_].duration_ms;
        }
    }
}

void MockTransport::load_step(const ScenarioStep& step) {
    running_ = step.running;
    waiting_ = step.waiting;
    total_ = step.total;
    entries_ = step.entries;
    prompt_ = step.prompt;

    if (step.emit_turn && !step.turn_content.empty()) {
        emit_turn(step.turn_content);
    }

    LOG_INFO("[mock] scenario '%s' step %zu/%zu: %s",
             SCENARIOS[scenario_idx_].name,
             step_idx_,
             SCENARIOS[scenario_idx_].steps.size(),
             step.msg.c_str());
}

void MockTransport::emit_heartbeat() {
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

    if (on_msg_) {
        on_msg_(Heartbeat{
            false,
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
}

void MockTransport::emit_turn(const std::string& content) {
    if (on_msg_) {
        on_msg_(TurnEvent{"turn", "assistant", content});
    }
}

void MockTransport::advance_step() {
    const auto& scenario = SCENARIOS[scenario_idx_];

    step_idx_++;
    if (step_idx_ >= scenario.steps.size()) {
        // Move to next scenario
        step_idx_ = 0;
        scenario_idx_ = (scenario_idx_ + 1) % SCENARIOS.size();
        LOG_INFO("[mock] switching to scenario: %s", SCENARIOS[scenario_idx_].name);
    }

    load_step(SCENARIOS[scenario_idx_].steps[step_idx_]);
}

void MockTransport::handle_buddy_msg(const BuddyMsg& msg) {
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, PermissionCmd>) {
            if (m.decision == "once") {
                LOG_INFO("[mock] approved %s", m.id.c_str());
            } else {
                LOG_INFO("[mock] denied %s", m.id.c_str());
            }
            prompt_ = std::nullopt;
            waiting_ = 0;
            emit_heartbeat();
        }
    }, msg);
}