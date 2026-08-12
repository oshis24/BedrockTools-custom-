#include "particleoptimizer.hpp"

#include "core/memory/Hooks.hpp"

#include <android/asset_manager.h>

#include <array>
#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <string_view>
#include <thread>

namespace {

using AAssetManagerOpenFn =
    AAsset* (*)(AAssetManager*, const char*, int);

ParticleOptimizerModule* g_module = nullptr;

AAssetManagerOpenFn g_openOriginal = nullptr;
bedrocktools::hooks::Handle g_openHook = nullptr;

std::atomic_bool g_workerStarted{false};
std::atomic_bool g_hookInstalled{false};


/*
 * Reference waits for libminecraftpe.so before installing
 * the AAssetManager hook.
 */
constexpr const char* kMinecraftLibrary =
    "libminecraftpe.so";

constexpr const char* kAndroidLibrary =
    "libandroid.so";

constexpr const char* kAssetOpenSymbol =
    "AAssetManager_open";


/*
 * Reference ultimately passes a filename which does not
 * represent a valid Minecraft material.
 *
 * We intentionally use a guaranteed-invalid filename rather
 * than relying on a pointer into the reference binary.
 */
constexpr const char* kDisabledAsset =
    "__bedrocktools_missing_material__.bin";


struct MaterialRule {
    bool ParticleOptimizerModule::*flag;
    std::string_view name;
};


/*
 * Material names reconstructed from the reference binary.
 */
constexpr std::array<MaterialRule, 11> kRules{{
    {
        &ParticleOptimizerModule::noParticles,
        "Particle.material.bin"
    },

    {
        &ParticleOptimizerModule::noParticles,
        "ParticlePrepass.material.bin"
    },

    {
        &ParticleOptimizerModule::noParticles,
        "ParticleForwardPBR.material.bin"
    },

    {
        &ParticleOptimizerModule::noFlipbook,
        "Flipbook.material.bin"
    },

    {
        &ParticleOptimizerModule::noShadow,
        "ShadowOverlay.material.bin"
    },

    {
        &ParticleOptimizerModule::noWeather,
        "Weather.material.bin"
    },

    {
        &ParticleOptimizerModule::noWeather,
        "WeatherForwardPBR.material.bin"
    },

    {
        &ParticleOptimizerModule::noStars,
        "Stars.material.bin"
    },

    {
        &ParticleOptimizerModule::noStars,
        "StarsForwardPBR.material.bin"
    },

    {
        &ParticleOptimizerModule::noSunMoon,
        "SunMoon.material.bin"
    },

    {
        &ParticleOptimizerModule::noSunMoon,
        "SunMoonForwardPBR.material.bin"
    }
}};


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

    return path.substr(
        slash + 1
    );
}


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
        if (
            name == rule.name &&
            module->*(rule.flag)
        ) {
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
    if (!g_openOriginal) {
        return nullptr;
    }


    /*
     * Disabled module = exact pass-through.
     */
    if (
        !g_module ||
        !g_module->enabled
    ) {
        return g_openOriginal(
            manager,
            filename,
            mode
        );
    }


    /*
     * Match material filename.
     */
    if (
        shouldDisable(
            g_module,
            filename
        )
    ) {
        /*
         * Do NOT return nullptr directly.
         *
         * Reference calls the original function again
         * using an invalid filename.
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


/*
 * Reference behaviour:
 *
 *     wait for libminecraftpe.so
 *     retry every 100 ms
 *     maximum ~100 attempts
 *     then open libandroid.so
 *     resolve AAssetManager_open
 *     install hook
 */
void assetHookWorker() {

    constexpr int kMaxAttempts = 100;

    constexpr auto kDelay =
        std::chrono::milliseconds(100);


    for (
        int attempt = 0;
        attempt < kMaxAttempts;
        ++attempt
    ) {

        if (
            g_hookInstalled.load(
                std::memory_order_acquire
            )
        ) {
            return;
        }


        /*
         * IMPORTANT:
         *
         * RTLD_NOLOAD is used here.
         *
         * We do not force Minecraft's native library
         * to load ourselves.
         */
        void* minecraft =
            dlopen(
                kMinecraftLibrary,
                RTLD_NOW | RTLD_NOLOAD
            );


        if (minecraft) {

            dlclose(minecraft);


            /*
             * Now obtain libandroid.so.
             */
            void* android =
                dlopen(
                    kAndroidLibrary,
                    RTLD_NOW | RTLD_NOLOAD
                );


            if (!android) {
                android =
                    dlopen(
                        kAndroidLibrary,
                        RTLD_NOW
                    );
            }


            if (android) {

                void* target =
                    dlsym(
                        android,
                        kAssetOpenSymbol
                    );


                if (target) {

                    auto hook =
                        bedrocktools::hooks::install(
                            target,
                            reinterpret_cast<void*>(
                                assetOpenHook
                            ),
                            reinterpret_cast<void**>(
                                &g_openOriginal
                            )
                        );


                    if (
                        hook &&
                        g_openOriginal
                    ) {

                        g_openHook = hook;

                        g_hookInstalled.store(
                            true,
                            std::memory_order_release
                        );

                        dlclose(android);

                        return;
                    }
                }


                dlclose(android);
            }
        }


        std::this_thread::sleep_for(
            kDelay
        );
    }
}


void startWorker() {

    bool expected = false;


    if (
        !g_workerStarted.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel
        )
    ) {
        return;
    }


    std::thread(
        assetHookWorker
    ).detach();
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

    if (g_module == this) {
        g_module = nullptr;
    }
}


void ParticleOptimizerModule::onInit() {

    /*
     * NEVER block ModuleRegistry::initialize().
     */
    startWorker();
}


void ParticleOptimizerModule::onEnable() {
}


void ParticleOptimizerModule::onDisable() {
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


    j["noParticles"] =
        noParticles;

    j["noFlipbook"] =
        noFlipbook;

    j["noShadow"] =
        noShadow;

    j["noWeather"] =
        noWeather;

    j["noStars"] =
        noStars;

    j["noSunMoon"] =
        noSunMoon;
}
