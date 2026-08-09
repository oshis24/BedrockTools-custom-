#pragma once

#include "../Module.hpp"
#include <pl/Input.hpp>

class ZoomModule : public Module {
public:
    float m_defaultZoomFov  = 20.58f;
    float m_targetZoomFov   = 20.58f;
    float m_currentFov      = 90.0f;
    float m_baseFov         = 90.0f;
    float m_animSpeed       = 0.25f;
    bool  m_lowSens         = true;
    float m_lowSensStrength = 0.9f;
    bool  m_hideHand        = true;
    bool  m_overlayToggle   = true;

    // --- Posisi, Skala, & Opacity (Transparansi) ---
    float m_posX    = 60.0f;
    float m_posY    = 120.0f;
    float m_scale   = 1.0f;
    float m_opacity = 0.8f; // Default 80% Transparansi (0.0f = Transparan, 1.0f = Pekat)

    bool m_animationFinished = true;
    bool m_isFirstTime       = true;

    bool m_keyZooming    = false;
    bool m_buttonZooming = false;
    bool isZoomActive();

    // --- State Touch Drag Tracking (Single Finger) ---
    int   m_trackedPointerId = -1;
    float m_lastTouchY       = 0.0f;

    ZoomModule();
    ~ZoomModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // --- Render & Hit-Test Tombol Style Minecraft ---
    bool contains(float x, float y);
    void renderButton();

    // --- Fungsi Touch Drag & Scroll ---
    void updateDrag(float deltaY);
    void onScroll(float scrollDelta);
    bool onTouchEvent(const pl::input::TouchEvent& ev);

private:
    bool m_fovHooked       = false;
    bool m_turnDeltaHooked = false;
    bool m_hideHandHooked  = false;
    bool m_touchHooked     = false;
};
