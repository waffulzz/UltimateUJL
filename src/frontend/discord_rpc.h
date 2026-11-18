// Discord Rich Presence over the Discord client's local IPC pipe (no SDK). Optional, off-able in settings.
#pragma once
#include <string>

// Turns presence on or off. client_id is a Discord application ID (digits); an empty one keeps it off.
void discord_rpc_configure(bool enabled, const std::string &client_id);
// Reads the game's state and sends it when it changed. Call from the UI thread; cheap, rate-limited inside.
void discord_rpc_frame(bool paused);
// One line for the settings window: "Connected", "Discord isn't running", ...
std::string discord_rpc_status();
// Clears the activity and closes the pipe (exit).
void discord_rpc_shutdown();
// The text that would be shown now (details, state), for logs and tests.
void discord_rpc_describe(bool paused, std::string &details, std::string &state);
