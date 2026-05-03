#define WLR_USE_UNSTABLE

#include <algorithm>
#include <any>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <lua.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#define private public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#undef private
#undef protected
#include <xkbcommon/xkbcommon.h>

inline HANDLE PHANDLE = nullptr;

namespace {

class COledSaver;
inline std::unique_ptr<COledSaver> g_pOledSaver;
inline bool g_dismissAfterActivity = false;

static const CConfigValue<Config::INTEGER> &PBACKGROUND() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:background");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PBORDER() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:border_color");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PBORDERSIZE() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:border_size");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PMARGIN() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:margin");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PGAP() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:gap");
    return VALUE;
}

static const CConfigValue<Config::FLOAT> &PSPEED() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:speed");
    return VALUE;
}

static const CConfigValue<Config::FLOAT> &POPACITY() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:opacity");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PDISMISSONACTIVITY() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:dismiss_on_activity");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PACTIVITYGRACEMS() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:activity_grace_ms");
    return VALUE;
}

static uint32_t framebufferFormatWithAlpha(uint32_t) {
    return DRM_FORMAT_ABGR8888;
}

static uint64_t hashString(const std::string &value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static double hashUnit(uint64_t hash, int shift) {
    return static_cast<double>((hash >> shift) & 0xffff) / 65535.0;
}

static bool previewableWindow(const PHLWINDOW &window) {
    if (!window || !window->m_isMapped || window->isHidden() || window->m_fadingOut ||
        !window->m_workspace)
        return false;

    if (window->m_size.x <= 1 || window->m_size.y <= 1 || window->m_realSize->value().x <= 1 ||
        window->m_realSize->value().y <= 1)
        return false;

    return true;
}

static void clampBoxToBounds(CBox &box, const Vector2D &size) {
    box.x = std::clamp(box.x, 0.0, std::max(0.0, size.x - box.w));
    box.y = std::clamp(box.y, 0.0, std::max(0.0, size.y - box.h));
}

static bool boxesOverlap(const CBox &a, const CBox &b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

struct SPreview {
    PHLWINDOW window;
    SP<Render::IFramebuffer> fb;
    CBox box;
    Vector2D velocity;
};

class COledSaver {
  public:
    explicit COledSaver(PHLMONITOR monitor) : pMonitor(monitor) {
        lastFrame = Time::steadyNow();
        activatedAt = lastFrame;
        installActivityHooks();
        refreshWindows();
        damage();
    }

    ~COledSaver() {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        for (auto &preview : previews)
            preview.fb.reset();
        previews.clear();
    }

    void render() {
        g_pHyprRenderer->m_renderPass.add(makeUnique<COledSaverPassElement>());
    }

    void draw() {
        if (!pMonitor)
            return;

        stepPhysics();

        CRegion fullDamage = {0, 0, INT16_MAX, INT16_MAX};
        Render::GL::g_pHyprOpenGL->renderRect(CBox{{0, 0}, pMonitor->m_pixelSize},
                                              CHyprColor(*PBACKGROUND()), {.damage = &fullDamage});

        const double scale = pMonitor->m_scale;
        const int border = std::max<Config::INTEGER>(0, *PBORDERSIZE());
        const float opacity = std::clamp<float>(*POPACITY(), 0.05F, 1.0F);

        for (auto &preview : previews) {
            if (!preview.window || !preview.fb || !preview.fb->getTexture())
                continue;

            CBox tilePx = preview.box.copy().scale(scale).round();
            CBox texBox = {
                tilePx.x,
                tilePx.y,
                pMonitor->m_pixelSize.x *
                    (tilePx.w / std::max(1.0, preview.window->m_realSize->value().x * scale)),
                pMonitor->m_pixelSize.y *
                    (tilePx.h / std::max(1.0, preview.window->m_realSize->value().y * scale)),
            };

            if (border > 0)
                Render::GL::g_pHyprOpenGL->renderRect(tilePx.copy().expand(border),
                                                      CHyprColor(*PBORDER()),
                                                      {.damage = &fullDamage, .round = border * 2});

            g_pHyprRenderer->m_renderData.clipBox = tilePx;
            Render::GL::g_pHyprOpenGL->renderTexture(
                preview.fb->getTexture(), texBox,
                {.damage = &fullDamage, .a = opacity, .round = border * 2});
            g_pHyprRenderer->m_renderData.clipBox = {};
        }

        damage();
    }

    void damage() {
        if (pMonitor)
            g_pHyprRenderer->damageMonitor(pMonitor.lock());
    }

    PHLMONITORREF pMonitor;

  private:
    class COledSaverPassElement : public IPassElement {
      public:
        std::vector<UP<IPassElement>> draw() override {
            if (g_pOledSaver)
                g_pOledSaver->draw();
            return {};
        }

        bool needsLiveBlur() override {
            return false;
        }

        bool needsPrecomputeBlur() override {
            return false;
        }

        std::optional<CBox> boundingBox() override {
            if (!g_pOledSaver || !g_pOledSaver->pMonitor)
                return std::nullopt;
            return CBox{{0, 0}, g_pOledSaver->pMonitor->m_size};
        }

        CRegion opaqueRegion() override {
            if (!g_pOledSaver || !g_pOledSaver->pMonitor)
                return {};
            return CBox{{0, 0}, g_pOledSaver->pMonitor->m_size};
        }

        const char *passName() override {
            return "COledSaverPassElement";
        }

        ePassElementType type() override {
            return EK_CUSTOM;
        }
    };

    void installActivityHooks() {
        auto onActivity = [this](Event::SCallbackInfo &info) { handleActivity(info); };

        mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen(
            [onActivity](Vector2D, Event::SCallbackInfo &info) { onActivity(info); });
        mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen(
            [this](IPointer::SButtonEvent event, Event::SCallbackInfo &info) {
                handlePointerButton(event, info);
            });
        touchMoveHook = Event::bus()->m_events.input.touch.motion.listen(
            [onActivity](ITouch::SMotionEvent, Event::SCallbackInfo &info) { onActivity(info); });
        touchDownHook = Event::bus()->m_events.input.touch.down.listen(
            [onActivity](ITouch::SDownEvent, Event::SCallbackInfo &info) { onActivity(info); });
        keyboardHook = Event::bus()->m_events.input.keyboard.key.listen(
            [this](IKeyboard::SKeyEvent event, Event::SCallbackInfo &info) {
                handleKeyboard(event, info);
            });
    }

    void handleActivity(Event::SCallbackInfo &info) {
        if (!shouldDismissForActivity())
            return;

        dismissFromInput(info);
    }

    void handlePointerButton(const IPointer::SButtonEvent &event, Event::SCallbackInfo &info) {
        if (isEmergencyPointerButton(event) || shouldDismissForActivity())
            dismissFromInput(info);
    }

    void handleKeyboard(const IKeyboard::SKeyEvent &event, Event::SCallbackInfo &info) {
        if (isEmergencyKey(event) || shouldDismissForActivity())
            dismissFromInput(info);
    }

    void dismissFromInput(Event::SCallbackInfo &info) {
        info.cancelled = true;
        g_dismissAfterActivity = true;
        damage();
    }

    bool shouldDismissForActivity() const {
        if (*PDISMISSONACTIVITY() == 0)
            return false;

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - activatedAt)
                .count();
        return elapsed >= std::max<Config::INTEGER>(0, *PACTIVITYGRACEMS());
    }

    bool isEmergencyPointerButton(const IPointer::SButtonEvent &event) const {
        return event.state == WL_POINTER_BUTTON_STATE_PRESSED && event.button == BTN_RIGHT;
    }

    bool isEmergencyKey(const IKeyboard::SKeyEvent &event) const {
        if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return false;

        const auto keyboard = g_pSeatManager && !g_pSeatManager->m_keyboard.expired()
                                  ? g_pSeatManager->m_keyboard.lock()
                                  : nullptr;
        if (!keyboard || !keyboard->m_xkbState)
            return false;

        const auto keycode = event.keycode + 8;
        return xkb_state_key_get_one_sym(keyboard->m_xkbState, keycode) == XKB_KEY_Escape;
    }

    void refreshWindows() {
        previews.clear();

        for (auto it = g_pCompositor->m_windows.rbegin(); it != g_pCompositor->m_windows.rend();
             ++it) {
            const auto &window = *it;
            if (!previewableWindow(window))
                continue;
            previews.push_back({.window = window});
        }

        std::ranges::reverse(previews);
        layoutInitialGrid();
        renderSnapshots();
    }

    void layoutInitialGrid() {
        if (!pMonitor || previews.empty())
            return;

        const double count = previews.size();
        const double aspect = std::max(0.1, pMonitor->m_size.x / std::max(1.0, pMonitor->m_size.y));
        int cols = std::max(1, (int)std::ceil(std::sqrt(count * aspect)));
        int rows = std::max(1, (int)std::ceil(count / cols));

        while (cols > 1 && (cols - 1) * rows >= (int)previews.size())
            cols--;

        rows = std::max(1, (int)std::ceil(count / cols));

        const double margin = std::max<Config::INTEGER>(0, *PMARGIN());
        const double gap = std::max<Config::INTEGER>(0, *PGAP());
        const double areaW = std::max(1.0, pMonitor->m_size.x - margin * 2.0);
        const double areaH = std::max(1.0, pMonitor->m_size.y - margin * 2.0);
        const double cellW = (areaW - gap * (cols - 1)) / cols;
        const double cellH = (areaH - gap * (rows - 1)) / rows;

        for (size_t i = 0; i < previews.size(); ++i) {
            auto &preview = previews[i];
            const int row = i / cols;
            const int col = i % cols;
            const auto winSize = preview.window->m_realSize->value();
            const double scale = std::min((cellW * 0.94) / std::max(1.0, winSize.x),
                                          (cellH * 0.9) / std::max(1.0, winSize.y));
            const double w = std::max(1.0, winSize.x * scale);
            const double h = std::max(1.0, winSize.y * scale);
            const double x = margin + col * (cellW + gap) + (cellW - w) * hashUnit(seedFor(i), 0);
            const double y = margin + row * (cellH + gap) + (cellH - h) * hashUnit(seedFor(i), 16);

            preview.box = {x, y, w, h};
            preview.velocity = velocityFor(i);
        }
    }

    uint64_t seedFor(size_t index) const {
        const auto &preview = previews[index];
        const auto klass = preview.window ? preview.window->m_class : "";
        return hashString(std::to_string(index) + ":" + klass);
    }

    Vector2D velocityFor(size_t index) const {
        const auto hash = seedFor(index);
        const auto angle = hashUnit(hash, 0) * 2.0 * M_PI;
        const auto speed = std::max(5.0F, *PSPEED()) * (0.65 + hashUnit(hash, 24) * 0.7);
        return {std::cos(angle) * speed, std::sin(angle) * speed};
    }

    void renderSnapshots() {
        if (!pMonitor)
            return;

        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        const auto format =
            framebufferFormatWithAlpha(pMonitor->m_output->state->state().drmFormat);

        for (auto &preview : previews) {
            if (!preview.window)
                continue;

            if (!preview.fb)
                preview.fb = g_pHyprRenderer->createFB("hypr-oled-saver");

            if (preview.fb->m_size != pMonitor->m_pixelSize) {
                preview.fb->release();
                preview.fb->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y, format);
            }

            CRegion fakeDamage{0, 0, static_cast<double>((int)pMonitor->m_transformedSize.x),
                               static_cast<double>((int)pMonitor->m_transformedSize.y)};
            if (!g_pHyprRenderer->beginFullFakeRender(pMonitor.lock(), fakeDamage, preview.fb))
                continue;

            g_pHyprRenderer->m_bRenderingSnapshot = true;
            g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor(0, 0, 0, 0)});
            g_pHyprRenderer->startRenderPass();
            g_pHyprRenderer->renderWindow(preview.window, pMonitor.lock(), Time::steadyNow(), false,
                                          Render::RENDER_PASS_ALL, true, true);
            g_pHyprRenderer->m_renderData.blockScreenShader = true;
            g_pHyprRenderer->endRender();
            g_pHyprRenderer->m_bRenderingSnapshot = false;
        }
    }

    void stepPhysics() {
        if (!pMonitor)
            return;

        const auto now = Time::steadyNow();
        const double dt =
            std::clamp(std::chrono::duration<double>(now - lastFrame).count(), 0.0, 0.05);
        lastFrame = now;

        for (auto &preview : previews) {
            preview.box.x += preview.velocity.x * dt;
            preview.box.y += preview.velocity.y * dt;
            bounceOffBounds(preview);
        }

        resolveCollisions();
    }

    void bounceOffBounds(SPreview &preview) const {
        if (preview.box.x < 0.0) {
            preview.box.x = 0.0;
            preview.velocity.x = std::abs(preview.velocity.x);
        } else if (preview.box.x + preview.box.w > pMonitor->m_size.x) {
            preview.box.x = pMonitor->m_size.x - preview.box.w;
            preview.velocity.x = -std::abs(preview.velocity.x);
        }

        if (preview.box.y < 0.0) {
            preview.box.y = 0.0;
            preview.velocity.y = std::abs(preview.velocity.y);
        } else if (preview.box.y + preview.box.h > pMonitor->m_size.y) {
            preview.box.y = pMonitor->m_size.y - preview.box.h;
            preview.velocity.y = -std::abs(preview.velocity.y);
        }
    }

    void resolveCollisions() {
        for (int pass = 0; pass < 4; ++pass) {
            bool changed = false;

            for (size_t i = 0; i < previews.size(); ++i) {
                for (size_t j = i + 1; j < previews.size(); ++j) {
                    if (!boxesOverlap(previews[i].box, previews[j].box))
                        continue;

                    resolveCollision(previews[i], previews[j]);
                    changed = true;
                }
            }

            if (!changed)
                break;
        }
    }

    void resolveCollision(SPreview &a, SPreview &b) const {
        const double overlapX =
            std::min(a.box.x + a.box.w, b.box.x + b.box.w) - std::max(a.box.x, b.box.x);
        const double overlapY =
            std::min(a.box.y + a.box.h, b.box.y + b.box.h) - std::max(a.box.y, b.box.y);

        if (overlapX <= overlapY) {
            const double direction =
                (a.box.x + a.box.w / 2.0 <= b.box.x + b.box.w / 2.0) ? -1.0 : 1.0;
            const double aSpeed = std::max(20.0, std::abs(a.velocity.x));
            const double bSpeed = std::max(20.0, std::abs(b.velocity.x));
            const double push = overlapX / 2.0 + 1.0;

            a.box.x += direction * push;
            b.box.x -= direction * push;
            a.velocity.x = direction * bSpeed;
            b.velocity.x = -direction * aSpeed;
        } else {
            const double direction =
                (a.box.y + a.box.h / 2.0 <= b.box.y + b.box.h / 2.0) ? -1.0 : 1.0;
            const double aSpeed = std::max(20.0, std::abs(a.velocity.y));
            const double bSpeed = std::max(20.0, std::abs(b.velocity.y));
            const double push = overlapY / 2.0 + 1.0;

            a.box.y += direction * push;
            b.box.y -= direction * push;
            a.velocity.y = direction * bSpeed;
            b.velocity.y = -direction * aSpeed;
        }

        clampBoxToBounds(a.box, pMonitor->m_size);
        clampBoxToBounds(b.box, pMonitor->m_size);
    }

    std::vector<SPreview> previews;
    std::chrono::steady_clock::time_point lastFrame;
    std::chrono::steady_clock::time_point activatedAt;
    CHyprSignalListener mouseMoveHook;
    CHyprSignalListener mouseButtonHook;
    CHyprSignalListener keyboardHook;
    CHyprSignalListener touchMoveHook;
    CHyprSignalListener touchDownHook;
};

