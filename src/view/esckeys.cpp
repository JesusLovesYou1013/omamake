#include "esckeys.h"

#include <QGuiApplication>
#include <QStyleHints>
#include <QJsonDocument>
#include <QJsonObject>
#include <dlfcn.h>

namespace {

EscKeys *theOne = nullptr;

// Looked up in the libraries Qt already loaded (as DialogLook does): only if they're there.
template <typename F>
F symbol(const char *library, const char *name)
{
    void *lib = dlopen(library, RTLD_LAZY | RTLD_NOLOAD);
    if (!lib)
        return nullptr;
    void *found = dlsym(lib, name);
    dlclose(lib);
    return reinterpret_cast<F>(found);
}

using GType = unsigned long;
struct Gtk {
    GType (*dialogType)() = nullptr;
    GType (*editableType)() = nullptr;
    GType (*searchEntryType)() = nullptr;
    int (*isA)(void *, GType) = nullptr;              // g_type_check_instance_is_a
    void *(*toplevel)(void *) = nullptr;               // gtk_widget_get_toplevel
    void *(*focus)(void *) = nullptr;                  // gtk_window_get_focus
    int (*bounds)(void *, int *, int *) = nullptr;     // gtk_editable_get_selection_bounds
    void (*select)(void *, int, int) = nullptr;        // gtk_editable_select_region
    void (*response)(void *, int) = nullptr;           // gtk_dialog_response
    int (*eventType)(const void *) = nullptr;          // gdk_event_get_event_type
    int (*keyval)(const void *, unsigned *) = nullptr; // gdk_event_get_keyval
    bool ok() const
    {
        return dialogType && editableType && searchEntryType && isA && toplevel && focus && bounds && select && response
            && eventType && keyval;
    }
} gtk;

constexpr int kGdkKeyPress = 8;            // GDK_KEY_PRESS
constexpr unsigned kGdkEscape = 0xff1b;    // GDK_KEY_Escape
constexpr int kGtkResponseCancel = -6;     // GTK_RESPONSE_CANCEL

// gtk_key_snooper_install's watcher: every key GTK gets, before its widgets. 1 = taken here.
int snoop(void *grabWidget, void *event, void *)
{
    unsigned key = 0;
    if (!theOne || !grabWidget || gtk.eventType(event) != kGdkKeyPress || !gtk.keyval(event, &key) || key != kGdkEscape)
        return 0;
    void *top = gtk.toplevel(grabWidget);
    if (!top || !gtk.isA(top, gtk.dialogType()))
        return 0;                                      // a completion list or menu over it: its own Esc
    if (theOne->press()) {
        gtk.response(top, kGtkResponseCancel);         // two quick Esc: the dialog is cancelled
        return 1;
    }
    void *f = gtk.focus(top);
    if (f && gtk.isA(f, gtk.searchEntryType()))
        return 0;                                      // the search box: its Esc stops searching (never shuts the dialog)
    if (f && gtk.isA(f, gtk.editableType())) {
        int from = 0, to = 0;
        if (gtk.bounds(f, &from, &to))
            gtk.select(f, to, to);                     // the highlight let go, the name kept as typed
    }
    return 1;                                          // one Esc never shuts the dialog
}

}

EscKeys::EscKeys(QObject *parent) : QObject(parent)
{
    clock.start();
    theOne = this;
}

EscKeys *EscKeys::instance() { return theOne; }

int EscKeys::interval()
{
    const int base = QGuiApplication::styleHints() ? QGuiApplication::styleHints()->mouseDoubleClickInterval() : 400;
    return qRound((base > 0 ? base : 400) * 1.2);
}

bool EscKeys::press()
{
    const qint64 now = clock.elapsed();
    const bool second = last >= 0 && now - last <= interval();
    last = second ? -1 : now;
    return second;
}

void EscKeys::reset() { last = -1; }

void EscKeys::watchGtkDialogs()
{
    static bool done = false;
    if (done)
        return;
    const char *g = "libgtk-3.so.0", *o = "libgobject-2.0.so.0", *d = "libgdk-3.so.0";
    gtk.dialogType = symbol<GType (*)()>(g, "gtk_dialog_get_type");
    gtk.editableType = symbol<GType (*)()>(g, "gtk_editable_get_type");
    gtk.searchEntryType = symbol<GType (*)()>(g, "gtk_search_entry_get_type");
    gtk.isA = symbol<int (*)(void *, GType)>(o, "g_type_check_instance_is_a");
    gtk.toplevel = symbol<void *(*)(void *)>(g, "gtk_widget_get_toplevel");
    gtk.focus = symbol<void *(*)(void *)>(g, "gtk_window_get_focus");
    gtk.bounds = symbol<int (*)(void *, int *, int *)>(g, "gtk_editable_get_selection_bounds");
    gtk.select = symbol<void (*)(void *, int, int)>(g, "gtk_editable_select_region");
    gtk.response = symbol<void (*)(void *, int)>(g, "gtk_dialog_response");
    gtk.eventType = symbol<int (*)(const void *)>(d, "gdk_event_get_event_type");
    gtk.keyval = symbol<int (*)(const void *, unsigned *)>(d, "gdk_event_get_keyval");
    const auto install = symbol<unsigned (*)(int (*)(void *, void *, void *), void *)>(g, "gtk_key_snooper_install");
    if (!gtk.ok() || !install)
        return;                                        // GTK isn't loaded (yet): tried again next dialog
    install(snoop, nullptr);
    done = true;
}

