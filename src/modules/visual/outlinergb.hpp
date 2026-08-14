#pragma once

#include "../Module.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>

class OutlineRGBModule final : public Module {
public:
    OutlineRGBModule();
    ~OutlineRGBModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool rgbCycle = true;

    float red = 1.0f;
    float green = 0.15f;
    float blue = 0.95f;
    float alpha = 1.0f;

    float rgbSpeed = 0.25f;

    /*
     * 1 = single line
     * 2 = 3 parallel lines
     * 3 = 5 parallel lines
     * ...
     *
     * This is a geometry-based thickness simulation because
     * GLES line width is unreliable on Android/Mali.
     */
    int thickness = 1;

private:
    bool installHooks();

    bool m_patched = false;
    void* m_patchTarget = nullptr;

public:
    static OutlineRGBModule* instance;
};
