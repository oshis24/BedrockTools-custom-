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


/*
 * Convert an angle into [-180, 180].
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
 *
 * BedrockTools reference uses an atan2-based horizontal
 * direction test rather than a complete frustum test.
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

    /*
     * Minecraft's yaw coordinate system requires the
     * 90 degree adjustment used by the reference.
     */
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
 * The reference implementation scales sample count
 * with distance:
 *
 *     distance * 2
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

    /*
     * Skip both endpoints.
     *
     * i = 1 ... sampleCount - 1
     */
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
 * Keep only the requested percentage of candidates.
 *
 * Candidates are ordered nearest -> farthest.
 *
 * We use nth_element when possible so that a 25%-50%
 * configuration does not require a complete sort of
 * every candidate.
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
     * Find the boundary element first.
     *
     * This is cheaper than sorting all candidates when
     * only a small percentage is retained.
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
     * Only sort the candidates that will actually
     * survive.
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
 * Main ActorManagerList detour.
 */
std::vector<void*> entityCullingHook(
    void* actorManager
) {
    if (!g_actorManagerListOriginal)
        return {};

    /*
     * Always obtain the original Actor list.
     */
    const std::vector<void*> actors =
        g_actorManagerListOriginal(
            actorManager
        );

    /*
     * If disabled, return the original result without
     * doing any additional work.
     */
    if (!g_module ||
        !g_module->enabled) {
        return actors;
    }

    if (actors.empty())
        return actors;

    if (!g_module->cullPlayers &&
        !g_module->cullEntities) {
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
     * The reference culling implementation obtains the
     * camera position/rotation from the player.
     *
     * Using the SDK Actor directly also avoids the extra
     * renderer/player-renderer chain used in v1.
     */
    const bedrocktools::sdk::Vec3 camera =
        localPlayer->position();

    const bedrocktools::sdk::Vec2 rotation =
        localPlayer->rotation();

    const float cameraYaw =
        rotation.y;


    /*
     * Maximum distance.
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
     * Block source for wall culling.
     */
    bedrocktools::sdk::BlockSource*
        blockSource = nullptr;

    if (auto* dimension =
            localPlayer->dimension()) {

        blockSource =
            dimension->blockSource();
    }


    /*
     * IMPORTANT:
     *
     * result contains actors which are guaranteed to
     * survive without going through the candidate list.
     *
     * candidates contains actors that are subject to
     * culling.
     */
    std::vector<void*> result;

    std::vector<Candidate> players;
    std::vector<Candidate> entities;


    /*
     * We intentionally do NOT reserve actors.size().
     *
     * The reference implementation grows its vectors
     * only when candidates actually survive the initial
     * filtering.
     */
    result.reserve(
        actors.size()
    );


    /*
     * SINGLE PASS through ActorManagerList.
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
         * Determine whether this Actor is a player.
         */
        const bool isPlayer =
            g_actorIsPlayer
                ? g_actorIsPlayer(actor)
                : false;


        /*
         * Determine whether this category is enabled.
         */
        const bool shouldCull =
            isPlayer
                ? g_module->cullPlayers
                : g_module->cullEntities;


        /*
         * If this category is disabled, preserve the
         * Actor immediately.
         */
        if (!shouldCull) {
            result.push_back(actor);
            continue;
        }


        /*
         * Already-invisible Actors are NOT removed.
         *
         * They are simply excluded from our additional
         * culling logic.
         */
        if (g_actorIsInvisible &&
            g_actorIsInvisible(actor)) {

            result.push_back(actor);
            continue;
        }


        auto* nativeActor =
            reinterpret_cast<
                bedrocktools::sdk::Actor*
            >(actor);


        /*
         * Actor bounding-box center.
         */
        const bedrocktools::sdk::AABB bounds =
            nativeActor->bounds();

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
         * Actor survived visibility tests.
         */
        Candidate candidate{
            actor,
            distanceSq
        };


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
     * Apply percentage limits.
     *
     * Both lists are sorted nearest-first.
     */
    trimCandidates(
        players,
        g_module->playersVisiblePercent
    );

    trimCandidates(
        entities,
        g_module->entitiesVisiblePercent
    );


    /*
     * Append surviving candidates.
     *
     * No second scan of the original Actor list.
     */
    for (const Candidate& candidate :
         players) {

        if (candidate.actor)
            result.push_back(
                candidate.actor
            );
    }

    for (const Candidate& candidate :
         entities) {

        if (candidate.actor)
            result.push_back(
                candidate.actor
            );
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


    /*
     * Do not install the hook here.
     *
     * Module::loadConfig() may enable the module before
     * initialization depending on startup order.
     *
     * onEnable() is the single hook installation point.
     */
}


void EntityCullingModule::installHooks() {

    if (m_actorManagerListHook)
        return;

    if (!m_actorManagerListTarget)
        return;

    m_actorManagerListHook =
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

    if (!m_actorManagerListHook)
        return;

    bedrocktools::hooks::remove(
        m_actorManagerListHook
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
