#include "dialoglook.h"

#include <dlfcn.h>

namespace {

// Looked up in the libraries Qt already loaded (privately, so RTLD_DEFAULT finds nothing;
// RTLD_NOLOAD: only if they're loaded).
template <typename F>
F symbol(const char *library, const char *name)
{
    void *lib = dlopen(library, RTLD_LAZY | RTLD_NOLOAD);
    if (!lib)
        return nullptr;
    void *found = dlsym(lib, name);
    dlclose(lib);                       // drops only dlopen's extra count; Qt keeps it loaded
    return reinterpret_cast<F>(found);
}

}

void DialogLook::setDark(bool dark)
{
    const auto settingsDefault = symbol<void *(*)()>("libgtk-3.so.0", "gtk_settings_get_default");
    const auto objectSet = symbol<void (*)(void *, const char *, ...)>("libgobject-2.0.so.0", "g_object_set");
    void *settings = settingsDefault ? settingsDefault() : nullptr;
    if (!settings || !objectSet)
        return;
    // Adwaita is GTK's own (always there); its dark variant is what Omarchy's dark themes use.
    objectSet(settings, "gtk-theme-name", "Adwaita", "gtk-application-prefer-dark-theme", int(dark), nullptr);
}