static void failNotif(const std::string &reason) {
    HyprlandAPI::addNotification(PHANDLE, "[hypr-oled-saver] Failure: " + reason,
                                 CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

static bool addConfigValue(SP<Config::Values::IValue> value) {
    const auto ret = Config::mgr()->registerPluginValue(PHANDLE, value);
    if (!ret) {
        Log::logger->log(Log::ERR, "[hypr-oled-saver] failed to register plugin value \"{}\": {}",
                         value->name(), ret.error());
        return false;
    }

    return true;
}

static void mustAddConfigValue(SP<Config::Values::IValue> value) {
    if (!addConfigValue(std::move(value)))
        throw std::runtime_error("[hypr-oled-saver] Failed to register plugin config value");
}

static SDispatchResult stopSaver() {
    if (g_pOledSaver) {
        g_pOledSaver.reset();
        g_pHyprRenderer->m_renderPass.removeAllOfType("COledSaverPassElement");
    }

    g_dismissAfterActivity = false;
    return {};
}

static SDispatchResult startSaver() {
    const auto monitor = Desktop::focusState()->monitor();
    if (!monitor)
        return {.success = false, .error = "no focused monitor"};

    g_pOledSaver = std::make_unique<COledSaver>(monitor);
    return {};
}

static SDispatchResult toggleSaver() {
    if (g_pOledSaver)
        return stopSaver();

    return startSaver();
}

static SDispatchResult setSaverActive(bool active) {
    return active ? startSaver() : stopSaver();
}

static SDispatchResult controlSaver(std::string arg) {
    std::ranges::transform(arg, arg.begin(), [](unsigned char c) { return std::tolower(c); });

    if (arg.empty() || arg == "start" || arg == "on" || arg == "open" || arg == "activate" ||
        arg == "active" || arg == "enable" || arg == "present")
        return startSaver();

    if (arg == "stop" || arg == "off" || arg == "close" || arg == "deactivate" ||
        arg == "inactive" || arg == "disable" || arg == "dismiss")
        return stopSaver();

    if (arg == "toggle")
        return toggleSaver();

    return {.success = false, .error = "unknown action: " + arg};
}

static SDispatchResult onDispatcher(std::string arg) {
    return controlSaver(std::move(arg));
}

static SDispatchResult onStartDispatcher(std::string) {
    return startSaver();
}

static SDispatchResult onStopDispatcher(std::string) {
    return stopSaver();
}

static SDispatchResult onToggleDispatcher(std::string) {
    return toggleSaver();
}

static SDispatchResult onActivateDispatcher(std::string) {
    return startSaver();
}

static SDispatchResult onDeactivateDispatcher(std::string) {
    return stopSaver();
}

static SDispatchResult onPresentDispatcher(std::string) {
    return startSaver();
}

static SDispatchResult onDismissDispatcher(std::string) {
    return stopSaver();
}

static int luaNoop(lua_State *) {
    return 0;
}

static int luaControl(lua_State *L, const std::string &action) {
    const auto result = controlSaver(action);
    if (!result.success)
        return luaL_error(L, "%s", result.error.c_str());

    lua_pushcfunction(L, ::luaNoop);
    return 1;
}

static int luaStart(lua_State *L) {
    return luaControl(L, "start");
}

static int luaStop(lua_State *L) {
    return luaControl(L, "stop");
}

static int luaToggle(lua_State *L) {
    return luaControl(L, "toggle");
}

static int luaActivate(lua_State *L) {
    return luaControl(L, "activate");
}

static int luaDeactivate(lua_State *L) {
    return luaControl(L, "deactivate");
}

static int luaPresent(lua_State *L) {
    return luaControl(L, "present");
}

static int luaDismiss(lua_State *L) {
    return luaControl(L, "dismiss");
}

static int luaSetActive(lua_State *L) {
    if (lua_gettop(L) < 1 || !lua_isboolean(L, 1))
        return luaL_error(L, "hyproledsaver.set_active: argument must be a boolean");

    const auto result = setSaverActive(lua_toboolean(L, 1));
    if (!result.success)
        return luaL_error(L, "%s", result.error.c_str());

    lua_pushcfunction(L, ::luaNoop);
    return 1;
}

static int luaRun(lua_State *L) {
    std::string arg = "toggle";

    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        if (!lua_isstring(L, 1))
            return luaL_error(L, "hyproledsaver.run: argument must be a string");

        arg = lua_tostring(L, 1);
    }

    return luaControl(L, arg);
}

static int luaEnabled(lua_State *L) {
    lua_pushboolean(L, g_pOledSaver != nullptr);
    return 1;
}

static int luaIsActive(lua_State *L) {
    return luaEnabled(L);
}

static int luaRefresh(lua_State *L) {
    if (!g_pOledSaver)
        return 0;

    const auto result = startSaver();
    if (!result.success)
        return luaL_error(L, "%s", result.error.c_str());

    return 0;
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string hash = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();

    if (hash != clientHash) {
        failNotif("Version mismatch (headers ver is not equal to running Hyprland ver)");
        throw std::runtime_error("[hypr-oled-saver] Version mismatch");
    }

    mustAddConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyproledsaver:background",
                                                               "background color", 0xFF000000));
    mustAddConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyproledsaver:border_color",
                                                               "preview border color", 0x2246C7D8));
    mustAddConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyproledsaver:border_size",
                                                             "preview border size", 2));
    mustAddConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyproledsaver:margin",
                                                             "initial layout margin", 64));
    mustAddConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyproledsaver:gap",
                                                             "initial layout gap", 36));
    mustAddConfigValue(makeShared<Config::Values::CFloatValue>(
        "plugin:hyproledsaver:speed", "preview velocity in logical px/s", 85.0F));
    mustAddConfigValue(makeShared<Config::Values::CFloatValue>("plugin:hyproledsaver:opacity",
                                                               "preview texture opacity", 0.82F));
    mustAddConfigValue(makeShared<Config::Values::CIntValue>(
        "plugin:hyproledsaver:dismiss_on_activity", "dismiss when input activity is detected", 1));
    mustAddConfigValue(makeShared<Config::Values::CIntValue>(
        "plugin:hyproledsaver:activity_grace_ms", "activity ignore window after activation", 500));

    static auto renderStage = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT || !g_pOledSaver)
            return;

        if (g_dismissAfterActivity) {
            stopSaver();
            return;
        }

        const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (monitor && g_pOledSaver->pMonitor == monitor)
            g_pOledSaver->render();
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsaver", ::onDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsaverstart", ::onStartDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsaverstop", ::onStopDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsavertoggle", ::onToggleDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsaveractivate", ::onActivateDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsaverdeactivate", ::onDeactivateDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsaverpresent", ::onPresentDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyproledsaverdismiss", ::onDismissDispatcher);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "start", ::luaStart);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "stop", ::luaStop);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "toggle", ::luaToggle);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "activate", ::luaActivate);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "deactivate", ::luaDeactivate);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "present", ::luaPresent);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "dismiss", ::luaDismiss);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "set_active", ::luaSetActive);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "run", ::luaRun);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "enabled", ::luaEnabled);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "is_active", ::luaIsActive);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyproledsaver", "refresh", ::luaRefresh);
    HyprlandAPI::reloadConfig();

    HyprlandAPI::addNotification(PHANDLE, "[hypr-oled-saver] Initialized successfully",
                                 CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);
    return {"hypr-oled-saver", "An OLED-friendly Hyprland screensaver plugin", "Ivan Malison",
            "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pOledSaver.reset();
    g_pHyprRenderer->m_renderPass.removeAllOfType("COledSaverPassElement");
}
