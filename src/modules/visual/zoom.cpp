#include "zoom.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Input.hpp>
#include <cmath>
#include <algorithm>
#include <array>
#include <chrono> // Diperlukan untuk Throttling Frame Render

static ZoomModule* g_zoomMod = nullptr;

// Helper untuk menghitung warna ARGB dengan Opacity dinamis
static inline uint32_t ApplyAlpha(uint32_t rgb, float opacity) {
    uint32_t a = static_cast<uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f) & 0xFF;
    return (a << 24) | (rgb & 0x00FFFFFF);
}

// =============================================================================
// FOV HOOK
// =============================================================================
static float (*_getFov_orig)(void*, float, int) = nullptr;

static float _getFov_zoom_hook(void* _this, float a, int enableVariableFOV) {
    float originalFov = 0.0f;
    if (_getFov_orig)
        originalFov = _getFov_orig(_this, a, enableVariableFOV);
    
    if (!g_zoomMod) return originalFov;

    // Render tombol overlay (Sudah di-throttle agar ringan)
    g_zoomMod->renderButton();

    // Abaikan FOV khusus UI / Hand / Inventory
    if (originalFov == 70.0f || originalFov == 60.0f) {
        return originalFov;
    }

    g_zoomMod->m_baseFov = originalFov;

    if (g_zoomMod->m_isFirstTime) {
        g_zoomMod->m_currentFov = originalFov;
        g_zoomMod->m_isFirstTime = false;
    }

    if (g_zoomMod->isZoomActive()) {
        g_zoomMod->m_animationFinished = false;
        g_zoomMod->m_currentFov = std::lerp(g_zoomMod->m_currentFov, g_zoomMod->m_targetZoomFov, g_zoomMod->m_animSpeed);
        return g_zoomMod->m_currentFov;
    } else {
        if (!g_zoomMod->m_animationFinished) {
            g_zoomMod->m_currentFov = std::lerp(g_zoomMod->m_currentFov, originalFov, g_zoomMod->m_animSpeed);
            if (std::abs(g_zoomMod->m_currentFov - originalFov) < 0.2f) {
                g_zoomMod->m_animationFinished = true;
                g_zoomMod->m_currentFov = originalFov;
            }
            return g_zoomMod->m_currentFov;
        }
    }

    return originalFov;
}

// =============================================================================
// CAMERA SENSITIVITY HOOK
// =============================================================================
struct Vec2 { float x, y; };
static void (*_applyTurnDelta_orig)(void*, Vec2*) = nullptr;

static void _applyTurnDelta_hook(void* _this, Vec2* rotationDelta) {
    if (g_zoomMod && (g_zoomMod->isZoomActive() || !g_zoomMod->m_animationFinished) && g_zoomMod->m_lowSens && g_zoomMod->m_baseFov > 0.1f) {
        float zoomRatio = g_zoomMod->m_currentFov / g_zoomMod->m_baseFov;
        float strength = g_zoomMod->m_lowSensStrength;
        float multiplier = 1.0f - (1.0f - zoomRatio) * strength;
        multiplier = std::clamp(multiplier, 0.01f, 1.0f);
        
        Vec2 modifiedDelta = { rotationDelta->x * multiplier, rotationDelta->y * multiplier };
        if (_applyTurnDelta_orig)
            _applyTurnDelta_orig(_this, &modifiedDelta);
    } else {
        if (_applyTurnDelta_orig)
            _applyTurnDelta_orig(_this, rotationDelta);
    }
}

// =============================================================================
// HIDE HAND HOOK
// =============================================================================
static bool (*_getHideItemInHand_orig)(void*) = nullptr;

static bool _getHideItemInHand_hook(void* _this) {
    bool hide = false;
    if (_getHideItemInHand_orig)
        hide = _getHideItemInHand_orig(_this);
    
    if (g_zoomMod && g_zoomMod->isZoomActive() && g_zoomMod->m_hideHand) {
        return true;
    }
    
    return hide;
}

// =============================================================================
// TOUCH INPUT BRIDGE
// =============================================================================
static bool _onTouchBridge(const pl::input::TouchEvent& ev) {
    if (g_zoomMod) {
        return g_zoomMod->onTouchEvent(ev);
    }
    return false;
}

// =============================================================================
// MODULE LIFECYCLE
// =============================================================================
ZoomModule::ZoomModule() 
    : Module("Zoom", "Smoothly zooms your camera like OptiFine.") {
    this->keybind = 0;
    g_zoomMod = this;
}

ZoomModule::~ZoomModule() {
    if (g_zoomMod == this) g_zoomMod = nullptr;
}

