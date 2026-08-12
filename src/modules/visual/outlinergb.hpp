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

    bool rgbCycle = false;

    float red = 1.0f;
    float green = 0.15f;
    float blue = 0.95f;
    float alpha = 1.0f;
};
