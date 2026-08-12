#include "entityculling.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using ActorManagerListFn =
    std::vector<void*> (*)(void*);

using ActorIsPlayerFn =
    bool (*)(void*);

using ActorIsInvisibleFn =
    bool (*)(void*);

using BlockSourceIsSolidBlockingBlockFn =
    bool (*)(void*, const bedrocktools::sdk::BlockPos*);


struct Candidate {
    void* actor = nullptr;
    float distanceSq = 0.0f;
};


EntityCullingModule* g_module = nullptr;

ActorManagerListFn g_actorManagerListOriginal = nullptr;

ActorIsPlayerFn g_actorIsPlayer = nullptr;

ActorIsInvisibleFn g_actorIsInvisible = nullptr;

BlockSourceIsSolidBlockingBlockFn
    g_isSolidBlockingBlock = nullptr;

bedrocktools::hooks::Handle
    g_actorManagerListHook = nullptr;


/*
 * Normalize angle into [-180, 180].
 */
float normalizeDegrees(float angle) {
    angle = std::fmod(angle, 360.0f);

    if (angle > 180.0f)
        angle -= 360.0f;

    if (angle < -180.0f)
        angle += 360.0f;

    return angle;
}


/*
 * Horizontal FOV test.
 */
bool insideViewAngle(
    float dx,
    float dz,
    float yaw,
    float angle
) {
    if (angle >= 360.0f)
        return true;

    if (angle <= 0.0f)
        return false;

    constexpr float RAD_TO_DEG =
        57.29577951308232f;

    const float direction =
        std::atan2(dz, dx) * RAD_TO_DEG;

    const float relative =
        normalizeDegrees(
            direction - (yaw + 90.0f)
        );

    return std::fabs(relative) <=
           angle * 0.5f;
}


/*
 * Wall visibility test.
 *
 * Same basic sampling model found during RE:
 *
 *     sampleCount = distance * 2
 *
 * clamped to [2, 256].
 */
bool blockedByWall(
    bedrocktools::sdk::BlockSource* blockSource,
    const bedrocktools::sdk::Vec3& camera,
    const bedrocktools::sdk::Vec3& target
) {
    if (!blockSource ||
        !g_isSolidBlockingBlock) {
        return false;
    }

    const float dx =
        target.x - camera.x;

    const float dy =
        target.y - camera.y;

    const float dz =
        target.z - camera.z;

    const float distanceSq =
        dx * dx +
        dy * dy +
        dz * dz;

    const float distance =
        std::sqrt(distanceSq);

    int sampleCount =
        static_cast<int>(
            distance * 2.0f
        );

    sampleCount =
        std::clamp(
            sampleCount,
            2,
            256
        );

    const float step =
        1.0f /
        static_cast<float>(sampleCount);

    for (int i = 1;
         i < sampleCount;
         ++i) {

        const float t =
            static_cast<float>(i) *
            step;

        bedrocktools::sdk::BlockPos blockPos{
            static_cast<int>(
                std::floor(
                    camera.x + dx * t
                )
            ),

            static_cast<int>(
                std::floor(
                    camera.y + dy * t
                )
            ),

            static_cast<int>(
                std::floor(
                    camera.z + dz * t
                )
            )
        };

        if (g_isSolidBlockingBlock(
                blockSource,
                &blockPos
            )) {
            return true;
        }
    }

    return false;
}


/*
 * Keep the requested percentage of candidates.
 *
 * Nearest entities are kept.
 */
void trimCandidates(
    std::vector<Candidate>& candidates,
    int percent
) {
    percent =
        std::clamp(
            percent,
            0,
            100
        );

    if (candidates.empty())
        return;

    if (percent >= 100)
        return;

    if (percent <= 0) {
        candidates.clear();
        return;
    }

    const std::size_t count =
        candidates.size();

    const std::size_t keep =
        std::max<std::size_t>(
            1,
            (
                count *
                static_cast<std::size_t>(percent) +
                99u
            ) / 100u
        );

    if (keep >= count)
        return;

    /*
     * Find the nearest 'keep' elements without
     * completely sorting the original vector.
     */
    auto middle =
        candidates.begin() +
        static_cast<std::ptrdiff_t>(keep);

    std::nth_element(
        candidates.begin(),
        middle,
        candidates.end(),
        [](const Candidate& a,
           const Candidate& b) {
            return a.distanceSq <
                   b.distanceSq;
        }
    );

    candidates.resize(keep);

    /*
     * Sort only the candidates that survived.
     */
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& a,
           const Candidate& b) {
            return a.distanceSq <
                   b.distanceSq;
        }
    );
}


/*
 * ActorManagerList hook.
 */
