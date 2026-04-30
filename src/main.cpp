#include <gtk-layer-shell/gtk-layer-shell.h>
#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr const char *VERSION = "0.1.0";
constexpr double FRAME_INTERVAL_SECONDS = 1.0 / 30.0;
constexpr int FRAME_INTERVAL_MS = 33;
constexpr int HYPR_REFRESH_MS = 5000;

struct Rect {
    int x = 0;
    int y = 0;
    int width = 1920;
    int height = 1080;
};

struct MonitorInfo {
    std::string name;
    Rect geometry;
    int activeWorkspaceId = 0;
};

struct ClientInfo {
    std::string key;
    std::string className;
    std::string title;
    std::string monitorName;
    int workspaceId = 0;
    Rect geometry;
    bool mapped = true;
    bool hidden = false;
};

struct Particle {
    std::string key;
    double x = 0;
    double y = 0;
    double w = 18;
    double h = 12;
    double vx = 12;
    double vy = 9;
    double red = 0.20;
    double green = 0.42;
    double blue = 0.54;
    double alpha = 0.10;
};

struct SurfaceState {
    GtkWidget *window = nullptr;
    GtkWidget *area = nullptr;
    std::string monitorName;
    Rect hyprGeometry;
    std::vector<Particle> particles;
    gint64 lastFrameUs = 0;
};

std::vector<std::unique_ptr<SurfaceState>> g_surfaces;
std::vector<MonitorInfo> g_monitors;
std::vector<ClientInfo> g_clients;

double clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

