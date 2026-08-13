#pragma once

#include "../Module.hpp"
#include <nlohmann/json.hpp>

class OutlineRGBModule final : public Module {
public:
    OutlineRGBModule();
    ~OutlineRGBModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool rgbCycle = true;
    bool outline3D = true;

    float colorRed = 1.0f;
    float colorGreen = 0.0f;
    float colorBlue = 0.0f;

    float chromaSpeed = 1.0f;
};
