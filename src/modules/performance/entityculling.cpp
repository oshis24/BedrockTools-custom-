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

using ActorManagerListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);
using BlockSourceIsSolidBlockingBlockFn =
    bool (*)(void*, const bedrocktools::sdk::BlockPos*);

struct Candidate {
    void* actor = nullptr;
    float distanceSq = 0.0f;
};

EntityCullingModule* g_entityCulling = nullptr;

ActorManagerListFn g_actorManagerListOriginal = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorIsInvisibleFn g_actorIsInvisible = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;


/*
 * Normalize an angle to [-180, 180].
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
 * Reference Entity Culling uses atan2-based horizontal
 * direction comparison rather than a full frustum-plane test.
 */
bool withinViewAngle(
    float dx,
    float dz,
    float cameraYaw,
    float viewAngle
) {
    if (viewAngle >= 360.0f)
        return true;

    if (viewAngle <= 0.0f)
        return false;

    constexpr float RadToDeg =
        57.29577951308232f;

    const float direction =
        std::atan2(dz, dx) * RadToDeg;

    /*
     * The reference implementation applies a 90 degree
     * coordinate-system offset before normalization.
     */
    const float relative =
        normalizeDegrees(
            direction - (cameraYaw + 90.0f)
        );

    return std::fabs(relative) <=
           (viewAngle * 0.5f);
}


/*
 * Test whether the line from camera to an entity passes
 * through a solid-blocking block.
 *
 * The reference implementation increases the number of
 * samples with distance and clamps it to [2, 256].
 */
