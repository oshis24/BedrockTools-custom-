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

    // Entity Culling configuration.
    bool cullPlayers = true;
    bool cullEntities = true;
    bool wallCulling = true;

    // Maximum horizontal view angle used by the reference
    // culling algorithm.
    float viewAngle = 180.0f;

    // Maximum entity distance.
    float cullDistance = 128.0f;

    // Percentage of nearest candidates retained after sorting.
    int playersVisiblePercent = 100;
    int entitiesVisiblePercent = 100;

private:
    void* m_actorManagerListTarget = nullptr;
    void* m_actorManagerListHook = nullptr;

    void installHooks();
    void removeHooks();
};