std::vector<void*> entityCullingHook(
    void* actorManager
) {
    if (!g_actorManagerListOriginal)
        return {};

    /*
     * Always obtain the original Actor list first.
     */
    const std::vector<void*> actors =
        g_actorManagerListOriginal(
            actorManager
        );

    /*
     * If the module is disabled, do absolutely
     * nothing extra.
     */
    if (!g_module ||
        !g_module->enabled) {
        return actors;
    }

    if (actors.empty())
        return actors;

    /*
     * If both categories are disabled, return
     * the original list immediately.
     */
    if (!g_module->cullPlayers &&
        !g_module->cullEntities) {
        return actors;
    }


    /*
     * Get Minecraft ClientInstance.
     */
    auto* client =
        bedrocktools::sdk::ClientInstance::current();

    if (!client)
        return actors;


    /*
     * Get local player.
     */
    auto* localPlayer =
        client->localPlayer();

    if (!localPlayer)
        return actors;


    /*
     * Camera position.
     *
     * The RE reference obtains the position from
     * the player/Actor path.
     */
    const bedrocktools::sdk::Vec3 camera =
        localPlayer->position();


    /*
     * Camera rotation.
     */
    const bedrocktools::sdk::Vec2 rotation =
        localPlayer->rotation();

    const float cameraYaw =
        rotation.y;


    /*
     * Maximum culling distance.
     */
    const float maxDistance =
        std::max(
            0.0f,
            g_module->cullDistance
        );

    const float maxDistanceSq =
        maxDistance *
        maxDistance;


    /*
     * Obtain BlockSource for optional wall culling.
     */
    bedrocktools::sdk::BlockSource*
        blockSource = nullptr;

    if (auto* dimension =
            localPlayer->dimension()) {

        blockSource =
            dimension->blockSource();
    }


    /*
     * Actors that bypass culling.
     */
    std::vector<void*> result;


    /*
     * Candidates that are subject to percentage
     * culling.
     */
    std::vector<Candidate> players;
    std::vector<Candidate> entities;


    /*
     * IMPORTANT:
     *
     * We deliberately do NOT do:
     *
     *     players.reserve(actors.size());
     *     entities.reserve(actors.size());
     *
     * The reference implementation only grows the
     * candidate vectors when required.
     */
    result.reserve(
        actors.size()
    );


    /*
     * SINGLE PASS through the original Actor list.
     */
    for (void* actor : actors) {

        if (!actor)
            continue;


        /*
         * Never cull the local player.
         */
        if (actor == localPlayer) {
            result.push_back(actor);
            continue;
        }


        /*
         * Determine Actor category.
         */
        const bool isPlayer =
            g_actorIsPlayer
                ? g_actorIsPlayer(actor)
                : false;


        /*
         * Determine whether this category should
         * be processed by Entity Culling.
         */
        const bool shouldCull =
            isPlayer
                ? g_module->cullPlayers
                : g_module->cullEntities;


        /*
         * Category disabled:
         * preserve Actor immediately.
         */
        if (!shouldCull) {
            result.push_back(actor);
            continue;
        }


        /*
         * IMPORTANT:
         *
         * Invisible actors are NOT removed.
         *
         * They simply bypass our additional culling.
         */
        if (g_actorIsInvisible &&
            g_actorIsInvisible(actor)) {

            result.push_back(actor);
            continue;
        }


        /*
         * Convert to SDK Actor.
         */
        auto* nativeActor =
            reinterpret_cast<
                bedrocktools::sdk::Actor*
            >(actor);


        /*
         * Obtain Actor bounds.
         */
        const bedrocktools::sdk::AABB bounds =
            nativeActor->bounds();


        /*
         * Calculate bounds center.
         */
        const bedrocktools::sdk::Vec3 center{
            (
                bounds.min.x +
                bounds.max.x
            ) * 0.5f,

            (
                bounds.min.y +
                bounds.max.y
            ) * 0.5f,

            (
                bounds.min.z +
                bounds.max.z
            ) * 0.5f
        };


        /*
         * Camera -> Actor vector.
         */
        const float dx =
            center.x - camera.x;

        const float dy =
            center.y - camera.y;

        const float dz =
            center.z - camera.z;


        /*
         * Distance squared.
         */
        const float distanceSq =
            dx * dx +
            dy * dy +
            dz * dz;


        /*
         * Distance culling.
         */
        if (distanceSq >
            maxDistanceSq) {

            continue;
        }


        /*
         * Horizontal FOV culling.
         */
        if (!insideViewAngle(
                dx,
                dz,
                cameraYaw,
                g_module->viewAngle
            )) {

            continue;
        }


        /*
         * Optional wall culling.
         */
        if (g_module->wallCulling &&
            blockSource &&
            g_isSolidBlockingBlock) {

            if (blockedByWall(
                    blockSource,
                    camera,
                    center
                )) {

                continue;
            }
        }


        /*
         * Actor survived all visibility tests.
         */
        Candidate candidate{
            actor,
            distanceSq
        };


        /*
         * Keep players and entities in separate
         * candidate lists because they have separate
         * visibility percentages.
         */
        if (isPlayer) {

            players.push_back(
                candidate
            );

        } else {

            entities.push_back(
                candidate
            );
        }
    }


    /*
     * Apply player percentage.
     */
    trimCandidates(
        players,
        g_module->playersVisiblePercent
    );


    /*
     * Apply entity percentage.
     */
    trimCandidates(
        entities,
        g_module->entitiesVisiblePercent
    );


    /*
     * Append surviving players.
     *
     * IMPORTANT:
     * There is NO second scan through the original
     * Actor list here.
     */
    for (const Candidate& candidate :
         players) {

        if (candidate.actor) {

            result.push_back(
                candidate.actor
            );
        }
    }


    /*
     * Append surviving entities.
     */
    for (const Candidate& candidate :
         entities) {

        if (candidate.actor) {

            result.push_back(
                candidate.actor
            );
        }
    }


    return result;
}

} // namespace


