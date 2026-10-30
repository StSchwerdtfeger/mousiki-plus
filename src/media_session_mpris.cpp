// MPRIS (the Linux desktop's media-player protocol) for Mousiki: see media_session.h. libdbus is opened with dlopen() at run
// time, so building and running need nothing installed; no libdbus or no session bus: the feature stays off.
#if !defined(_WIN32) && !defined(__APPLE__)

#include "media_session.h"

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace muisc::media::detail {

namespace {

// ---- the few libdbus declarations needed (the ABI of libdbus-1.so.3 is stable) ----------------------------------------
struct DBusConnection;
struct DBusMessage;
struct DBusError { alignas(8) char pad[64]; };
struct DBusMessageIter { alignas(8) char pad[128]; };
using dbus_bool_t = unsigned int;
struct DBusObjectPathVTable {
    void (*unregister_function)(DBusConnection*, void*);
    unsigned int (*message_function)(DBusConnection*, DBusMessage*, void*);
    void (*pad1)(void*); void (*pad2)(void*); void (*pad3)(void*); void (*pad4)(void*);
};
constexpr int kTypeBool = 'b', kTypeInt64 = 'x', kTypeDouble = 'd', kTypeString = 's',
              kTypePath = 'o', kTypeArray = 'a', kTypeVariant = 'v', kTypeDictEntry = 'e';
constexpr unsigned kHandled = 0, kNotYetHandled = 1;
constexpr int kNameDoNotQueue = 4;

struct Api {
    void* lib = nullptr;
    dbus_bool_t (*threads_init_default)() = nullptr;
    void (*error_init)(DBusError*) = nullptr;
    void (*error_free)(DBusError*) = nullptr;
    dbus_bool_t (*error_is_set)(const DBusError*) = nullptr;
    DBusConnection* (*connection_open_private)(const char*, DBusError*) = nullptr;
    dbus_bool_t (*bus_register)(DBusConnection*, DBusError*) = nullptr;
    int (*bus_request_name)(DBusConnection*, const char*, unsigned, DBusError*) = nullptr;
    void (*connection_set_exit_on_disconnect)(DBusConnection*, dbus_bool_t) = nullptr;
    dbus_bool_t (*connection_register_object_path)(DBusConnection*, const char*, const DBusObjectPathVTable*, void*) = nullptr;
    dbus_bool_t (*connection_read_write_dispatch)(DBusConnection*, int) = nullptr;
    dbus_bool_t (*connection_send)(DBusConnection*, DBusMessage*, unsigned*) = nullptr;
    void (*connection_flush)(DBusConnection*) = nullptr;
    void (*connection_close)(DBusConnection*) = nullptr;
    void (*connection_unref)(DBusConnection*) = nullptr;
    DBusMessage* (*message_new_method_return)(DBusMessage*) = nullptr;
    DBusMessage* (*message_new_error)(DBusMessage*, const char*, const char*) = nullptr;
    DBusMessage* (*message_new_signal)(const char*, const char*, const char*) = nullptr;
    dbus_bool_t (*message_is_method_call)(DBusMessage*, const char*, const char*) = nullptr;
    const char* (*message_get_interface)(DBusMessage*) = nullptr;
    const char* (*message_get_member)(DBusMessage*) = nullptr;
    void (*message_unref)(DBusMessage*) = nullptr;
    dbus_bool_t (*iter_init)(DBusMessage*, DBusMessageIter*) = nullptr;
    void (*iter_init_append)(DBusMessage*, DBusMessageIter*) = nullptr;
    int (*iter_get_arg_type)(DBusMessageIter*) = nullptr;
    void (*iter_get_basic)(DBusMessageIter*, void*) = nullptr;
    dbus_bool_t (*iter_next)(DBusMessageIter*) = nullptr;
    void (*iter_recurse)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    dbus_bool_t (*iter_append_basic)(DBusMessageIter*, int, const void*) = nullptr;
    dbus_bool_t (*iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*) = nullptr;
    dbus_bool_t (*iter_close_container)(DBusMessageIter*, DBusMessageIter*) = nullptr;
};
Api g_api;

template <class T>
bool load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(dlsym(g_api.lib, name));
    return fn != nullptr;
}

bool load_api() {
    if (g_api.lib) return true;
    g_api.lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!g_api.lib) g_api.lib = dlopen("libdbus-1.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_api.lib) return false;
    Api& a = g_api;
    const bool ok = load(a.threads_init_default, "dbus_threads_init_default") && load(a.error_init, "dbus_error_init") &&
        load(a.error_free, "dbus_error_free") && load(a.error_is_set, "dbus_error_is_set") &&
        load(a.connection_open_private, "dbus_connection_open_private") && load(a.bus_register, "dbus_bus_register") &&
        load(a.bus_request_name, "dbus_bus_request_name") &&
        load(a.connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect") &&
        load(a.connection_register_object_path, "dbus_connection_register_object_path") &&
        load(a.connection_read_write_dispatch, "dbus_connection_read_write_dispatch") &&
        load(a.connection_send, "dbus_connection_send") && load(a.connection_flush, "dbus_connection_flush") &&
        load(a.connection_close, "dbus_connection_close") && load(a.connection_unref, "dbus_connection_unref") &&
        load(a.message_new_method_return, "dbus_message_new_method_return") && load(a.message_new_error, "dbus_message_new_error") &&
        load(a.message_new_signal, "dbus_message_new_signal") && load(a.message_is_method_call, "dbus_message_is_method_call") &&
        load(a.message_get_interface, "dbus_message_get_interface") && load(a.message_get_member, "dbus_message_get_member") &&
        load(a.message_unref, "dbus_message_unref") && load(a.iter_init, "dbus_message_iter_init") &&
        load(a.iter_init_append, "dbus_message_iter_init_append") && load(a.iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
        load(a.iter_get_basic, "dbus_message_iter_get_basic") && load(a.iter_next, "dbus_message_iter_next") &&
        load(a.iter_recurse, "dbus_message_iter_recurse") && load(a.iter_append_basic, "dbus_message_iter_append_basic") &&
        load(a.iter_open_container, "dbus_message_iter_open_container") && load(a.iter_close_container, "dbus_message_iter_close_container");
    if (!ok) { dlclose(g_api.lib); g_api = Api{}; return false; }
    return true;
}

// ---- the state shared between the program's thread and the D-Bus thread --------------------------------------------------
constexpr const char* kPath = "/org/mpris/MediaPlayer2";
constexpr const char* kIfaceRoot = "org.mpris.MediaPlayer2";
constexpr const char* kIfacePlayer = "org.mpris.MediaPlayer2.Player";
constexpr const char* kIfaceProps = "org.freedesktop.DBus.Properties";
constexpr const char* kIfaceIntro = "org.freedesktop.DBus.Introspectable";

std::mutex g_mu;
Info g_info;                                   // the newest status
std::chrono::steady_clock::time_point g_info_at;   // when it arrived (the position is extrapolated from there)
unsigned g_track_serial = 1;                   // changes with the title / artist / album
std::vector<remote::Command> g_cmds;
std::atomic<bool> g_stop{false};
// A std::thread that is still joinable when the program ends would call std::terminate(): this one is stopped and joined by
// the destructor, so a program (or a test) that never called shutdown() still ends cleanly.
struct ThreadHolder {
    std::thread t;
    ~ThreadHolder() { g_stop = true; if (t.joinable()) t.join(); }
};
ThreadHolder g_th;
DBusConnection* g_conn = nullptr;

double position_now_locked() {
    double p = g_info.position;
    if (g_info.state == "playing")
        p += std::chrono::duration<double>(std::chrono::steady_clock::now() - g_info_at).count();
    if (g_info.duration > 0.0) p = std::min(p, g_info.duration);
    return std::max(0.0, p);
}

// ---- writing values -------------------------------------------------------------------------------------------------------
void put_string(DBusMessageIter* it, int type, const std::string& s) { const char* c = s.c_str(); g_api.iter_append_basic(it, type, &c); }

void open_variant(DBusMessageIter* parent, const char* sig, DBusMessageIter* v) { g_api.iter_open_container(parent, kTypeVariant, sig, v); }
void var_bool(DBusMessageIter* p, bool b) { DBusMessageIter v; open_variant(p, "b", &v); dbus_bool_t x = b; g_api.iter_append_basic(&v, kTypeBool, &x); g_api.iter_close_container(p, &v); }
void var_string(DBusMessageIter* p, const std::string& s) { DBusMessageIter v; open_variant(p, "s", &v); put_string(&v, kTypeString, s); g_api.iter_close_container(p, &v); }
void var_double(DBusMessageIter* p, double d) { DBusMessageIter v; open_variant(p, "d", &v); g_api.iter_append_basic(&v, kTypeDouble, &d); g_api.iter_close_container(p, &v); }
void var_int64(DBusMessageIter* p, long long x) { DBusMessageIter v; open_variant(p, "x", &v); const int64_t y = x; g_api.iter_append_basic(&v, kTypeInt64, &y); g_api.iter_close_container(p, &v); }
void var_empty_strings(DBusMessageIter* p) { DBusMessageIter v, a; open_variant(p, "as", &v); g_api.iter_open_container(&v, kTypeArray, "s", &a); g_api.iter_close_container(&v, &a); g_api.iter_close_container(p, &v); }

// one {sv} entry of a dictionary: the key, then the variant `fn` writes
template <class F>
void dict_entry(DBusMessageIter* dict, const char* key, F&& fn) {
    DBusMessageIter e;
    g_api.iter_open_container(dict, kTypeDictEntry, nullptr, &e);
    put_string(&e, kTypeString, key);
    fn(&e);
    g_api.iter_close_container(dict, &e);
}

void var_metadata(DBusMessageIter* p, const Info& in, unsigned serial) {
    DBusMessageIter v, d;
    open_variant(p, "a{sv}", &v);
    g_api.iter_open_container(&v, kTypeArray, "{sv}", &d);
    if (in.state == "stopped") {
        dict_entry(&d, "mpris:trackid", [](DBusMessageIter* e) { DBusMessageIter x; open_variant(e, "o", &x); put_string(&x, kTypePath, "/org/mpris/MediaPlayer2/TrackList/NoTrack"); g_api.iter_close_container(e, &x); });
    } else {
        dict_entry(&d, "mpris:trackid", [&](DBusMessageIter* e) { DBusMessageIter x; open_variant(e, "o", &x); put_string(&x, kTypePath, "/org/mousiki/track/" + std::to_string(serial)); g_api.iter_close_container(e, &x); });
        if (in.duration > 0.0) dict_entry(&d, "mpris:length", [&](DBusMessageIter* e) { var_int64(e, std::llround(in.duration * 1e6)); });
        if (!in.title.empty()) dict_entry(&d, "xesam:title", [&](DBusMessageIter* e) { var_string(e, in.title); });
        if (!in.artist.empty()) dict_entry(&d, "xesam:artist", [&](DBusMessageIter* e) {
            DBusMessageIter x, a; open_variant(e, "as", &x); g_api.iter_open_container(&x, kTypeArray, "s", &a);
            put_string(&a, kTypeString, in.artist); g_api.iter_close_container(&x, &a); g_api.iter_close_container(e, &x); });
        if (!in.album.empty()) dict_entry(&d, "xesam:album", [&](DBusMessageIter* e) { var_string(e, in.album); });
    }
    g_api.iter_close_container(&v, &d);
    g_api.iter_close_container(p, &v);
}

const char* status_name(const Info& in) { return in.state == "playing" ? "Playing" : in.state == "paused" ? "Paused" : "Stopped"; }

const char* const kRootProps[] = {"CanQuit", "CanRaise", "HasTrackList", "Identity", "SupportedUriSchemes", "SupportedMimeTypes"};
const char* const kPlayerProps[] = {"PlaybackStatus", "LoopStatus", "Rate", "Shuffle", "Metadata", "Volume", "Position", "MinimumRate",
                                    "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", "CanControl"};

// Writes the variant of one property into `parent` (a dict entry or a Get reply); false when there is no such property.
bool put_property(DBusMessageIter* parent, const std::string& iface, const std::string& name) {
    Info in; unsigned serial; double pos;
    { std::lock_guard<std::mutex> lk(g_mu); in = g_info; serial = g_track_serial; pos = position_now_locked(); }
    if (iface == kIfaceRoot) {
        if (name == "CanQuit" || name == "CanRaise" || name == "HasTrackList") var_bool(parent, false);
        else if (name == "Identity") var_string(parent, "Mousiki Plus");
        else if (name == "SupportedUriSchemes" || name == "SupportedMimeTypes") var_empty_strings(parent);
        else return false;
        return true;
    }
    if (iface != kIfacePlayer) return false;
    if (name == "PlaybackStatus") var_string(parent, status_name(in));
    else if (name == "LoopStatus") var_string(parent, "None");
    else if (name == "Rate" || name == "MinimumRate" || name == "MaximumRate") var_double(parent, 1.0);
    else if (name == "Shuffle") var_bool(parent, false);
    else if (name == "Metadata") var_metadata(parent, in, serial);
    else if (name == "Volume") var_double(parent, in.volume < 0 ? 1.0 : in.volume / 100.0);
    else if (name == "Position") var_int64(parent, std::llround(pos * 1e6));
    else if (name == "CanGoNext" || name == "CanGoPrevious" || name == "CanPlay" || name == "CanPause" || name == "CanControl") var_bool(parent, true);
    else if (name == "CanSeek") var_bool(parent, in.duration > 0.0);
    else return false;
    return true;
}

void send_and_free(DBusMessage* m) { if (!m) return; g_api.connection_send(g_conn, m, nullptr); g_api.message_unref(m); }
void reply_error(DBusMessage* msg, const char* name, const char* text) { send_and_free(g_api.message_new_error(msg, name, text)); }

const char* kIntrospection =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    " <interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg direction=\"out\" type=\"s\"/></method></interface>\n"
    " <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "  <method name=\"Get\"><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"out\" type=\"v\"/></method>\n"
    "  <method name=\"GetAll\"><arg direction=\"in\" type=\"s\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
    "  <method name=\"Set\"><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"v\"/></method>\n"
    "  <signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>\n"
    " </interface>\n"
    " <interface name=\"org.mpris.MediaPlayer2\">\n"
    "  <method name=\"Raise\"/><method name=\"Quit\"/>\n"
    "  <property name=\"CanQuit\" type=\"b\" access=\"read\"/><property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
    "  <property name=\"HasTrackList\" type=\"b\" access=\"read\"/><property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/><property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
    " </interface>\n"
    " <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
    "  <method name=\"Next\"/><method name=\"Previous\"/><method name=\"Pause\"/><method name=\"PlayPause\"/><method name=\"Stop\"/><method name=\"Play\"/>\n"
    "  <method name=\"Seek\"><arg direction=\"in\" type=\"x\"/></method>\n"
    "  <method name=\"SetPosition\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"x\"/></method>\n"
    "  <method name=\"OpenUri\"><arg direction=\"in\" type=\"s\"/></method>\n"
    "  <signal name=\"Seeked\"><arg type=\"x\"/></signal>\n"
    "  <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/><property name=\"LoopStatus\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Rate\" type=\"d\" access=\"read\"/><property name=\"Shuffle\" type=\"b\" access=\"read\"/>\n"
    "  <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/><property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
    "  <property name=\"Position\" type=\"x\" access=\"read\"/><property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
    "  <property name=\"MaximumRate\" type=\"d\" access=\"read\"/><property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
    "  <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/><property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
    "  <property name=\"CanPause\" type=\"b\" access=\"read\"/><property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
    "  <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
    " </interface>\n"
    "</node>\n";

bool next_string(DBusMessageIter* it, std::string& out) {
    if (g_api.iter_get_arg_type(it) != kTypeString && g_api.iter_get_arg_type(it) != kTypePath) return false;
    const char* c = nullptr;
    g_api.iter_get_basic(it, &c);
    out = c ? c : "";
    g_api.iter_next(it);
    return true;
}

void queue_command(const std::string& method, long long value) {
    Info now;
    { std::lock_guard<std::mutex> lk(g_mu); now = g_info; now.position = position_now_locked(); }
    const remote::Command c = command_for(method, value, now);
    if (c.name.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cmds.size() < 16) g_cmds.push_back(c);
}

unsigned on_message(DBusConnection*, DBusMessage* msg, void*) {
    const char* ci = g_api.message_get_interface(msg);
    const char* cm = g_api.message_get_member(msg);
    if (!ci || !cm) return kNotYetHandled;
    const std::string iface = ci, member = cm;
    DBusMessageIter it;

    if (iface == kIfaceIntro && member == "Introspect") {
        DBusMessage* r = g_api.message_new_method_return(msg);
        DBusMessageIter ri; g_api.iter_init_append(r, &ri);
        const char* x = kIntrospection; g_api.iter_append_basic(&ri, kTypeString, &x);
        send_and_free(r);
        return kHandled;
    }
    if (iface == kIfaceProps) {
        bool have = g_api.iter_init(msg, &it);
        std::string a, b;
        if (member == "Get") {
            if (!have || !next_string(&it, a) || !next_string(&it, b)) { reply_error(msg, "org.freedesktop.DBus.Error.InvalidArgs", "Get(ss)"); return kHandled; }
            DBusMessage* r = g_api.message_new_method_return(msg);
            DBusMessageIter ri; g_api.iter_init_append(r, &ri);
            if (!put_property(&ri, a, b)) { g_api.message_unref(r); reply_error(msg, "org.freedesktop.DBus.Error.UnknownProperty", "no such property"); return kHandled; }
            send_and_free(r);
            return kHandled;
        }
        if (member == "GetAll") {
            if (!have || !next_string(&it, a)) { reply_error(msg, "org.freedesktop.DBus.Error.InvalidArgs", "GetAll(s)"); return kHandled; }
            DBusMessage* r = g_api.message_new_method_return(msg);
            DBusMessageIter ri, d; g_api.iter_init_append(r, &ri);
            g_api.iter_open_container(&ri, kTypeArray, "{sv}", &d);
            if (a == kIfaceRoot) for (const char* n : kRootProps) dict_entry(&d, n, [&](DBusMessageIter* e) { put_property(e, a, n); });
            else if (a == kIfacePlayer) for (const char* n : kPlayerProps) dict_entry(&d, n, [&](DBusMessageIter* e) { put_property(e, a, n); });
            g_api.iter_close_container(&ri, &d);
            send_and_free(r);
            return kHandled;
        }
        if (member == "Set") {
            if (!have || !next_string(&it, a) || !next_string(&it, b) || g_api.iter_get_arg_type(&it) != kTypeVariant) {
                reply_error(msg, "org.freedesktop.DBus.Error.InvalidArgs", "Set(ssv)"); return kHandled;
            }
            if (a == kIfacePlayer && b == "Volume") {
                DBusMessageIter v; g_api.iter_recurse(&it, &v);
                if (g_api.iter_get_arg_type(&v) == kTypeDouble) {
                    double d = 0; g_api.iter_get_basic(&v, &d);
                    queue_command("SetVolume", std::llround(std::min(1.0, std::max(0.0, d)) * 1e6));
                    send_and_free(g_api.message_new_method_return(msg));
                    return kHandled;
                }
            }
            reply_error(msg, "org.freedesktop.DBus.Error.PropertyReadOnly", "this property cannot be set");
            return kHandled;
        }
        return kNotYetHandled;
    }
    if (iface == kIfaceRoot) {
        if (member == "Raise" || member == "Quit") { send_and_free(g_api.message_new_method_return(msg)); return kHandled; }   // (CanRaise / CanQuit are false)
        return kNotYetHandled;
    }
    if (iface == kIfacePlayer) {
        if (member == "Play" || member == "Pause" || member == "PlayPause" || member == "Stop" || member == "Next" || member == "Previous") {
            queue_command(member, 0);
            send_and_free(g_api.message_new_method_return(msg));
            return kHandled;
        }
        if (member == "Seek") {
            long long off = 0;
            if (g_api.iter_init(msg, &it) && g_api.iter_get_arg_type(&it) == kTypeInt64) { int64_t x = 0; g_api.iter_get_basic(&it, &x); off = x; }
            queue_command("Seek", off);
            send_and_free(g_api.message_new_method_return(msg));
            return kHandled;
        }
        if (member == "SetPosition") {
            std::string track;
            if (g_api.iter_init(msg, &it) && next_string(&it, track) && g_api.iter_get_arg_type(&it) == kTypeInt64) {
                int64_t x = 0; g_api.iter_get_basic(&it, &x);
                queue_command("SetPosition", x);
            }
            send_and_free(g_api.message_new_method_return(msg));
            return kHandled;
        }
        if (member == "OpenUri") { send_and_free(g_api.message_new_method_return(msg)); return kHandled; }   // not supported: ignored
        return kNotYetHandled;
    }
    return kNotYetHandled;
}

// ---- the signals --------------------------------------------------------------------------------------------------------------
struct Sent { std::string status; unsigned serial = 0; int volume = -2; bool can_seek = false; double expected = 0.0; bool valid = false; };

void send_changes(Sent& sent) {
    Info in; unsigned serial; double pos;
    { std::lock_guard<std::mutex> lk(g_mu); in = g_info; serial = g_track_serial; pos = position_now_locked(); }
    const std::string status = status_name(in);
    const bool can_seek = in.duration > 0.0;
    const bool st = !sent.valid || status != sent.status;
    const bool md = !sent.valid || serial != sent.serial || (in.state == "stopped") != (sent.status == "Stopped");
    const bool vol = !sent.valid || in.volume != sent.volume;
    const bool cs = !sent.valid || can_seek != sent.can_seek;
    if (sent.valid && !st && !md && !vol && !cs && sent.status == "Playing" && std::fabs(pos - sent.expected) > 1.5) {
        DBusMessage* m = g_api.message_new_signal(kPath, kIfacePlayer, "Seeked");   // the position jumped: a seek
        if (m) { DBusMessageIter i; g_api.iter_init_append(m, &i); const int64_t x = std::llround(pos * 1e6); g_api.iter_append_basic(&i, kTypeInt64, &x); send_and_free(m); }
    }
    sent.expected = pos;
    if (sent.valid && (st || md || vol || cs)) {
        DBusMessage* m = g_api.message_new_signal(kPath, kIfaceProps, "PropertiesChanged");
        if (m) {
            DBusMessageIter i, d, none;
            g_api.iter_init_append(m, &i);
            put_string(&i, kTypeString, kIfacePlayer);
            g_api.iter_open_container(&i, kTypeArray, "{sv}", &d);
            if (st) dict_entry(&d, "PlaybackStatus", [&](DBusMessageIter* e) { put_property(e, kIfacePlayer, "PlaybackStatus"); });
            if (md) dict_entry(&d, "Metadata", [&](DBusMessageIter* e) { put_property(e, kIfacePlayer, "Metadata"); });
            if (vol) dict_entry(&d, "Volume", [&](DBusMessageIter* e) { put_property(e, kIfacePlayer, "Volume"); });
            if (cs) dict_entry(&d, "CanSeek", [&](DBusMessageIter* e) { put_property(e, kIfacePlayer, "CanSeek"); });
            g_api.iter_close_container(&i, &d);
            g_api.iter_open_container(&i, kTypeArray, "s", &none);
            g_api.iter_close_container(&i, &none);
            send_and_free(m);
        }
    }
    sent.status = status; sent.serial = serial; sent.volume = in.volume; sent.can_seek = can_seek; sent.valid = true;
}

void loop() {
    Sent sent;
    while (!g_stop.load()) {
        if (!g_api.connection_read_write_dispatch(g_conn, 100)) break;   // false: the bus went away
        send_changes(sent);
        g_api.connection_flush(g_conn);
    }
}

std::string session_bus_address() {
    if (const char* a = std::getenv("DBUS_SESSION_BUS_ADDRESS")) if (*a) return a;
    const std::string p = "/run/user/" + std::to_string(static_cast<long>(getuid())) + "/bus";   // the systemd default
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return "unix:path=" + p;
    return "";   // no session bus: nothing is started (libdbus would try to launch one)
}

} // namespace

bool backend_start() {
    const std::string addr = session_bus_address();
    if (addr.empty() || !load_api()) return false;
    g_api.threads_init_default();
    DBusError err; g_api.error_init(&err);
    DBusConnection* c = g_api.connection_open_private(addr.c_str(), &err);
    if (!c || g_api.error_is_set(&err)) { g_api.error_free(&err); return false; }
    g_api.connection_set_exit_on_disconnect(c, 0);
    if (!g_api.bus_register(c, &err)) { g_api.error_free(&err); g_api.connection_close(c); g_api.connection_unref(c); return false; }
    int r = g_api.bus_request_name(c, "org.mpris.MediaPlayer2.mousiki", kNameDoNotQueue, &err);
    if (g_api.error_is_set(&err)) { g_api.error_free(&err); g_api.error_init(&err); r = 0; }
    if (r != 1 && r != 4) {   // not the primary owner (a second Mousiki): its own name
        r = g_api.bus_request_name(c, ("org.mpris.MediaPlayer2.mousiki.instance" + std::to_string(static_cast<long>(getpid()))).c_str(), kNameDoNotQueue, &err);
        if (g_api.error_is_set(&err) || (r != 1 && r != 4)) { g_api.error_free(&err); g_api.connection_close(c); g_api.connection_unref(c); return false; }
    }
    g_api.error_free(&err);
    static const DBusObjectPathVTable vt = {nullptr, on_message, nullptr, nullptr, nullptr, nullptr};
    if (!g_api.connection_register_object_path(c, kPath, &vt, nullptr)) { g_api.connection_close(c); g_api.connection_unref(c); return false; }
    g_conn = c;
    g_stop = false;
    g_th.t = std::thread(loop);
    return true;
}

void backend_stop() {
    if (!g_conn) return;
    g_stop = true;
    if (g_th.t.joinable()) g_th.t.join();
    g_api.connection_close(g_conn);
    g_api.connection_unref(g_conn);
    g_conn = nullptr;
}

void backend_update(const Info& info) {
    std::lock_guard<std::mutex> lk(g_mu);
    const bool new_track = info.title != g_info.title || info.artist != g_info.artist || info.album != g_info.album;
    if (new_track) ++g_track_serial;
    g_info = info;
    g_info_at = std::chrono::steady_clock::now();
}

std::vector<remote::Command> backend_poll(const Info&) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<remote::Command> out;
    out.swap(g_cmds);
    return out;
}

} // namespace muisc::media::detail

#endif
