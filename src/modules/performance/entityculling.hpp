#pragma once

#include "../Module.hpp"

#include <nlohmann/json.hpp>

class EntityCullingModule final : public Module {
public:
    EntityCullingModule();
    ~EntityCullingModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool cullPlayers = true;
    bool cullEntities = true;
    bool wallCulling = true;

    float viewAngle = 120.0f;
    float cullDistance = 64.0f;

    int playersVisiblePercent = 100;
    int entitiesVisiblePercent = 50;

private:
    void* m_actorManagerListTarget = nullptr;

    void installHooks();
    void removeHooks();
};