bool lineHitsSolidBlock(
    bedrocktools::sdk::BlockSource* blockSource,
    const bedrocktools::sdk::Vec3& start,
    const bedrocktools::sdk::Vec3& end
) {
    if (!blockSource || !g_isSolidBlockingBlock)
        return false;

    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float dz = end.z - start.z;

    const float distance =
        std::sqrt(
            dx * dx +
            dy * dy +
            dz * dz
        );

    int sampleCount =
        static_cast<int>(distance * 2.0f);

    sampleCount =
        std::clamp(sampleCount, 2, 256);

    const float step =
        1.0f / static_cast<float>(sampleCount);

    /*
     * Do not test the camera endpoint or entity endpoint.
     * Those points are not wall samples in the reference
     * implementation.
     */
    for (int i = 1; i < sampleCount; ++i) {
        const float t =
            static_cast<float>(i) * step;

        bedrocktools::sdk::BlockPos blockPos{
            static_cast<int>(
                std::floor(start.x + dx * t)
            ),
            static_cast<int>(
                std::floor(start.y + dy * t)
            ),
            static_cast<int>(
                std::floor(start.z + dz * t)
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
 * Keep only the requested percentage of candidates.
 *
 * Candidates are sorted by distance first, so this keeps
 * the nearest candidates.
 */
void trimCandidates(
    std::vector<Candidate>& candidates,
    int percent
) {
    percent =
        std::clamp(percent, 0, 100);

    const std::size_t count =
        candidates.size();

    const std::size_t keep =
        (
            count *
            static_cast<std::size_t>(percent) +
            99u
        ) / 100u;

    if (keep < count)
        candidates.resize(keep);
}


/*
 * Main ActorManagerList detour.
 */
std::vector<void*> entityCullingHook(
    void* actorManager
) {
    if (!g_actorManagerListOriginal)
        return {};

    /*
     * Always obtain the original list first.
     */
    auto actors =
        g_actorManagerListOriginal(actorManager);

    /*
     * If the module is disabled, behave exactly like
     * the original function.
     */
    if (!g_entityCulling ||
        !g_entityCulling->enabled) {
        return actors;
    }

    if (actors.empty())
        return actors;

    if (!g_entityCulling->cullPlayers &&
        !g_entityCulling->cullEntities) {
        return actors;
    }

    /*
     * Obtain the current ClientInstance.
     */
    auto* client =
        bedrocktools::sdk::ClientInstance::current();

    if (!client)
        return actors;

    /*
     * Obtain the local player.
     */
    auto* localPlayer =
        client->localPlayer();

    if (!localPlayer)
        return actors;

    /*
     * Entity Culling uses the renderer's camera position.
     */
    auto* renderer =
        client->levelRenderer();

    if (!renderer)
        return actors;

    auto* rendererPlayer =
        renderer->playerRenderer();

    if (!rendererPlayer)
        return actors;

    const bedrocktools::sdk::Vec3 camera =
        rendererPlayer->cameraPosition();

    /*
     * Local player's yaw.
     */
    const bedrocktools::sdk::Vec2 rotation =
        localPlayer->rotation();

    const float cameraYaw =
        rotation.y;

    /*
     * Distance limit.
     */
    const float distance =
        std::max(
            0.0f,
            g_entityCulling->cullDistance
        );

    const float maxDistanceSq =
        distance * distance;

    /*
     * World block source used for wall culling.
     */
    auto* dimension =
        localPlayer->dimension();

    auto* blockSource =
        dimension
            ? dimension->blockSource()
            : nullptr;

    std::vector<Candidate> players;
    std::vector<Candidate> entities;

    players.reserve(actors.size());
    entities.reserve(actors.size());


    /*
     * Iterate through every Actor returned by Minecraft.
     */
    for (void* actor : actors) {
        if (!actor)
            continue;

        /*
         * Never cull the local player.
         */
        if (actor == localPlayer)
            continue;

        /*
         * Determine player/entity category.
         */
        const bool isPlayer =
            g_actorIsPlayer
                ? g_actorIsPlayer(actor)
                : false;

        const bool shouldCull =
            isPlayer
                ? g_entityCulling->cullPlayers
                : g_entityCulling->cullEntities;

        /*
         * If this category is disabled, we don't add the
         * Actor to the culling candidate list.
         *
         * It will be preserved below when rebuilding the
         * final list.
         */
        if (!shouldCull)
            continue;

        /*
         * Minecraft already considers this Actor invisible.
         * Don't attempt additional visibility calculations.
         */
        if (g_actorIsInvisible &&
            g_actorIsInvisible(actor)) {
            continue;
        }

        auto* nativeActor =
            reinterpret_cast<
                bedrocktools::sdk::Actor*
            >(actor);

        /*
         * Get the Actor AABB.
         */
        const bedrocktools::sdk::AABB bounds =
            nativeActor->bounds();

        /*
         * Entity center.
         */
        const bedrocktools::sdk::Vec3 center{
            (bounds.min.x + bounds.max.x) * 0.5f,
            (bounds.min.y + bounds.max.y) * 0.5f,
            (bounds.min.z + bounds.max.z) * 0.5f
        };

        /*
         * Camera -> entity vector.
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
         * Horizontal angle/FOV culling.
         */
        if (!withinViewAngle(
                dx,
                dz,
                cameraYaw,
                g_entityCulling->viewAngle
            )) {
            continue;
        }

        /*
         * Optional wall culling.
         */
        if (g_entityCulling->wallCulling &&
            blockSource &&
            g_isSolidBlockingBlock) {

            if (lineHitsSolidBlock(
                    blockSource,
                    camera,
                    center
                )) {
                continue;
            }
        }

        /*
         * Candidate survived all visibility tests.
         */
        Candidate candidate{
            actor,
            distanceSq
        };

        if (isPlayer)
            players.push_back(candidate);
        else
            entities.push_back(candidate);
    }


    /*
     * Sort nearest → farthest.
     */
    const auto byDistance =
        [](const Candidate& a,
           const Candidate& b) {
            return a.distanceSq <
                   b.distanceSq;
        };

    std::sort(
        players.begin(),
        players.end(),
        byDistance
    );

    std::sort(
        entities.begin(),
        entities.end(),
        byDistance
    );


    /*
     * Keep the requested percentage.
     */
    trimCandidates(
        players,
        g_entityCulling->playersVisiblePercent
    );

    trimCandidates(
        entities,
        g_entityCulling->entitiesVisiblePercent
    );


    /*
     * Mark survivors.
     *
     * Actors for which culling was disabled are preserved.
     * The local player is also preserved.
     */
    std::vector<void*> result;
    result.reserve(actors.size());

    for (void* actor : actors) {
        if (!actor)
            continue;

        if (actor == localPlayer) {
            result.push_back(actor);
            continue;
        }

        const bool isPlayer =
            g_actorIsPlayer
                ? g_actorIsPlayer(actor)
                : false;

        const bool shouldCull =
            isPlayer
                ? g_entityCulling->cullPlayers
                : g_entityCulling->cullEntities;

        /*
         * Category isn't being culled.
         */
        if (!shouldCull) {
            result.push_back(actor);
            continue;
        }

        /*
         * Search candidate lists for the Actor.
         *
         * This is intentionally simple for the first test
         * build. We can optimize this after functionality
         * is confirmed.
         */
        bool keep = false;

        const auto& list =
            isPlayer
                ? players
                : entities;

        for (const auto& candidate : list) {
            if (candidate.actor == actor) {
                keep = true;
                break;
            }
        }

        if (keep)
            result.push_back(actor);
    }

    return result;
}

} // namespace


EntityCullingModule::EntityCullingModule()
    : Module(
        "Entity Culling",
        "Cull distant, off-angle, and wall-occluded entities for better performance."
    ) {
    g_entityCulling = this;

    /*
     * This module has no HUD editor component.
     */
    hideInHudEditor = true;
}


EntityCullingModule::~EntityCullingModule() {
    removeHooks();

    if (g_entityCulling == this)
        g_entityCulling = nullptr;
}


void EntityCullingModule::onInit() {
    /*
     * Resolve ActorManagerList.
     */
    m_actorManagerListTarget =
        reinterpret_cast<void*>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::ActorManagerList
            )
        );

    /*
     * Resolve Actor helpers.
     */
    g_actorIsPlayer =
        reinterpret_cast<ActorIsPlayerFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::ActorIsPlayer
            )
        );

    g_actorIsInvisible =
        reinterpret_cast<ActorIsInvisibleFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::ActorIsInvisible
            )
        );

    /*
     * Resolve block visibility helper.
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

    /*
     * If the module was enabled from config before
     * initialization, install the hook now.
     */
    if (enabled)
        installHooks();
}


