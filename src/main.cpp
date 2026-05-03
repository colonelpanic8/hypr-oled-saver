#define WLR_USE_UNSTABLE

#include <algorithm>
#include <any>
#include <array>
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

static const CConfigValue<Config::FLOAT> &POPACITY() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:opacity");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PMAXVISIBLE() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:max_visible");
    return VALUE;
}

static const CConfigValue<Config::FLOAT> &PTARGETWINDOWAREA() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:target_window_area");
    return VALUE;
}

static const CConfigValue<Config::FLOAT> &PMINWINDOWSCALE() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:min_window_scale");
    return VALUE;
}

static const CConfigValue<Config::FLOAT> &PMAXWINDOWSCALE() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:max_window_scale");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PPATHDURATIONMIN() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:path_duration_ms_min");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PPATHDURATIONMAX() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:path_duration_ms_max");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PSTARTSTAGGERMIN() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:start_stagger_ms_min");
    return VALUE;
}

static const CConfigValue<Config::INTEGER> &PSTARTSTAGGERMAX() {
    static const CConfigValue<Config::INTEGER> VALUE("plugin:hyproledsaver:start_stagger_ms_max");
    return VALUE;
}

static const CConfigValue<Config::FLOAT> &PCURVESTRENGTH() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:curve_strength");
    return VALUE;
}

