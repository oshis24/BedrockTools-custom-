#pragma once

#include "../Module.hpp"

#include <nlohmann/json.hpp>

class ParticleOptimizerModule final : public Module {
public:
    ParticleOptimizerModule();
    ~ParticleOptimizerModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool noParticles = false;
    bool noFlipbook = false;
    bool noShadow = false;
    bool noWeather = false;
    bool noStars = false;
    bool noSunMoon = false;
};
