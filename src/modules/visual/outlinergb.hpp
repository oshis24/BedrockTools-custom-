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

    bool blockOutline = true;
    bool entityOutline = true;
    bool outline3D = true;
    bool rgbCycle = true;

    float red = 1.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;

    float chromaSpeed = 1.0f;
    float thickness = 1.0f;
};
