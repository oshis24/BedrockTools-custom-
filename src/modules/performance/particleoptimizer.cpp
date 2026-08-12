#include "particleoptimizer.hpp"

#include "core/memory/Hooks.hpp"

#include <android/asset_manager.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <string_view>

namespace {

using AAssetManagerOpenFn =
    AAsset* (*)(AAssetManager*, const char*, int);

ParticleOptimizerModule* g_module = nullptr;

AAssetManagerOpenFn g_openOriginal = nullptr;

bedrocktools::hooks::Handle g_openHook = nullptr;

bool g_initialized = false;


/*
 * We deliberately use the same basic idea as the reference
 * implementation:
 *
 *     matched material
 *          ↓
 *     original AAssetManager_open()
 *          ↓
 *     non-existent sentinel asset
 *
 * We do NOT return nullptr directly.
 */
constexpr const char* kDisabledAsset =
    "__bedrocktools_disabled_material__";


struct MaterialRule {
    bool ParticleOptimizerModule::*flag;
    std::string_view name;
};


/*
 * Material names confirmed during RE.
 */
constexpr std::array<MaterialRule, 9> kRules{{
    {&ParticleOptimizerModule::noParticles,
        "Particle.material.bin"},

    {&ParticleOptimizerModule::noParticles,
        "ParticlePrepass.material.bin"},

    {&ParticleOptimizerModule::noParticles,
        "ParticleForwardPBR.material.bin"},

    {&ParticleOptimizerModule::noFlipbook,
        "Flipbook.material.bin"},

    {&ParticleOptimizerModule::noShadow,
        "ShadowOverlay.material.bin"},

    {&ParticleOptimizerModule::noWeather,
        "Weather.material.bin"},

    {&ParticleOptimizerModule::noWeather,
        "WeatherForwardPBR.material.bin"},

    {&ParticleOptimizerModule::noStars,
        "Stars.material.bin"},

    {&ParticleOptimizerModule::noStars,
        "StarsForwardPBR.material.bin"}
}};


/*
 * Separate rules for Sun/Moon because the array above is
 * intentionally compact and easy to extend.
 */
constexpr std::array<MaterialRule, 2> kSunMoonRules{{
    {&ParticleOptimizerModule::noSunMoon,
        "SunMoon.material.bin"},

    {&ParticleOptimizerModule::noSunMoon,
        "SunMoonForwardPBR.material.bin"}
}};


/*
 * Extract only the final filename component.
 *
 * The reference implementation uses std::filesystem::path
 * and then filename().
 *
 * We do the equivalent without constructing filesystem
 * objects on every asset request.
 */
std::string_view baseName(
    const char* filename
) {
    if (!filename)
        return {};

    const std::string_view path(filename);

    const std::size_t slash =
        path.find_last_of("/\\");

    if (slash == std::string_view::npos)
        return path;

    return path.substr(slash + 1);
}


/*
 * Match the final component rather than relying on one
 * specific absolute/relative path.
 *
 * This preserves the important portability property of
 * the old mod: all known path variants collapse to the
 * same filename.
 */
bool shouldDisable(
    ParticleOptimizerModule* module,
    const char* filename
) {
    if (!module || !filename)
        return false;

    const std::string_view name =
        baseName(filename);

    if (name.empty())
        return false;


    for (const auto& rule : kRules) {
        if (name == rule.name &&
            module->*(rule.flag)) {

            return true;
        }
    }


    for (const auto& rule : kSunMoonRules) {
        if (name == rule.name &&
            module->*(rule.flag)) {

            return true;
        }
    }


    return false;
}


/*
 * AAssetManager_open detour.
 */
AAsset* assetOpenHook(
    AAssetManager* manager,
    const char* filename,
    int mode
) {
    /*
     * Fail open:
     * if the module/original is unavailable, preserve
     * normal Minecraft behaviour.
     */
    if (!g_openOriginal)
        return nullptr;


    /*
     * The hook remains installed while the module is disabled.
     * This makes module enable/disable safe without having to
     * repeatedly patch a system library.
     */
    if (!g_module ||
        !g_module->enabled) {

        return g_openOriginal(
            manager,
            filename,
            mode
        );
    }


    /*
     * Only inspect the asset name when at least one relevant
     * switch is enabled.
     */
    if (shouldDisable(
            g_module,
            filename
        )) {

        /*
         * This follows the reference mod's behaviour:
         *
         *     matched real asset
         *             ↓
         *     replace filename
         *             ↓
         *     original AAssetManager_open
         *
         * The sentinel is intentionally not a real asset.
         */
        return g_openOriginal(
            manager,
            kDisabledAsset,
            mode
        );
    }


    return g_openOriginal(
        manager,
        filename,
        mode
    );
}


void installHook() {
    if (g_initialized)
        return;

    auto libandroid =
        bedrocktools::hooks::openLibrary(
            "libandroid.so"
        );

    if (!libandroid)
        return;


    const auto address =
        bedrocktools::hooks::symbol(
            libandroid,
            "AAssetManager_open"
        );


    if (!address) {
        bedrocktools::hooks::closeLibrary(
            libandroid
        );
        return;
    }


    g_openHook =
        bedrocktools::hooks::install(
            reinterpret_cast<void*>(address),
            reinterpret_cast<void*>(assetOpenHook),
            reinterpret_cast<void**>(
                &g_openOriginal
            )
        );


    bedrocktools::hooks::closeLibrary(
        libandroid
    );


    g_initialized =
        g_openHook != nullptr &&
        g_openOriginal != nullptr;
}

} // namespace


ParticleOptimizerModule::ParticleOptimizerModule()
    : Module(
        "Particle Optimizer",
        "Disables selected Minecraft material effects to reduce rendering workload."
    ) {
    g_module = this;
}


ParticleOptimizerModule::~ParticleOptimizerModule() {
    /*
     * We intentionally keep the system-library hook installed
     * for the lifetime of BedrockTools.
     *
     * The detour becomes a transparent pass-through when the
     * module is disabled.
     */
    if (g_module == this)
        g_module = nullptr;
}


void ParticleOptimizerModule::onInit() {
    installHook();
}


void ParticleOptimizerModule::onEnable() {
    /*
     * Nothing else is required.
     *
     * The installed hook reads the current module state.
     */
}


void ParticleOptimizerModule::onDisable() {
    /*
     * Keep hook installed and become a transparent
     * AAssetManager_open pass-through.
     */
}


void ParticleOptimizerModule::loadConfig(
    const nlohmann::json& j
) {
    Module::loadConfig(j);

    if (j.contains("noParticles"))
        noParticles =
            j["noParticles"].get<bool>();

    if (j.contains("noFlipbook"))
        noFlipbook =
            j["noFlipbook"].get<bool>();

    if (j.contains("noShadow"))
        noShadow =
            j["noShadow"].get<bool>();

    if (j.contains("noWeather"))
        noWeather =
            j["noWeather"].get<bool>();

    if (j.contains("noStars"))
        noStars =
            j["noStars"].get<bool>();

    if (j.contains("noSunMoon"))
        noSunMoon =
            j["noSunMoon"].get<bool>();
}


void ParticleOptimizerModule::saveConfig(
    nlohmann::json& j
) {
    Module::saveConfig(j);

    j["noParticles"] = noParticles;
    j["noFlipbook"] = noFlipbook;
    j["noShadow"] = noShadow;
    j["noWeather"] = noWeather;
    j["noStars"] = noStars;
    j["noSunMoon"] = noSunMoon;
}