EntityCullingModule::EntityCullingModule()
    : Module(
        "Entity Culling",
        "Cull distant, off-angle, and occluded entities."
    ) {

    g_module = this;

    hideInHudEditor = true;
}


EntityCullingModule::~EntityCullingModule() {

    removeHooks();

    if (g_module == this)
        g_module = nullptr;
}


void EntityCullingModule::onInit() {

    /*
     * ActorManagerList.
     */
    m_actorManagerListTarget =
        reinterpret_cast<void*>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    ActorManagerList
            )
        );


    /*
     * ActorIsPlayer.
     */
    g_actorIsPlayer =
        reinterpret_cast<ActorIsPlayerFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    ActorIsPlayer
            )
        );


    /*
     * ActorIsInvisible.
     */
    g_actorIsInvisible =
        reinterpret_cast<ActorIsInvisibleFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    ActorIsInvisible
            )
        );


    /*
     * BlockSourceIsSolidBlockingBlock.
     */
    g_isSolidBlockingBlock =
        reinterpret_cast<
            BlockSourceIsSolidBlockingBlockFn
        >(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    BlockSourceIsSolidBlockingBlock
            )
        );
}


void EntityCullingModule::installHooks() {

    if (g_actorManagerListHook)
        return;

    if (!m_actorManagerListTarget)
        return;


    g_actorManagerListHook =
        bedrocktools::hooks::install(
            m_actorManagerListTarget,
            reinterpret_cast<void*>(
                entityCullingHook
            ),
            reinterpret_cast<void**>(
                &g_actorManagerListOriginal
            )
        );
}


void EntityCullingModule::removeHooks() {

    if (!g_actorManagerListHook)
        return;


    bedrocktools::hooks::remove(
        g_actorManagerListHook
    );


    g_actorManagerListHook = nullptr;

    g_actorManagerListOriginal = nullptr;
}


void EntityCullingModule::onEnable() {

    installHooks();
}


void EntityCullingModule::onDisable() {

    removeHooks();
}


void EntityCullingModule::loadConfig(
    const nlohmann::json& j
) {
    Module::loadConfig(j);


    if (j.contains("cullPlayers")) {

        cullPlayers =
            j["cullPlayers"].get<bool>();
    }


    if (j.contains("cullEntities")) {

        cullEntities =
            j["cullEntities"].get<bool>();
    }


    if (j.contains("wallCulling")) {

        wallCulling =
            j["wallCulling"].get<bool>();
    }


    if (j.contains("viewAngle")) {

        viewAngle =
            std::clamp(
                j["viewAngle"].get<float>(),
                1.0f,
                360.0f
            );
    }


    if (j.contains("cullDistance")) {

        cullDistance =
            std::clamp(
                j["cullDistance"].get<float>(),
                8.0f,
                540.0f
            );
    }


    if (j.contains("playersVisiblePercent")) {

        playersVisiblePercent =
            std::clamp(
                j["playersVisiblePercent"].get<int>(),
                0,
                100
            );
    }


    if (j.contains("entitiesVisiblePercent")) {

        entitiesVisiblePercent =
            std::clamp(
                j["entitiesVisiblePercent"].get<int>(),
                0,
                100
            );
    }
}


void EntityCullingModule::saveConfig(
    nlohmann::json& j
) {
    Module::saveConfig(j);


    j["cullPlayers"] =
        cullPlayers;


    j["cullEntities"] =
        cullEntities;


    j["wallCulling"] =
        wallCulling;


    j["viewAngle"] =
        viewAngle;


    j["cullDistance"] =
        cullDistance;


    j["playersVisiblePercent"] =
        playersVisiblePercent;


    j["entitiesVisiblePercent"] =
        entitiesVisiblePercent;
}