void ZoomModule::onInit() {
    if (!m_fovHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetFov);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_getFov_zoom_hook, (void**)&_getFov_orig);
            m_fovHooked = true;
        }
    }
    
    if (!m_turnDeltaHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerApplyTurnDelta);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_applyTurnDelta_hook, (void**)&_applyTurnDelta_orig);
            m_turnDeltaHooked = true;
        }
    }
    
    if (!m_hideHandHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseOptionRegistryGetHideItemInHand);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_getHideItemInHand_hook, (void**)&_getHideItemInHand_orig);
            m_hideHandHooked = true;
        }
    }

    if (!m_touchHooked) {
        pl::input::registerTouchCallback(_onTouchBridge);
        m_touchHooked = true;
    }
}

void ZoomModule::onEnable() {
    m_isFirstTime = true;
    m_animationFinished = false;
}

void ZoomModule::onDisable() {
    m_animationFinished = false;
    m_keyZooming = false;
    m_buttonZooming = false;
    m_trackedPointerId = -1;
}

bool ZoomModule::isZoomActive() {
    if (!enabled) return false;
    return m_keyZooming || m_buttonZooming;
}

// =============================================================================
// HIT-TESTING & HAND-DRAWN OVERLAY (OPTIMIZED & OPACITY CONTROLLED)
// =============================================================================
bool ZoomModule::contains(float px, float py) {
    float size = 52.0f * m_scale;
    return (px >= m_posX && px <= (m_posX + size) &&
            py >= m_posY && py <= (m_posY + size));
}

void ZoomModule::renderButton() {
    if (!enabled || !m_overlayToggle) return;

    // --- OPTIMASI UTAMA: FRAME-RATE THROTTLING (~60 FPS) ---
    // Mencegah lag akibat GetFov dipanggil puluhan kali dalam 1 frame
    static auto lastRenderTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRenderTime).count();
    
    if (elapsed < 14) { // Dibatasi maksimal ~70 FPS
        return;
    }
    lastRenderTime = now;

    float size = 52.0f * m_scale;
    float x = m_posX;
    float y = m_posY;
    bool active = isZoomActive();

    // Hitung Warna dengan Opacity
    float op = m_opacity;
    uint32_t colOuterBorder = ApplyAlpha(0x00373737, op);
    uint32_t colTopBevel    = ApplyAlpha(active ? 0x005B5B5B : 0x00C6C6C6, op);
    uint32_t colBottomBevel = ApplyAlpha(active ? 0x00373737 : 0x005B5B5B, op);
    uint32_t colFill        = ApplyAlpha(active ? 0x003C3C3C : 0x008B8B8B, op);
    uint32_t colText        = ApplyAlpha(active ? 0x0055FF55 : 0x00FFFFFF, op);
    uint32_t colTextShadow  = ApplyAlpha(0x001E1E1E, op);

    float b = 2.0f * m_scale;

    std::array<pl::modmenu::DrawCommand, 6> cmds{};

    // 1. Outer Border
    cmds[0].type = pl::modmenu::DrawCommandType::RectFilled;
    cmds[0].x = x; cmds[0].y = y; 
    cmds[0].w = size; cmds[0].h = size;
    cmds[0].color = colOuterBorder;

    // 2. Bevel Atas & Kiri
    cmds[1].type = pl::modmenu::DrawCommandType::RectFilled;
    cmds[1].x = x + b; cmds[1].y = y + b; 
    cmds[1].w = size - (2.0f * b); cmds[1].h = size - (2.0f * b);
    cmds[1].color = colTopBevel;

    // 3. Bevel Bawah & Kanan
    cmds[2].type = pl::modmenu::DrawCommandType::RectFilled;
    cmds[2].x = x + (2.0f * b); cmds[2].y = y + (2.0f * b); 
    cmds[2].w = size - (3.0f * b); cmds[2].h = size - (3.0f * b);
    cmds[2].color = colBottomBevel;

    // 4. Inner Fill
    cmds[3].type = pl::modmenu::DrawCommandType::RectFilled;
    cmds[3].x = x + (2.0f * b); cmds[3].y = y + (2.0f * b); 
    cmds[3].w = size - (4.0f * b); cmds[3].h = size - (4.0f * b);
    cmds[3].color = colFill;

    // Posisi Teks
    float fontSize = 16.0f * m_scale;
    float textX = x + (size * 0.5f) - (fontSize * 0.55f);
    float textY = y + (size * 0.5f) - (fontSize * 0.50f);

    // 5. Bayangan Teks
    cmds[4].type = pl::modmenu::DrawCommandType::Text;
    cmds[4].x = textX + (1.2f * m_scale); 
    cmds[4].y = textY + (1.2f * m_scale);
    cmds[4].text = "ZM"; 
    cmds[4].color = colTextShadow; 
    cmds[4].size = fontSize;

    // 6. Teks Utama
    cmds[5].type = pl::modmenu::DrawCommandType::Text;
    cmds[5].x = textX; 
    cmds[5].y = textY;
    cmds[5].text = "ZM"; 
    cmds[5].color = colText; 
    cmds[5].size = fontSize;

    pl::modmenu::submitDrawCommands("bedrocktools.Zoom", cmds);
}

