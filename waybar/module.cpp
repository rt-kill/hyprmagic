// waybar CFFI module for hyprmagic: renders the floater indicator in-process in
// waybar (no external follower binary). Event-driven from Hyprland's
// .socket2.sock; state comes from the hyprmagic compositor plugin's
// `magic:waybar-state <monitor>` hyprctl command over .socket.sock, so all
// floater semantics live in exactly one place. ABI v1: string config values
// arrive bare (no JSON decoding needed for the icon glyphs).
//
// The GTK/socket/output-detection plumbing is intentionally identical to
// hyprwsg_plugin/waybar/module.cpp (sibling projects); only the marked
// identity constants, config parsing, and the render call differ.

#include "render.hpp"
#include "waybar_cffi_module.h"
#include "xdg-output-client-protocol.h"

#include <gdk/gdkwayland.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <wayland-client.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

    // ── per-module identity (the only semantic differences from the wsg module) ──
    constexpr const char* WIDGET_NAME   = "custom-hyprmagic"; // keeps style.css #custom-hyprmagic working
    constexpr const char* STATE_COMMAND = "magic:waybar-state";
    constexpr const char* LOG_TAG       = "hyprmagic waybar";

    // waybar.rs:27-34
    bool isRelevantEvent(const std::string& ev) {
        return ev.starts_with("openwindow>>") || ev.starts_with("closewindow>>") || ev.starts_with("movewindow>>") || ev.starts_with("activespecial>>") ||
            ev.starts_with("createworkspace>>") || ev.starts_with("destroyworkspace>>");
    }

    // ── constants ────────────────────────────────────────────────────────
    constexpr guint  DEBOUNCE_MS     = 20;
    constexpr guint  RECONNECT_S     = 2;
    constexpr int    CONNECT_MS      = 500;        // bound on a blocking connect()
    constexpr int    QUERY_MS        = 500;        // total wall-clock bound on one query()
    constexpr size_t MAX_REPLY_BYTES = 256 * 1024; // cap on a single state reply
    constexpr size_t MAX_EVENT_BYTES = 64 * 1024;  // cap on the partial-line event buffer

    std::string socketDir() {
        const char* his = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
        const char* xdg = g_getenv("XDG_RUNTIME_DIR");
        if (!his || !xdg)
            return "";
        return std::string(xdg) + "/hypr/" + his;
    }

    // Connect to an AF_UNIX socket, bounding the connect to timeoutMs (a wedged
    // compositor must never freeze waybar's main loop). The returned fd is left
    // NON-BLOCKING; callers poll. -1 on any failure.
    int connectUnix(const std::string& path, int timeoutMs) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0)
            return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            close(fd);
            return -1;
        }
        std::strcpy(addr.sun_path, path.c_str());

        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            if (errno != EINPROGRESS) {
                close(fd);
                return -1;
            }
            pollfd p{fd, POLLOUT, 0};
            int    pr;
            do {
                pr = poll(&p, 1, timeoutMs);
            } while (pr < 0 && errno == EINTR);
            if (pr <= 0) {
                close(fd);
                return -1;
            }
            int       err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                close(fd);
                return -1;
            }
        }
        return fd;
    }

    // One request/reply over .socket.sock (the hyprctl wire format IS the raw
    // command string). Bounded end to end by QUERY_MS so a stalled or trickling
    // compositor can never hang the bar. Returns "" on any failure (the caller
    // keeps the last good render rather than blanking).
    std::string query(const std::string& request) {
        const auto dir = socketDir();
        if (dir.empty())
            return "";
        const int fd = connectUnix(dir + "/.socket.sock", CONNECT_MS);
        if (fd < 0)
            return "";

        std::string out;
        if (write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size())) {
            shutdown(fd, SHUT_WR);
            const gint64 deadline = g_get_monotonic_time() + static_cast<gint64>(QUERY_MS) * 1000;
            char         buf[4096];
            while (out.size() < MAX_REPLY_BYTES) {
                const gint64 remainingUs = deadline - g_get_monotonic_time();
                if (remainingUs <= 0)
                    break;
                pollfd p{fd, POLLIN, 0};
                const int pr = poll(&p, 1, static_cast<int>(remainingUs / 1000));
                if (pr < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (pr == 0)
                    break; // deadline
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                        continue;
                    break;
                }
                if (n == 0)
                    break; // EOF
                out.append(buf, n);
            }
        }
        close(fd);
        return out;
    }

    // ── output detection ────────────────────────────────────────────────
    // A CFFI instance lives in one bar on one output, but waybar doesn't tell it
    // which. gtk_layer_get_monitor gives the bar's GdkMonitor authoritatively;
    // we then map that to a Hyprland connector name by LOGICAL POSITION. We must
    // read positions from xdg-output (Hyprland hardcodes wl_output.geometry to
    // 0,0), bound on our own event queue so we never disturb GDK's dispatching.
    struct WlOut {
        wl_output*      out = nullptr;
        zxdg_output_v1* xdg = nullptr;
        std::string     name;                    // connector (wl_output v4 name event)
        int32_t         lx = INT32_MIN, ly = INT32_MIN; // xdg-output logical position
    };

    void outName(void* data, wl_output*, const char* n) {
        static_cast<WlOut*>(data)->name = n ? n : "";
    }
    void outGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
    void outMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
    void outDone(void*, wl_output*) {}
    void outScale(void*, wl_output*, int32_t) {}
    void outDescription(void*, wl_output*, const char*) {}
    const wl_output_listener OUT_LISTENER = {outGeometry, outMode, outDone, outScale, outName, outDescription};

    void xdgLogicalPosition(void* data, zxdg_output_v1*, int32_t x, int32_t y) {
        auto* o = static_cast<WlOut*>(data);
        o->lx   = x;
        o->ly   = y;
    }
    void xdgLogicalSize(void*, zxdg_output_v1*, int32_t, int32_t) {}
    void xdgDone(void*, zxdg_output_v1*) {}
    void xdgName(void*, zxdg_output_v1*, const char*) {}
    void xdgDescription(void*, zxdg_output_v1*, const char*) {}
    const zxdg_output_v1_listener XDG_LISTENER = {xdgLogicalPosition, xdgLogicalSize, xdgDone, xdgName, xdgDescription};

    struct ScanState {
        zxdg_output_manager_v1*             xdgMgr = nullptr;
        std::vector<std::unique_ptr<WlOut>> outs;
    };

    void regGlobal(void* data, wl_registry* reg, uint32_t id, const char* iface, uint32_t version) {
        auto* st = static_cast<ScanState*>(data);
        if (std::strcmp(iface, wl_output_interface.name) == 0 && version >= 4) {
            auto o = std::make_unique<WlOut>();
            o->out = static_cast<wl_output*>(wl_registry_bind(reg, id, &wl_output_interface, 4));
            wl_output_add_listener(o->out, &OUT_LISTENER, o.get());
            st->outs.push_back(std::move(o));
        } else if (std::strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
            st->xdgMgr = static_cast<zxdg_output_manager_v1*>(
                wl_registry_bind(reg, id, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
        }
    }
    void regRemove(void*, wl_registry*, uint32_t) {}
    const wl_registry_listener REG_LISTENER = {regGlobal, regRemove};

    struct OutputInfo {
        std::string name;
        int32_t     lx, ly;
    };

    std::vector<OutputInfo> scanOutputs() {
        std::vector<OutputInfo> result;
        GdkDisplay*             gdisp = gdk_display_get_default();
        if (!gdisp || !GDK_IS_WAYLAND_DISPLAY(gdisp))
            return result;
        wl_display* disp = gdk_wayland_display_get_wl_display(gdisp);

        ScanState       st;
        wl_event_queue* q       = wl_display_create_queue(disp);
        auto*           wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(disp));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapped), q);
        wl_registry* reg = wl_display_get_registry(wrapped);
        wl_registry_add_listener(reg, &REG_LISTENER, &st);
        wl_display_roundtrip_queue(disp, q); // deliver globals (wl_outputs + xdg manager)

        if (st.xdgMgr) {
            for (auto& o : st.outs) {
                o->xdg = zxdg_output_manager_v1_get_xdg_output(st.xdgMgr, o->out);
                zxdg_output_v1_add_listener(o->xdg, &XDG_LISTENER, o.get());
            }
            wl_display_roundtrip_queue(disp, q); // deliver wl_output name + xdg logical_position
        }

        for (auto& o : st.outs) {
            result.push_back({o->name, o->lx, o->ly});
            if (o->xdg)
                zxdg_output_v1_destroy(o->xdg);
            wl_output_release(o->out); // v3+ release (not destroy) so the server frees its resource
        }
        if (st.xdgMgr)
            zxdg_output_manager_v1_destroy(st.xdgMgr);
        wl_registry_destroy(reg);
        wl_proxy_wrapper_destroy(wrapped);
        wl_event_queue_destroy(q);
        return result;
    }

    // ── instance ────────────────────────────────────────────────────────
    struct Instance {
        GtkWidget*  root  = nullptr; // waybar's EventBox for this module
        GtkLabel*   label = nullptr;
        gulong      mapHandler = 0;
        std::string explicitOutput, output, lastClass;
        bool        degraded = false; // last query failed → warned once, holding last render
        MagicConfig config;
        GIOChannel* chan = nullptr;
        guint       watchId = 0, debounceId = 0, reconnectId = 0;
        std::string evBuf;
    };

    void detectOutput(Instance* inst);

    void render(Instance* inst) {
        if (inst->output.empty())
            detectOutput(inst); // self-heals if detection wasn't possible at map
        if (inst->output.empty()) {
            if (!inst->degraded) {
                g_warning("%s: could not resolve this bar's monitor; leaving last state", LOG_TAG);
                inst->degraded = true;
            }
            return;
        }

        const std::string reply = query(std::string(STATE_COMMAND) + " " + inst->output);
        if (reply.empty()) {
            // Socket failure (compositor stalled / down). Keep the last good
            // render instead of blanking the widget; warn once per outage.
            if (!inst->degraded) {
                g_warning("%s: state query for '%s' failed; keeping last render", LOG_TAG, inst->output.c_str());
                inst->degraded = true;
            }
            return;
        }
        inst->degraded = false;

        const auto out = renderMagic(reply, inst->config);
        gtk_label_set_markup(inst->label, out.text.c_str());
        gtk_widget_set_tooltip_text(GTK_WIDGET(inst->label), out.tooltip.c_str());

        if (out.cssClass != inst->lastClass) { // avoid restyling churn when unchanged
            GtkStyleContext* ctx = gtk_widget_get_style_context(inst->root);
            if (!inst->lastClass.empty())
                gtk_style_context_remove_class(ctx, inst->lastClass.c_str());
            if (!out.cssClass.empty())
                gtk_style_context_add_class(ctx, out.cssClass.c_str());
            inst->lastClass = out.cssClass;
        }
    }

    void detectOutput(Instance* inst) {
        if (!inst->explicitOutput.empty()) {
            inst->output = inst->explicitOutput;
            return;
        }
        // Authoritative: waybar pins each bar window to its monitor via
        // gtk-layer-shell, available immediately — unlike monitor_at_window,
        // which reads wl_surface.enter state that hasn't arrived at map time.
        GtkWidget*  top = gtk_widget_get_toplevel(GTK_WIDGET(inst->label));
        GdkMonitor* mon = nullptr;
        if (top && GTK_IS_WINDOW(top) && gtk_layer_is_layer_window(GTK_WINDOW(top)))
            mon = gtk_layer_get_monitor(GTK_WINDOW(top));
        if (!mon) {
            GdkWindow*  win  = top ? gtk_widget_get_window(top) : nullptr;
            GdkDisplay* disp = gdk_display_get_default();
            if (win && disp)
                mon = gdk_display_get_monitor_at_window(disp, win);
        }
        if (!mon)
            return;

        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo); // logical coords, == xdg-output logical position

        // Primary: match by logical position — unique per monitor, so this is
        // correct even for two identical-model ("twin") displays.
        const auto outs = scanOutputs();
        for (const auto& o : outs)
            if (!o.name.empty() && o.lx == geo.x && o.ly == geo.y) {
                inst->output = o.name;
                return;
            }
        // Fallback: hand the EDID model to the plugin's description matcher.
        // Skip if it contains '/': HyprCtl's getReply scans any request with a
        // '/' for flag chars over the WHOLE string, and "magic:waybar-state"
        // itself contains 'a' → it would spuriously set the 'all' param.
        if (const char* model = gdk_monitor_get_model(mon))
            if (*model && !std::strchr(model, '/'))
                inst->output = model;
    }

    void connectEventSocket(Instance* inst);

    gboolean onReconnect(gpointer data) {
        auto* inst        = static_cast<Instance*>(data);
        inst->reconnectId = 0;
        connectEventSocket(inst);
        if (inst->chan)
            render(inst); // catch up on whatever happened while disconnected
        return G_SOURCE_REMOVE;
    }

    void scheduleReconnect(Instance* inst) {
        if (inst->chan) {
            g_io_channel_unref(inst->chan);
            inst->chan = nullptr;
        }
        inst->watchId = 0; // the watch is being removed by our G_SOURCE_REMOVE return
        if (inst->reconnectId == 0)
            inst->reconnectId = g_timeout_add_seconds(RECONNECT_S, onReconnect, inst);
    }

    gboolean onDebounce(gpointer data) {
        auto* inst       = static_cast<Instance*>(data);
        inst->debounceId = 0;
        render(inst);
        return G_SOURCE_REMOVE;
    }

    gboolean onEvent(GIOChannel* chan, GIOCondition cond, gpointer data) {
        auto* inst = static_cast<Instance*>(data);

        // Drain whatever is readable FIRST, even if HUP is also set — the final
        // events before a compositor restart are worth rendering.
        if (cond & G_IO_IN) {
            char          buf[4096];
            const ssize_t n = read(g_io_channel_unix_get_fd(chan), buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    return G_SOURCE_CONTINUE; // transient, not a disconnect
                scheduleReconnect(inst);
                return G_SOURCE_REMOVE;
            }
            if (n == 0) { // EOF
                scheduleReconnect(inst);
                return G_SOURCE_REMOVE;
            }
            inst->evBuf.append(buf, n);

            bool   dirty = false;
            size_t pos;
            while ((pos = inst->evBuf.find('\n')) != std::string::npos) {
                if (isRelevantEvent(inst->evBuf.substr(0, pos)))
                    dirty = true;
                inst->evBuf.erase(0, pos + 1);
            }
            if (inst->evBuf.size() > MAX_EVENT_BYTES)
                inst->evBuf.clear(); // cap a pathological partial line

            if (dirty && inst->debounceId == 0)
                inst->debounceId = g_timeout_add(DEBOUNCE_MS, onDebounce, inst);
        }

        if (cond & (G_IO_HUP | G_IO_ERR)) {
            scheduleReconnect(inst);
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    void connectEventSocket(Instance* inst) {
        const auto dir = socketDir();
        const int  fd  = dir.empty() ? -1 : connectUnix(dir + "/.socket2.sock", CONNECT_MS);
        if (fd < 0) {
            scheduleReconnect(inst);
            return;
        }
        inst->chan = g_io_channel_unix_new(fd);
        g_io_channel_set_close_on_unref(inst->chan, TRUE);
        g_io_channel_set_encoding(inst->chan, nullptr, nullptr); // raw bytes
        inst->watchId = g_io_add_watch(inst->chan, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), onEvent, inst);
    }

    // Output detection needs the toplevel to be mapped on its monitor; "map"
    // fires every time the bar is shown — re-detect + re-render each time.
    void onMap(GtkWidget*, gpointer data) {
        auto* inst = static_cast<Instance*>(data);
        inst->output.clear(); // re-resolve: the bar may have moved monitors
        inst->degraded = false;
        render(inst);
    }

} // namespace