uint64_t hashString(const std::string &value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

double hashUnit(uint64_t hash, int shift) {
    return static_cast<double>((hash >> shift) & 0xffff) / 65535.0;
}

std::string runCommand(const char *command) {
    std::array<char, 4096> buffer{};
    std::string output;

    FILE *pipe = popen(command, "r");
    if (!pipe)
        return output;

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += buffer.data();

    pclose(pipe);
    return output;
}

std::optional<json> commandJson(const char *command) {
    const auto output = runCommand(command);
    if (output.empty())
        return std::nullopt;

    auto parsed = json::parse(output, nullptr, false);
    if (parsed.is_discarded())
        return std::nullopt;

    return parsed;
}

int jsonInt(const json &object, const char *key, int fallback = 0) {
    if (!object.contains(key) || !object.at(key).is_number())
        return fallback;
    return object.at(key).get<int>();
}

std::string jsonString(const json &object, const char *key) {
    if (!object.contains(key) || !object.at(key).is_string())
        return {};
    return object.at(key).get<std::string>();
}

Rect jsonRectFromArrays(const json &object) {
    Rect result;

    if (object.contains("at") && object.at("at").is_array() && object.at("at").size() >= 2) {
        result.x = object.at("at").at(0).get<int>();
        result.y = object.at("at").at(1).get<int>();
    }

    if (object.contains("size") && object.at("size").is_array() && object.at("size").size() >= 2) {
        result.width = object.at("size").at(0).get<int>();
        result.height = object.at("size").at(1).get<int>();
    }

    return result;
}

std::vector<MonitorInfo> loadMonitors() {
    std::vector<MonitorInfo> result;
    const auto monitors = commandJson("hyprctl -j monitors 2>/dev/null");
    if (!monitors || !monitors->is_array())
        return result;

    for (const auto &item : *monitors) {
        MonitorInfo monitor;
        monitor.name = jsonString(item, "name");
        monitor.geometry.x = jsonInt(item, "x");
        monitor.geometry.y = jsonInt(item, "y");
        monitor.geometry.width = jsonInt(item, "width", 1920);
        monitor.geometry.height = jsonInt(item, "height", 1080);

        if (item.contains("activeWorkspace") && item.at("activeWorkspace").is_object())
            monitor.activeWorkspaceId = jsonInt(item.at("activeWorkspace"), "id");

        if (!monitor.name.empty())
            result.push_back(std::move(monitor));
    }

    return result;
}

std::vector<ClientInfo> loadClients() {
    std::vector<ClientInfo> result;
    const auto clients = commandJson("hyprctl -j clients 2>/dev/null");
    if (!clients || !clients->is_array())
        return result;

    for (const auto &item : *clients) {
        ClientInfo client;
        client.key = jsonString(item, "address");
        client.className = jsonString(item, "class");
        client.title = jsonString(item, "title");
        client.monitorName = jsonString(item, "monitor");
        client.geometry = jsonRectFromArrays(item);

        if (item.contains("workspace") && item.at("workspace").is_object())
            client.workspaceId = jsonInt(item.at("workspace"), "id");

        if (item.contains("mapped") && item.at("mapped").is_boolean())
            client.mapped = item.at("mapped").get<bool>();

        if (item.contains("hidden") && item.at("hidden").is_boolean())
            client.hidden = item.at("hidden").get<bool>();

        if (client.key.empty())
            client.key = client.className + ":" + client.title;

        result.push_back(std::move(client));
    }

    return result;
}

MonitorInfo monitorForSurface(const SurfaceState &surface) {
    for (const auto &monitor : g_monitors) {
        if (monitor.name == surface.monitorName)
            return monitor;
    }

    MonitorInfo fallback;
    fallback.name = surface.monitorName;
    fallback.geometry = surface.hyprGeometry;
    return fallback;
}

Particle particleFromClient(const ClientInfo &client, const MonitorInfo &monitor, int width,
                            int height) {
    const uint64_t hash = hashString(client.key + client.className + client.title);

    const double monitorWidth = std::max(1, monitor.geometry.width);
    const double monitorHeight = std::max(1, monitor.geometry.height);
    const double relX =
        (static_cast<double>(client.geometry.x - monitor.geometry.x) / monitorWidth);
    const double relY =
        (static_cast<double>(client.geometry.y - monitor.geometry.y) / monitorHeight);

    const double aspect =
        clamp(static_cast<double>(client.geometry.width) / std::max(1, client.geometry.height),
              0.35, 3.5);
    const double areaScale =
        std::sqrt(std::max(1.0, static_cast<double>(client.geometry.width) *
                                    static_cast<double>(client.geometry.height)) /
                  std::max(1.0, monitorWidth * monitorHeight));

    Particle particle;
    particle.key = client.key;
    particle.w = clamp(16.0 + areaScale * 88.0 * std::sqrt(aspect), 10.0, 76.0);
    particle.h = clamp(particle.w / aspect, 8.0, 48.0);
    particle.x = clamp(relX * width, 0.0, std::max(0.0, width - particle.w));
    particle.y = clamp(relY * height, 0.0, std::max(0.0, height - particle.h));

    const double angle = hashUnit(hash, 0) * 2.0 * M_PI;
    const double speed = 6.0 + hashUnit(hash, 16) * 16.0;
    particle.vx = std::cos(angle) * speed;
    particle.vy = std::sin(angle) * speed;
    particle.red = 0.08 + hashUnit(hash, 8) * 0.16;
    particle.green = 0.18 + hashUnit(hash, 24) * 0.28;
    particle.blue = 0.22 + hashUnit(hash, 40) * 0.30;
    particle.alpha = 0.08 + hashUnit(hash, 48) * 0.05;
    return particle;
}

Particle syntheticParticle(const std::string &monitorName, size_t index, int width, int height) {
    const uint64_t hash = hashString(monitorName + ":synthetic:" + std::to_string(index));
    Particle particle;
    particle.key = "synthetic:" + std::to_string(index);
    particle.w = 8.0 + hashUnit(hash, 0) * 18.0;
    particle.h = 6.0 + hashUnit(hash, 8) * 14.0;
    particle.x = hashUnit(hash, 16) * std::max(0.0, width - particle.w);
    particle.y = hashUnit(hash, 24) * std::max(0.0, height - particle.h);
    particle.vx = (hashUnit(hash, 32) - 0.5) * 18.0;
    particle.vy = (hashUnit(hash, 40) - 0.5) * 18.0;
    particle.red = 0.12;
    particle.green = 0.26;
    particle.blue = 0.30;
    particle.alpha = 0.07;
    return particle;
}

std::vector<Particle> desiredParticlesForSurface(const SurfaceState &surface, int width,
                                                 int height) {
    std::vector<Particle> particles;
    const auto monitor = monitorForSurface(surface);

    for (const auto &client : g_clients) {
        const bool visibleOnWorkspace =
            monitor.activeWorkspaceId == 0 || client.workspaceId == monitor.activeWorkspaceId;
        const bool onMonitor = client.monitorName.empty() || client.monitorName == monitor.name;

        if (!client.mapped || client.hidden || !visibleOnWorkspace || !onMonitor)
            continue;

        particles.push_back(particleFromClient(client, monitor, width, height));
    }

    if (particles.empty()) {
        for (size_t i = 0; i < 5; ++i)
            particles.push_back(syntheticParticle(surface.monitorName, i, width, height));
    }

    return particles;
}

void refreshSurfaceParticles(SurfaceState &surface) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(surface.area, &allocation);
    const int width = std::max(1, allocation.width);
    const int height = std::max(1, allocation.height);

    std::unordered_map<std::string, Particle> existing;
    for (const auto &particle : surface.particles)
        existing.emplace(particle.key, particle);

    auto desired = desiredParticlesForSurface(surface, width, height);
    for (auto &particle : desired) {
        const auto found = existing.find(particle.key);
        if (found == existing.end())
            continue;

        const auto old = found->second;
        particle.x = clamp(old.x, 0.0, std::max(0.0, width - particle.w));
        particle.y = clamp(old.y, 0.0, std::max(0.0, height - particle.h));
        particle.vx = old.vx;
        particle.vy = old.vy;
    }

    surface.particles = std::move(desired);
}

