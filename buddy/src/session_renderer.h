#pragma once

struct AppState;
class Renderer;

// Render AppState to a Renderer. The only place that reads AppState and issues
// drawing commands.
void render_session(AppState* app, Renderer* r);
