#include "particleoptimizer.hpp"

#include "core/memory/Hooks.hpp"

#include <android/asset_manager.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <dlfcn.h>
#include <string_view>
#include <thread>

namespace {

using AAssetManagerOpenFn =
    AAsset* (*)(AAssetManager*, const char*, int);

ParticleOptimizerModule* g_module = nullptr;

AAssetManagerOpenFn g_openOriginal = nullptr;
bedrocktools::hooks::Handle g_openHook = nullptr;

std::atomic_bool g_workerStarted = false;
std::atomic_bool g_hookInstalled = false;


/*
 * This is deliberately not a real Minecraft asset.
 *
 * RE of the old UnViableTweaks hook shows that after a
 * matched material it redirects AAssetManager_open() to
 * a non-existent filename. The pointer lands in the
 * "form" substring of the nearby static string data.
 *
 * Using a short non-existent filename is sufficient.
 */
constexpr std::string_view kDisabledAsset = "form";


struct MaterialRule {
    bool ParticleOptimizerModule::*flag;
    std::string_view name;
};


/*
 * Material names found in the old implementation.
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
 * AAssetManager_open hook.
 *
 * This hook is intentionally very cheap:
 *
 *     disabled
 *          -> original()
 *
 *     enabled + non-target
 *          -> original(filename)
 *
 *     enabled + target
 *          -> original("form")
 */
AAsset* assetOpenHook(
    AAssetManager* manager,
    const char* filename,
    int mode
) {
    if (!g_openOriginal)
        return nullptr;

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

    if (
        shouldDisable(
            g_module,
            filename
        )
    ) {
        return g_openOriginal(
            manager,
            kDisabledAsset.data(),
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
 * The reference implementation waits for libandroid.so
 * instead of forcing the library to load.
 *
 * Observed behaviour:
 *
 *     dlopen(..., RTLD_NOW | RTLD_NOLOAD)
 *     retry
 *     sleep ~100 ms
 *     up to roughly 100 attempts
 */
void assetHookWorker() {
    constexpr int kAttempts = 100;
    constexpr auto kDelay =
        std::chrono::milliseconds(100);

    for (
        int attempt = 0;
        attempt < kAttempts;
        ++attempt
    ) {
        if (g_hookInstalled.load(
                std::memory_order_acquire
            )) {
            return;
        }

        void* libandroid =
            dlopen(
                "libandroid.so",
                RTLD_NOW | RTLD_NOLOAD
            );

        if (libandroid) {

            void* symbol =
                dlsym(
                    libandroid,
                    "AAssetManager_open"
                );

            if (symbol) {

                auto hook =
                    bedrocktools::hooks::install(
                        symbol,
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

                    dlclose(libandroid);
                    return;
                }
            }

            dlclose(libandroid);
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
        "Disables selected particle and material effects."
    ) {
    g_module = this;
}


ParticleOptimizerModule::~ParticleOptimizerModule() {
    /*
     * BedrockTools owns modules for the lifetime of the
     * runtime, so the system hook remains installed.
     *
     * The detour becomes a transparent pass-through when
     * this module is disabled.
     */
    if (g_module == this)
        g_module = nullptr;
}


void ParticleOptimizerModule::onInit() {
    /*
     * IMPORTANT:
     *
     * Do not block ModuleRegistry::initialize().
     *
     * The reference implementation installs this hook
     * asynchronously after libandroid is available.
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

    j["noParticles"] = noParticles;
    j["noFlipbook"] = noFlipbook;
    j["noShadow"] = noShadow;
    j["noWeather"] = noWeather;
    j["noStars"] = noStars;
    j["noSunMoon"] = noSunMoon;
}
