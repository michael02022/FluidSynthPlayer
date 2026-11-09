// The CLAP entry point. Kept apart from the plugin code (static library
// fluidsynthplayer_impl) so every plugin format links the same code: the
// .clap exports it directly, the VST3 hands it to clap-wrapper.
#include <clap/clap.h>

bool fspEntryInit(const char* pluginPath);
void fspEntryDeinit();
const void* fspEntryGetFactory(const char* factoryId);

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = fspEntryInit,
    .deinit = fspEntryDeinit,
    .get_factory = fspEntryGetFactory,
};