namespace {

// GTK 3's GdkEventKey (its layout is part of GTK 3's ABI).
struct GdkEventKeyRaw {
    int type;
    void *window;
    signed char sendEvent;
    unsigned time;
    unsigned state;
    unsigned keyval;
    int length;
    char *string;
    unsigned short hardwareKeycode;
    unsigned char group;
    unsigned isModifier : 1;
};

void *testDialog = nullptr;
void *testEntry = nullptr;
int testResponse = 0;
void onTestResponse(void *, int response, void *) { testResponse = response; }

}

QString EscKeys::testGtkDialog(const QString &what)
{
    if (!qEnvironmentVariableIsSet("OMAMAKE_TEST_INPUT"))
        return {};
    watchGtkDialogs();
    const char *g = "libgtk-3.so.0", *o = "libgobject-2.0.so.0", *d = "libgdk-3.so.0";
    QJsonObject out;
    if (what == QLatin1String("make")) {
        const auto dialogNew = symbol<void *(*)()>(g, "gtk_dialog_new");
        const auto content = symbol<void *(*)(void *)>(g, "gtk_dialog_get_content_area");
        const auto entryNew = symbol<void *(*)()>(g, "gtk_entry_new");
        const auto add = symbol<void (*)(void *, void *)>(g, "gtk_container_add");
        const auto setText = symbol<void (*)(void *, const char *)>(g, "gtk_entry_set_text");
        const auto setFocus = symbol<void (*)(void *, void *)>(g, "gtk_window_set_focus");
        const auto realize = symbol<void (*)(void *)>(g, "gtk_widget_realize");
        const auto connect = symbol<unsigned long (*)(void *, const char *, void *, void *, void *, int)>(o, "g_signal_connect_data");
        if (!dialogNew || !content || !entryNew || !add || !setText || !setFocus || !realize || !connect || !gtk.ok()) {
            out.insert(QStringLiteral("made"), false);
            return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
        }
        testDialog = dialogNew();
        testEntry = entryNew();
        add(content(testDialog), testEntry);
        setText(testEntry, "Part.step");
        realize(testDialog);
        realize(testEntry);
        setFocus(testDialog, testEntry);
        gtk.select(testEntry, 0, 4);
        testResponse = 0;
        connect(testDialog, "response", reinterpret_cast<void *>(&onTestResponse), nullptr, nullptr, 0);
        reset();
    } else if (what == QLatin1String("esc") && testDialog) {
        const auto eventNew = symbol<void *(*)(int)>(d, "gdk_event_new");
        const auto eventFree = symbol<void (*)(void *)>(d, "gdk_event_free");
        const auto windowOf = symbol<void *(*)(void *)>(g, "gtk_widget_get_window");
        const auto ref = symbol<void *(*)(void *)>(o, "g_object_ref");
        const auto doEvent = symbol<void (*)(void *)>(g, "gtk_main_do_event");
        if (eventNew && eventFree && windowOf && ref && doEvent && windowOf(testDialog)) {
            auto *ev = static_cast<GdkEventKeyRaw *>(eventNew(kGdkKeyPress));
            ev->window = ref(windowOf(testDialog));
            ev->keyval = kGdkEscape;
            ev->time = unsigned(clock.elapsed());
            doEvent(ev);
            eventFree(ev);
        }
    } else if (what == QLatin1String("free") && testDialog) {
        const auto destroy = symbol<void (*)(void *)>(g, "gtk_widget_destroy");
        if (destroy)
            destroy(testDialog);
        testDialog = testEntry = nullptr;
    }
    out.insert(QStringLiteral("made"), testDialog != nullptr);
    if (testEntry) {
        const auto getText = symbol<const char *(*)(void *)>(g, "gtk_entry_get_text");
        int from = 0, to = 0;
        out.insert(QStringLiteral("text"), QString::fromUtf8(getText ? getText(testEntry) : ""));
        out.insert(QStringLiteral("selected"), bool(gtk.bounds(testEntry, &from, &to)));
    }
    out.insert(QStringLiteral("response"), testResponse);
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}