void refreshHyprlandState() {
    auto monitors = loadMonitors();
    auto clients = loadClients();

    if (!monitors.empty())
        g_monitors = std::move(monitors);

    g_clients = std::move(clients);

    for (auto &surface : g_surfaces)
        refreshSurfaceParticles(*surface);
}

void drawClock(cairo_t *cr, GtkWidget *widget) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);

    const auto nowTime = std::time(nullptr);
    std::tm localTime{};
    localtime_r(&nowTime, &localTime);

    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M", &localTime);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, buffer, -1);

    PangoFontDescription *font = pango_font_description_from_string("Noto Sans 34");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int textWidth = 0;
    int textHeight = 0;
    pango_layout_get_pixel_size(layout, &textWidth, &textHeight);

    const double seconds = static_cast<double>(g_get_monotonic_time()) / 1000000.0;
    const double xTravel = std::max(1, allocation.width - textWidth - 80);
    const double yTravel = std::max(1, allocation.height - textHeight - 80);
    const double x = 40.0 + (std::sin(seconds / 37.0) * 0.5 + 0.5) * xTravel;
    const double y = 40.0 + (std::cos(seconds / 53.0) * 0.5 + 0.5) * yTravel;

    cairo_set_source_rgba(cr, 0.34, 0.48, 0.50, 0.34);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

gboolean onDraw(GtkWidget *widget, cairo_t *cr, gpointer userData) {
    auto *surface = static_cast<SurfaceState *>(userData);

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    for (const auto &particle : surface->particles) {
        cairo_set_source_rgba(cr, particle.red, particle.green, particle.blue, particle.alpha);
        cairo_rectangle(cr, particle.x, particle.y, particle.w, particle.h);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, particle.red + 0.08, particle.green + 0.08, particle.blue + 0.08,
                              particle.alpha + 0.05);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }

    drawClock(cr, widget);
    return FALSE;
}

void stepParticles(SurfaceState &surface, double dt) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(surface.area, &allocation);

    const double width = std::max(1, allocation.width);
    const double height = std::max(1, allocation.height);

    for (auto &particle : surface.particles) {
        particle.x += particle.vx * dt;
        particle.y += particle.vy * dt;

        if (particle.x < 0.0) {
            particle.x = 0.0;
            particle.vx = std::abs(particle.vx);
        } else if (particle.x + particle.w > width) {
            particle.x = width - particle.w;
            particle.vx = -std::abs(particle.vx);
        }

        if (particle.y < 0.0) {
            particle.y = 0.0;
            particle.vy = std::abs(particle.vy);
        } else if (particle.y + particle.h > height) {
            particle.y = height - particle.h;
            particle.vy = -std::abs(particle.vy);
        }
    }

    for (size_t i = 0; i < surface.particles.size(); ++i) {
        for (size_t j = i + 1; j < surface.particles.size(); ++j) {
            auto &a = surface.particles[i];
            auto &b = surface.particles[j];
            const bool overlaps =
                a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
            if (!overlaps)
                continue;

            std::swap(a.vx, b.vx);
            std::swap(a.vy, b.vy);
        }
    }
}

