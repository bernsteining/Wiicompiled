#include "discord_presence.h"

// A browser page cannot reach Discord's local IPC socket, so Rich Presence is a no-op on the web
// build. CMake compiles this file instead of src/discord_presence.cpp there.
namespace DiscordPresence {

void Initialize(const std::string&, const std::string&) {}
void SetClient(const std::string&) {}
void SetActivity(Activity) {}
void Reset() {}
void Shutdown() {}

} // namespace DiscordPresence
