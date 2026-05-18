#pragma once
#include <cstdint>

struct AppState;

namespace persistence {

// Path to state file: ~/.config/openbuddy/state.json
const char* state_path();

// Save stats + settings + names. Returns true on success.
bool save(const AppState& app);

// Load stats + settings + names. Returns true if file existed and loaded.
// If false, leaves everything at defaults (fresh start).
bool load(AppState* app);

} // namespace persistence