gboolean onFrame(gpointer userData) {
    auto *surface = static_cast<SurfaceState *>(userData);
    const auto nowUs = g_get_monotonic_time();
    if (surface->lastFrameUs == 0)
        surface->lastFrameUs = nowUs;

    const double dt =
        clamp(static_cast<double>(nowUs - surface->lastFrameUs) / 1000000.0, 0.0, 0.2);
    surface->lastFrameUs = nowUs;

    stepParticles(*surface, dt);
    gtk_widget_queue_draw(surface->area);
    return G_SOURCE_CONTINUE;
}

gboolean onRefresh(gpointer) {
    refreshHyprlandState();
    return G_SOURCE_CONTINUE;
}

void configureLayerWindow(GtkWindow *window, GdkMonitor *monitor) {
    gtk_layer_init_for_window(window);
    gtk_layer_set_namespace(window, "hypr-oled-saver");
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_monitor(window, monitor);
    gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(window, -1);

    for (auto edge : {GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT,
                      GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM}) {
        gtk_layer_set_anchor(window, edge, TRUE);
    }
}

std::unique_ptr<SurfaceState> createSurface(GdkMonitor *monitor, size_t index) {
    auto surface = std::make_unique<SurfaceState>();

    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    const char *model = gdk_monitor_get_model(monitor);

    surface->monitorName = model ? model : ("monitor-" + std::to_string(index));
    surface->hyprGeometry =
        Rect{geometry.x, geometry.y, std::max(1, geometry.width), std::max(1, geometry.height)};

    for (const auto &hyprMonitor : g_monitors) {
        if (hyprMonitor.geometry.x == geometry.x && hyprMonitor.geometry.y == geometry.y) {
            surface->monitorName = hyprMonitor.name;
            surface->hyprGeometry = hyprMonitor.geometry;
            break;
        }
    }

    surface->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(surface->window), "hypr-oled-saver");
    gtk_window_set_decorated(GTK_WINDOW(surface->window), FALSE);
    gtk_widget_set_app_paintable(surface->window, TRUE);
    configureLayerWindow(GTK_WINDOW(surface->window), monitor);

    surface->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(surface->area, TRUE);
    gtk_widget_set_vexpand(surface->area, TRUE);
    gtk_container_add(GTK_CONTAINER(surface->window), surface->area);

    g_signal_connect(surface->area, "draw", G_CALLBACK(onDraw), surface.get());
    g_signal_connect(surface->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    gtk_widget_show_all(surface->window);
    refreshSurfaceParticles(*surface);

    g_timeout_add(FRAME_INTERVAL_MS, onFrame, surface.get());
    return surface;
}

void createSurfaces() {
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;

    const int monitorCount = std::max(1, gdk_display_get_n_monitors(display));
    for (int i = 0; i < monitorCount; ++i) {
        GdkMonitor *monitor = gdk_display_get_monitor(display, i);
        if (!monitor)
            continue;
        g_surfaces.push_back(createSurface(monitor, static_cast<size_t>(i)));
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1) {
        const std::string arg = argv[1];

        if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: hypr-oled-saver [--help] [--version]\n\n"
                "Starts an OLED-friendly GTK layer-shell screensaver for Hyprland.\n"
                "Stop it by terminating the process, for example: pkill -x hypr-oled-saver\n");
            return 0;
        }

        if (arg == "--version") {
            std::printf("hypr-oled-saver %s\n", VERSION);
            return 0;
        }
    }

    gtk_init(&argc, &argv);

    refreshHyprlandState();
    createSurfaces();
    g_timeout_add(HYPR_REFRESH_MS, onRefresh, nullptr);

    gtk_main();
    return 0;
}