// =============================================================================
// SINGLE-FINGER TOUCH CONTROL
// =============================================================================
void ZoomModule::updateDrag(float deltaY) {
    if (!isZoomActive()) return;
    
    float change = deltaY * 0.08f;
    float minLimit = 3.0f;
    float maxLimit = std::max(minLimit + 5.0f, m_baseFov - 5.0f);
    
    m_targetZoomFov = std::clamp(m_targetZoomFov + change, minLimit, maxLimit);
}

void ZoomModule::onScroll(float scrollDelta) {
    if (!isZoomActive()) return;
    float change = -scrollDelta * 2.5f;
    float minLimit = 3.0f;
    float maxLimit = std::max(minLimit + 5.0f, m_baseFov - 5.0f);
    
    m_targetZoomFov = std::clamp(m_targetZoomFov + change, minLimit, maxLimit);
}

bool ZoomModule::onTouchEvent(const pl::input::TouchEvent& ev) {
    if (!enabled || !m_overlayToggle) return false;

    constexpr int kActionMask        = 0xFF;
    constexpr int kActionDown        = 0;
    constexpr int kActionUp          = 1;
    constexpr int kActionMove        = 2;
    constexpr int kActionCancel      = 3;
    constexpr int kActionPointerDown = 5;
    constexpr int kActionPointerUp   = 6;

    int action = ev.action & kActionMask;

    switch (action) {
        case kActionDown:
        case kActionPointerDown:
            if (m_trackedPointerId == -1 && contains(ev.x, ev.y)) {
                m_trackedPointerId = ev.pointerId;
                m_lastTouchY = ev.y;
                m_buttonZooming = true;
                m_isFirstTime = true;
                m_animationFinished = false;
                m_targetZoomFov = m_defaultZoomFov;
                return true; // KONSUMSI EVENT
            }
            break;

        case kActionMove:
            if (m_trackedPointerId != -1 && ev.pointerId == m_trackedPointerId) {
                float deltaY = ev.y - m_lastTouchY;
                m_lastTouchY = ev.y;
                updateDrag(deltaY);
                return true;
            }
            break;

        case kActionUp:
        case kActionPointerUp:
        case kActionCancel:
            if (ev.pointerId == m_trackedPointerId) {
                m_trackedPointerId = -1;
                m_buttonZooming = false;
                return true;
            }
            break;
    }

    return false;
}

void ZoomModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (key == "keybind") {
        if (isDown && !m_keyZooming) {
            m_isFirstTime = true;
            m_animationFinished = false;
            m_targetZoomFov = m_defaultZoomFov;
        }
        m_keyZooming = isDown;
    }
}

void ZoomModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_defaultZoomFov"))  m_defaultZoomFov  = j["m_defaultZoomFov"].get<float>();
    if (j.contains("m_targetZoomFov"))   m_targetZoomFov   = j["m_targetZoomFov"].get<float>();
    if (j.contains("m_animSpeed"))       m_animSpeed       = j["m_animSpeed"].get<float>();
    if (j.contains("m_lowSens"))         m_lowSens         = j["m_lowSens"].get<bool>();
    if (j.contains("m_lowSensStrength")) m_lowSensStrength = j["m_lowSensStrength"].get<float>();
    if (j.contains("m_hideHand"))        m_hideHand        = j["m_hideHand"].get<bool>();
    if (j.contains("m_overlayToggle"))   m_overlayToggle   = j["m_overlayToggle"].get<bool>();
    if (j.contains("m_posX"))            m_posX            = j["m_posX"].get<float>();
    if (j.contains("m_posY"))            m_posY            = j["m_posY"].get<float>();
    if (j.contains("m_scale"))           m_scale           = j["m_scale"].get<float>();
    if (j.contains("m_opacity"))         m_opacity         = j["m_opacity"].get<float>();
}

void ZoomModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_defaultZoomFov"]  = m_defaultZoomFov;
    j["m_targetZoomFov"]   = m_targetZoomFov;
    j["m_animSpeed"]       = m_animSpeed;
    j["m_lowSens"]         = m_lowSens;
    j["m_lowSensStrength"] = m_lowSensStrength;
    j["m_hideHand"]        = m_hideHand;
    j["m_overlayToggle"]   = m_overlayToggle;
    j["m_posX"]            = m_posX;
    j["m_posY"]            = m_posY;
    j["m_scale"]           = m_scale;
    j["m_opacity"]         = m_opacity;
}