extern "C" {

const size_t wbcffi_version = 1; // v1: string config values arrive bare

void* wbcffi_init(const wbcffi_init_info* info, const wbcffi_config_entry* entries, size_t len) {
    auto* inst = new Instance();

    for (size_t i = 0; i < len; i++) {
        const std::string k = entries[i].key;
        const std::string v = entries[i].value;
        if (k == "output")
            inst->explicitOutput = v;
        else if (k == "focused-bg")
            inst->config.focusedBg = v;
        else if (k == "focused-fg")
            inst->config.focusedFg = v;
        else if (k == "visible-bg")
            inst->config.visibleBg = v;
        else if (k == "visible-fg")
            inst->config.visibleFg = v;
        else if (k == "hidden-bg")
            inst->config.hiddenBg = v;
        else if (k == "hidden-fg")
            inst->config.hiddenFg = v;
        else if (k.starts_with("icon-"))
            inst->config.icons[k.substr(5)] = v;
    }

    inst->root = GTK_WIDGET(info->get_root_widget(info->obj));
    gtk_widget_set_name(inst->root, WIDGET_NAME);

    inst->label = GTK_LABEL(gtk_label_new(""));
    gtk_container_add(GTK_CONTAINER(inst->root), GTK_WIDGET(inst->label));
    gtk_widget_show(GTK_WIDGET(inst->label));

    inst->mapHandler = g_signal_connect(inst->root, "map", G_CALLBACK(onMap), inst);
    connectEventSocket(inst);
    return inst;
}

void wbcffi_deinit(void* instance) {
    auto* inst = static_cast<Instance*>(instance);
    // waybar destroys the EventBox AFTER deinit, so the map handler could still
    // fire on a freed Instance — disconnect it first.
    if (inst->mapHandler && inst->root)
        g_signal_handler_disconnect(inst->root, inst->mapHandler);
    if (inst->watchId)
        g_source_remove(inst->watchId);
    if (inst->debounceId)
        g_source_remove(inst->debounceId);
    if (inst->reconnectId)
        g_source_remove(inst->reconnectId);
    if (inst->chan)
        g_io_channel_unref(inst->chan);
    delete inst; // label is destroyed with waybar's container
}

// waybar's CFFI dispatcher calls these unconditionally when present in its
// hook table (CFFI::update/refresh don't null-check) — and refresh(signal)
// reaches every module on any SIGRTMIN+N (e.g. the mako do-not-disturb bind's
// pkill -RTMIN+1 waybar). Export no-ops so those paths can never crash.
void wbcffi_update(void*) {}
void wbcffi_refresh(void*, int) {}
void wbcffi_doaction(void*, const char*) {}

} // extern "C"