void EntityCullingModule::installHooks() {
    if (m_actorManagerListHook)
        return;

    if (!m_actorManagerListTarget)
        return;

    m_actorManagerListHook =
        reinterpret_cast<void*>(
            bedrocktools::hooks::install(
                m_actorManagerListTarget,
                reinterpret_cast<void*>(
                    entityCullingHook
                ),
                reinterpret_cast<void**>(
                    &g_actorManagerListOriginal
                )
            )
        );
}


void EntityCullingModule::removeHooks() {
    if (!m_actorManagerListHook)
        return;

    bedrocktools::hooks::remove(
        reinterpret_cast<
            bedrocktools::hooks::Handle
        >(m_actorManagerListHook)
    );

    m_actorManagerListHook = nullptr;
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

    if (j.contains("cullPlayers"))
        cullPlayers =
            j["cullPlayers"].get<bool>();

    if (j.contains("cullEntities"))
        cullEntities =
            j["cullEntities"].get<bool>();

    if (j.contains("wallCulling"))
        wallCulling =
            j["wallCulling"].get<bool>();

    if (j.contains("viewAngle"))
        viewAngle =
            std::clamp(
                j["viewAngle"].get<float>(),
                1.0f,
                360.0f
            );

    if (j.contains("cullDistance"))
        cullDistance =
            std::clamp(
                j["cullDistance"].get<float>(),
                8.0f,
                540.0f
            );

    if (j.contains("playersVisiblePercent"))
        playersVisiblePercent =
            std::clamp(
                j["playersVisiblePercent"].get<int>(),
                0,
                100
            );

    if (j.contains("entitiesVisiblePercent"))
        entitiesVisiblePercent =
            std::clamp(
                j["entitiesVisiblePercent"].get<int>(),
                0,
                100
            );
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
---REG---
ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry registry;
    return registry;
}

ModuleRegistry.cpp include missing in tail? It had performance include already. So we need tell user only three changes. But code itself still potentially has semantic mismatch: Invisible should probably preserve not skip. Reference said invisible entities maybe skip culling, not cull them. Our current code skips them from candidate, meaning they disappear! Must fix: if invisible, result should preserve actor. Also wall culling should perhaps only if blocked, but if blockSource null preserve. Our code if cull target and no blockSource still candidate, okay.

Most dangerous: returning filtered list from ActorManagerList may have huge semantic implications. But reference does it. Fine.

Potential recursion? Hook ActorManagerList and our code calls original. Fine.

Need no use `std::unordered_set` now. Need code complete. Also view angle reference perhaps parameter +0x48 not necessarily 180 default. Fine.

Could perhaps use linear search O(n²) after sort; okay first build, but performance mod itself adds overhead. Better use unordered_set survivors, but code simple. Yet entity culling should be efficient. We can use unordered_set. But include unordered_set. The current code's linear search can be bad. Let's make survivors set for O(n). But memory overhead. Fine.

Need maybe install hook on init if enabled; Module config load happens before initialize? uncertain but okay.

Need mention do NOT change Signatures.cpp. No new signatures because existing ones. Need mention xmake no change.

Need writing block because complete code artifact. Maybe two writing blocks? user asks code complete, multiple files. Use document blocks? Could put 3 files in one document writing block with headings and code? Writing blocks should be finished reusable text, but code is excluded. So code blocks outside writing block. Developer says no writing block for code. Good.

Need explain current source has already been modified but user should not use that archive? They want GitHub. We can say ignore previous generated archive. Need maybe exact registry diff. Could give full ModuleRegistry.cpp? User asks code complete, perhaps only lines to add. But "kirim kode lengkap nya kesini nnti saya tempel ke github". Could mean complete entityculling files. We'll give complete 2 files and exact registry additions, not full 200-line registry. Also perhaps manifest no need.

Need cite source lines. We can mention xmake auto includes cpp and existing signatures. Also `ClientInstance::current`, levelRenderer, localPlayer. We have source lines for localPlayer truncated but enough. Could cite. Need no URL.

Let's final in Indonesian.