static const CConfigValue<Config::FLOAT> &PPATHACCELPOWER() {
    static const CConfigValue<Config::FLOAT> VALUE("plugin:hyproledsaver:path_accel_power");
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

static uint64_t mixHash(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static double randomUnit(uint64_t seed, uint64_t salt) {
    return static_cast<double>(mixHash(seed + salt * 0x9e3779b97f4a7c15ULL) >> 11) *
           (1.0 / 9007199254740992.0);
}

static double randomRange(uint64_t seed, uint64_t salt, double min, double max) {
    return min + (max - min) * randomUnit(seed, salt);
}

static int randomRangeInt(uint64_t seed, uint64_t salt, int min, int max) {
    if (max <= min)
        return min;
    return min + (int)std::round(randomUnit(seed, salt) * (max - min));
}

static double smoothstep(double edge0, double edge1, double value) {
    const double t = std::clamp((value - edge0) / std::max(0.0001, edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

static double distance(const Vector2D &a, const Vector2D &b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
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

struct SPreview {
    PHLWINDOW window;
    SP<Render::IFramebuffer> fb;
    CBox box;
    int slot = -1;
    bool visible = false;
    std::chrono::steady_clock::time_point pathStartedAt;
    int durationMs = 1;
    double opacity = 0.0;

    struct SPath {
        Vector2D p0;
        Vector2D p1;
        Vector2D p2;
        Vector2D p3;
        std::array<double, 49> cumulative = {};
    } path;
};

class COledSaver {
  public:
    explicit COledSaver(PHLMONITOR monitor) : pMonitor(monitor) {
        activatedAt = Time::steadyNow();
        pathGeneration = mixHash((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     activatedAt.time_since_epoch())
                                     .count());
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

        updatePaths();

        CRegion fullDamage = {0, 0, INT16_MAX, INT16_MAX};
        Render::GL::g_pHyprOpenGL->renderRect(CBox{{0, 0}, pMonitor->m_pixelSize},
                                              CHyprColor(*PBACKGROUND()), {.damage = &fullDamage});

        const double scale = pMonitor->m_scale;
        const int border = std::max<Config::INTEGER>(0, *PBORDERSIZE());

        for (auto &preview : previews) {
            if (!preview.visible || preview.opacity <= 0.001 || !preview.window || !preview.fb ||
                !preview.fb->getTexture())
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
                {.damage = &fullDamage, .a = (float)preview.opacity, .round = border * 2});
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
        renderSnapshots();
        initializePaths();
    }

    void initializePaths() {
        if (!pMonitor || previews.empty())
            return;

        nextPreviewIndex =
            (size_t)std::floor(randomRange(pathGeneration, 0, 0.0, (double)previews.size()));
        const int visibleCount =
            std::min<int>(std::max<Config::INTEGER>(1, *PMAXVISIBLE()), previews.size());

        for (int slot = 0; slot < visibleCount; ++slot)
            scheduleNextPreview(slot, Time::steadyNow(), initialDelayForSlot(slot));
    }

    void updatePaths() {
        if (!pMonitor || previews.empty())
            return;

        const auto now = Time::steadyNow();
        std::vector<int> finishedSlots;

        for (auto &preview : previews) {
            if (!preview.visible)
                continue;

            const double localMs = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now - preview.pathStartedAt)
                                       .count();

            if (localMs < 0.0) {
                preview.opacity = 0.0;
                continue;
            }

            const double progress = localMs / std::max(1.0, (double)preview.durationMs);
            if (progress >= 1.0) {
                finishedSlots.push_back(preview.slot);
                preview.visible = false;
                preview.slot = -1;
                preview.opacity = 0.0;
                continue;
            }

            const double distanceProgress = std::pow(
                std::clamp(progress, 0.0, 1.0), std::clamp<double>(*PPATHACCELPOWER(), 0.25, 4.0));
            const double pathT = parameterAtDistance(preview.path, distanceProgress);
            const auto center = cubicBezier(preview.path, pathT);
            preview.box.x = center.x - preview.box.w / 2.0;
            preview.box.y = center.y - preview.box.h / 2.0;
            preview.opacity = opacityForProgress(progress);
        }

        for (const auto slot : finishedSlots)
            scheduleNextPreview(slot, now, nextDelayForSlot(slot));
    }

    double opacityForProgress(double progress) const {
        const double fadeIn = smoothstep(0.04, 0.24, progress);
        const double fadeOut = 1.0 - smoothstep(0.68, 0.96, progress);
        return std::clamp<double>(*POPACITY(), 0.0, 1.0) * fadeIn * fadeOut;
    }

    int initialDelayForSlot(int slot) const {
        if (slot == 0)
            return 0;

        return nextDelayForSlot(slot);
    }

    int nextDelayForSlot(int slot) const {
        const auto seed = mixHash(pathGeneration ^ hashString("delay:" + std::to_string(slot)));
        const int minDelay = std::max<Config::INTEGER>(0, *PSTARTSTAGGERMIN());
        const int maxDelay = std::max<Config::INTEGER>(minDelay, *PSTARTSTAGGERMAX());
        return randomRangeInt(seed, 0, minDelay, maxDelay);
    }

    void scheduleNextPreview(int slot, std::chrono::steady_clock::time_point now, int delayMs) {
        if (previews.empty())
            return;

        const auto index = nextInactivePreviewIndex();
        auto &preview = previews[index];
        preview.visible = true;
        preview.slot = slot;
        preview.opacity = 0.0;
        preview.durationMs = pathDurationFor(index, slot);
        preview.pathStartedAt = now + std::chrono::milliseconds(delayMs);
        preview.box = sizedBoxForWindow(preview.window);
        preview.path = pathFor(preview, index, slot);
        buildArcLength(preview.path);
    }

    size_t nextInactivePreviewIndex() {
        for (size_t attempts = 0; attempts < previews.size(); ++attempts) {
            const size_t index = nextPreviewIndex % previews.size();
            nextPreviewIndex = (nextPreviewIndex + 1) % previews.size();
            if (!previews[index].visible)
                return index;
        }

        const size_t index = nextPreviewIndex % previews.size();
        nextPreviewIndex = (nextPreviewIndex + 1) % previews.size();
        return index;
    }

    CBox sizedBoxForWindow(const PHLWINDOW &window) const {
        const auto winSize = window->m_realSize->value();
        const double monitorArea = std::max(1.0, pMonitor->m_size.x * pMonitor->m_size.y);
        const double targetArea = monitorArea * std::clamp<double>(*PTARGETWINDOWAREA(), 0.04, 0.9);
        const double rawScale = std::sqrt(targetArea / std::max(1.0, winSize.x * winSize.y));
        const double minScale = std::clamp<double>(*PMINWINDOWSCALE(), 0.05, 4.0);
        const double maxScale =
            std::max(minScale, std::clamp<double>(*PMAXWINDOWSCALE(), 0.05, 4.0));
        const double windowScale = std::clamp(rawScale, minScale, maxScale);
        const double w = std::max(1.0, winSize.x * windowScale);
        const double h = std::max(1.0, winSize.y * windowScale);
        return {0, 0, w, h};
    }

    uint64_t seedFor(size_t index) const {
        const auto &preview = previews[index];
        const auto klass = preview.window ? preview.window->m_class : "";
        return hashString(std::to_string(index) + ":" + klass);
    }

    int pathDurationFor(size_t index, int slot) const {
        const int minDuration = std::max<Config::INTEGER>(1000, *PPATHDURATIONMIN());
        const int maxDuration = std::max<Config::INTEGER>(minDuration, *PPATHDURATIONMAX());
        return randomRangeInt(pathSeed(index, slot), 1, minDuration, maxDuration);
    }

    uint64_t pathSeed(size_t index, int slot) const {
        return mixHash(seedFor(index) ^ ((uint64_t)slot << 32) ^ pathGeneration);
    }

    SPreview::SPath pathFor(const SPreview &preview, size_t index, int slot) {
        const auto seed = pathSeed(index, slot);
        const bool leftToRight = slot % 2 == 0;
        const auto size = pMonitor->m_size;
        const double halfW = preview.box.w / 2.0;
        const double halfH = preview.box.h / 2.0;
        const double startX = leftToRight ? -halfW * 0.55 : size.x + halfW * 0.55;
        const double endX = leftToRight ? size.x + halfW * 0.55 : -halfW * 0.55;
        const double minY = std::clamp(halfH * 0.85, 0.0, size.y);
        const double maxY = std::clamp(size.y - halfH * 0.85, minY, size.y);
        const double startY = randomRange(seed, 2, minY, maxY);
        const double endY = randomRange(seed, 3, minY, maxY);

        SPreview::SPath path;
        path.p0 = {startX, startY};
        path.p3 = {endX, endY};

        const double length = std::max(1.0, distance(path.p0, path.p3));
        const Vector2D direction = {(path.p3.x - path.p0.x) / length,
                                    (path.p3.y - path.p0.y) / length};
        const Vector2D normal = {-direction.y, direction.x};
        const double bendSign = slot % 2 == 0 ? 1.0 : -1.0;
        const double strength = std::clamp<double>(*PCURVESTRENGTH(), 0.02, 0.8) *
                                std::min(length, std::max(size.x, size.y));
        const double bend1 = bendSign * randomRange(seed, 4, 0.45, 1.0) * strength;
        const double bend2Sign = randomUnit(seed, 5) < 0.75 ? bendSign : -bendSign;
        const double bend2 = bend2Sign * randomRange(seed, 6, 0.45, 1.0) * strength;
        const double p1Distance = randomRange(seed, 7, 0.22, 0.38);
        const double p2Distance = randomRange(seed, 8, 0.62, 0.82);

        path.p1 = {path.p0.x + direction.x * length * p1Distance + normal.x * bend1,
                   path.p0.y + direction.y * length * p1Distance + normal.y * bend1};
        path.p2 = {path.p0.x + direction.x * length * p2Distance + normal.x * bend2,
                   path.p0.y + direction.y * length * p2Distance + normal.y * bend2};

        pathGeneration = mixHash(pathGeneration + 1);
        return path;
    }

    static Vector2D cubicBezier(const SPreview::SPath &path, double t) {
        const double inv = 1.0 - t;
        const double b0 = inv * inv * inv;
        const double b1 = 3.0 * inv * inv * t;
        const double b2 = 3.0 * inv * t * t;
        const double b3 = t * t * t;
        return {path.p0.x * b0 + path.p1.x * b1 + path.p2.x * b2 + path.p3.x * b3,
                path.p0.y * b0 + path.p1.y * b1 + path.p2.y * b2 + path.p3.y * b3};
    }

    static void buildArcLength(SPreview::SPath &path) {
        path.cumulative[0] = 0.0;
        Vector2D previous = path.p0;
        double total = 0.0;

        for (size_t i = 1; i < path.cumulative.size(); ++i) {
            const double t = (double)i / (double)(path.cumulative.size() - 1);
            const auto current = cubicBezier(path, t);
            total += distance(previous, current);
            path.cumulative[i] = total;
            previous = current;
        }

        if (total <= 0.0)
            return;

        for (auto &value : path.cumulative)
            value /= total;
    }

    static double parameterAtDistance(const SPreview::SPath &path, double distanceProgress) {
        const double target = std::clamp(distanceProgress, 0.0, 1.0);
        const auto upper = std::ranges::lower_bound(path.cumulative, target);
        if (upper == path.cumulative.begin())
            return 0.0;
        if (upper == path.cumulative.end())
            return 1.0;

        const size_t upperIndex = upper - path.cumulative.begin();
        const size_t lowerIndex = upperIndex - 1;
        const double lowerValue = path.cumulative[lowerIndex];
        const double upperValue = path.cumulative[upperIndex];
        const double local = (target - lowerValue) / std::max(0.000001, upperValue - lowerValue);
        const double lowerT = (double)lowerIndex / (double)(path.cumulative.size() - 1);
        const double upperT = (double)upperIndex / (double)(path.cumulative.size() - 1);
        return lowerT + (upperT - lowerT) * local;
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

    std::vector<SPreview> previews;
    std::chrono::steady_clock::time_point activatedAt;
    size_t nextPreviewIndex = 0;
    uint64_t pathGeneration = 1;
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

    g_dismissAfterActivity = false;
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

    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyproledsaver:background",
                                                           "background color", 0xFF000000));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyproledsaver:border_color",
                                                           "preview border color", 0x2246C7D8));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyproledsaver:border_size",
                                                         "preview border size", 2));
    addConfigValue(makeShared<Config::Values::CFloatValue>(
        "plugin:hyproledsaver:opacity", "maximum preview texture opacity", 0.34F));
    addConfigValue(makeShared<Config::Values::CIntValue>(
        "plugin:hyproledsaver:max_visible", "maximum simultaneously visible previews", 2));
    addConfigValue(makeShared<Config::Values::CFloatValue>(
        "plugin:hyproledsaver:target_window_area", "target fraction of monitor area per preview",
        0.34F));
    addConfigValue(makeShared<Config::Values::CFloatValue>(
        "plugin:hyproledsaver:min_window_scale", "minimum scale relative to real window size",
        0.55F));
    addConfigValue(makeShared<Config::Values::CFloatValue>(
        "plugin:hyproledsaver:max_window_scale", "maximum scale relative to real window size",
        0.94F));
    addConfigValue(makeShared<Config::Values::CIntValue>(
        "plugin:hyproledsaver:path_duration_ms_min", "minimum Bezier path duration", 35000));
    addConfigValue(makeShared<Config::Values::CIntValue>(
        "plugin:hyproledsaver:path_duration_ms_max", "maximum Bezier path duration", 70000));
    addConfigValue(
        makeShared<Config::Values::CIntValue>("plugin:hyproledsaver:start_stagger_ms_min",
                                              "minimum stagger between preview starts", 2500));
    addConfigValue(
        makeShared<Config::Values::CIntValue>("plugin:hyproledsaver:start_stagger_ms_max",
                                              "maximum stagger between preview starts", 9000));
    addConfigValue(makeShared<Config::Values::CFloatValue>(
        "plugin:hyproledsaver:curve_strength", "Bezier control-point bend strength", 0.28F));
    addConfigValue(makeShared<Config::Values::CFloatValue>(
        "plugin:hyproledsaver:path_accel_power", "Bezier path acceleration power", 1.35F));
    addConfigValue(makeShared<Config::Values::CIntValue>(
        "plugin:hyproledsaver:dismiss_on_activity", "dismiss when input activity is detected", 1));
    addConfigValue(makeShared<Config::Values::CIntValue>(
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
