#include "outlinergb.hpp"

#include "core/memory/Hooks.hpp"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>


namespace {

using GlUniform4fvFn =
    void (*)(GLint, GLsizei, const GLfloat*);

using GlUseProgramFn =
    void (*)(GLuint);

using GlEnableFn =
    void (*)(GLenum);

using GlDisableFn =
    void (*)(GLenum);

using GlGetProgramivFn =
    void (*)(GLuint, GLenum, GLint*);

using GlGetActiveUniformFn =
    void (*)(
        GLuint,
        GLuint,
        GLsizei,
        GLsizei*,
        GLint*,
        GLenum*,
        GLchar*
    );

using GlGetUniformLocationFn =
    GLint (*)(GLuint, const GLchar*);


OutlineRGBModule* g_module = nullptr;


/*
 * Original functions.
 */
GlUniform4fvFn g_uniform4fvOriginal = nullptr;
GlUseProgramFn g_useProgramOriginal = nullptr;
GlEnableFn g_enableOriginal = nullptr;
GlDisableFn g_disableOriginal = nullptr;


/*
 * GL helper functions. They are not hooked; they are only
 * resolved from the same GLES library.
 */
GlGetProgramivFn g_getProgramiv = nullptr;
GlGetActiveUniformFn g_getActiveUniform = nullptr;
GlGetUniformLocationFn g_getUniformLocation = nullptr;


/*
 * Hook handles.
 */
bedrocktools::hooks::Handle
    g_uniform4fvHook = nullptr;

bedrocktools::hooks::Handle
    g_useProgramHook = nullptr;

bedrocktools::hooks::Handle
    g_enableHook = nullptr;

bedrocktools::hooks::Handle
    g_disableHook = nullptr;


/*
 * Initialization state.
 */
std::atomic_bool g_workerStarted = false;
std::atomic_bool g_hooksInstalled = false;


/*
 * RE reference tracks the current GL program.
 *
 * GL calls are made from the rendering thread, so a
 * thread-local value is safer for our implementation.
 */
thread_local GLuint g_currentProgram = 0;


/*
 * RE reference tracks GL_DEPTH_TEST through glEnable/
 * glDisable.
 */
thread_local bool g_depthTestEnabled = false;


/*
 * Cached information for each shader program.
 */
struct ProgramInfo {
    bool inspected = false;

    GLint outlineLocation = -1;
};


std::mutex g_programMutex;

std::unordered_map<
    GLuint,
    ProgramInfo
> g_programs;


/*
 * Exact shader uniform names found in the reference RE.
 *
 * These are the important correction over our previous
 * heuristic "Bones/Color/MatColor" implementation.
 */
constexpr std::string_view
    kOutlineUniformA =
        "FogAndDistanceColor";

constexpr std::string_view
    kOutlineUniformB =
        "RenderChunkAlpha";


/*
 * HSV → RGB.
 */
std::array<float, 3> hsvToRgb(
    float hue
) {
    hue -= std::floor(hue);

    const float scaled =
        hue * 6.0f;

    const int sector =
        static_cast<int>(
            std::floor(scaled)
        );

    const float fraction =
        scaled -
        static_cast<float>(sector);

    const float p =
        0.0f;

    const float q =
        1.0f -
        fraction;

    const float t =
        fraction;

    switch (sector % 6) {
        case 0:
            return {1.0f, t, p};

        case 1:
            return {q, 1.0f, p};

        case 2:
            return {p, 1.0f, t};

        case 3:
            return {p, q, 1.0f};

        case 4:
            return {t, p, 1.0f};

        default:
            return {1.0f, p, q};
    }
}


/*
 * RE reference uses:
 *
 *     clock_gettime(CLOCK_MONOTONIC)
 *     seconds * 0.5
 *     * 6
 *     fmod(..., 6)
 *
 * which gives a complete 6-phase RGB cycle in roughly
 * two seconds.
 */
double monotonicSeconds() {
    timespec ts{};

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &ts
        ) != 0
    ) {
        return 0.0;
    }

    return
        static_cast<double>(
            ts.tv_sec
        ) +
        static_cast<double>(
            ts.tv_nsec
        ) * 1.0e-9;
}


/*
 * Generates the exact style of RGB transition used in
 * the reference module.
 */
std::array<float, 4> getOutlineColor() {

    if (!g_module) {
        return {
            1.0f,
            1.0f,
            1.0f,
            1.0f
        };
    }


    const float alpha =
        std::clamp(
            g_module->alpha,
            0.0f,
            1.0f
        );


    /*
     * Static/custom colour mode.
     */
    if (!g_module->rgbCycle) {
        return {
            std::clamp(
                g_module->red,
                0.0f,
                1.0f
            ),

            std::clamp(
                g_module->green,
                0.0f,
                1.0f
            ),

            std::clamp(
                g_module->blue,
                0.0f,
                1.0f
            ),

            alpha
        };
    }


    /*
     * 2-second RGB cycle.
     */
    const float phase =
        static_cast<float>(
            std::fmod(
                monotonicSeconds() * 0.5 * 6.0,
                6.0
            )
        );

    const float hue =
        phase / 6.0f;

    const auto rgb =
        hsvToRgb(hue);


    return {
        rgb[0],
        rgb[1],
        rgb[2],
        alpha
    };
}


/*
 * Test whether the current uniform name is one of the
 * two exact reference outline uniforms.
 */
bool isOutlineUniform(
    std::string_view name
) {
    return
        name == kOutlineUniformA ||
        name == kOutlineUniformB;
}


/*
 * Scan one GL program for the exact outline uniform.
 *
 * The reference scans GL_ACTIVE_UNIFORMS and stores the
 * location for later glUniform4fv calls.
 */
ProgramInfo inspectProgram(
    GLuint program
) {
    ProgramInfo info{};

    if (
        !g_getProgramiv ||
        !g_getActiveUniform ||
        !g_getUniformLocation
    ) {
        info.inspected = true;
        return info;
    }


    GLint uniformCount = 0;

    g_getProgramiv(
        program,
        GL_ACTIVE_UNIFORMS,
        &uniformCount
    );


    if (
        uniformCount <= 0 ||
        uniformCount > 512
    ) {
        info.inspected = true;
        return info;
    }


    std::array<char, 256> nameBuffer{};


    for (
        GLint index = 0;
        index < uniformCount;
        ++index
    ) {

        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;


        g_getActiveUniform(
            program,
            static_cast<GLuint>(index),
            static_cast<GLsizei>(
                nameBuffer.size()
            ),
            &length,
            &size,
            &type,
            nameBuffer.data()
        );


        if (length <= 0)
            continue;


        const std::string_view name(
            nameBuffer.data(),
            static_cast<std::size_t>(
                length
            )
        );


        if (
            !isOutlineUniform(name)
        ) {
            continue;
        }


        const GLint location =
            g_getUniformLocation(
                program,
                nameBuffer.data()
            );


        if (location >= 0) {
            info.outlineLocation =
                location;

            /*
             * One of the two target uniforms is sufficient.
             */
            break;
        }
    }


    info.inspected = true;

    return info;
}


/*
 * Retrieve cached program info, inspecting only once.
 */
ProgramInfo getProgramInfo(
    GLuint program
) {
    {
        std::lock_guard lock(
            g_programMutex
        );

        const auto it =
            g_programs.find(program);

        if (
            it != g_programs.end()
        ) {
            return it->second;
        }
    }


    const ProgramInfo info =
        inspectProgram(program);


    {
        std::lock_guard lock(
            g_programMutex
        );

        if (g_programs.size() < 256) {
            g_programs.emplace(
                program,
                info
            );
        }
    }


    return info;
}


/*
 * glUseProgram detour.
 *
 * Reference behaviour:
 *
 *     save current program
 *     call original
 */
void useProgramHook(
    GLuint program
) {
    g_currentProgram =
        program;

    if (g_useProgramOriginal) {
        g_useProgramOriginal(
            program
        );
    }
}


/*
 * glEnable detour.
 *
 * Reference specifically tracks GL_DEPTH_TEST (0xB71).
 */
void enableHook(
    GLenum capability
) {
    if (
        capability ==
        GL_DEPTH_TEST
    ) {
        g_depthTestEnabled = true;
    }

    if (g_enableOriginal) {
        g_enableOriginal(
            capability
        );
    }
}


/*
 * glDisable detour.
 */
void disableHook(
    GLenum capability
) {
    if (
        capability ==
        GL_DEPTH_TEST
    ) {
        g_depthTestEnabled = false;
    }

    if (g_disableOriginal) {
        g_disableOriginal(
            capability
        );
    }
}


/*
 * glUniform4fv detour.
 *
 * Exact reference conditions reconstructed from RE:
 *
 *   1. outline enabled
 *   2. current program has a cached target location
 *   3. location == target location
 *   4. GL_DEPTH_TEST is enabled
 *   5. incoming RGB is below ~0.2
 *   6. incoming alpha is above ~0.5
 *
 * Then the outgoing vec4 is replaced with the configured
 * or RGB-cycled outline colour.
 */
void uniform4fvHook(
    GLint location,
    GLsizei count,
    const GLfloat* value
) {
    if (
        !g_uniform4fvOriginal ||
        !value ||
        count <= 0
    ) {
        if (g_uniform4fvOriginal) {
            g_uniform4fvOriginal(
                location,
                count,
                value
            );
        }

        return;
    }


    /*
     * Disabled = exact pass-through.
     */
    if (
        !g_module ||
        !g_module->enabled
    ) {
        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    /*
     * Outline master switch.
     */
    if (
        !g_module->masterEnabled ||
        !g_module->keybindActive
    ) {
        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    /*
     * Reference tracks depth-test state because the
     * outline pass is distinguished from other uniform
     * updates by this GL state.
     */
    if (!g_depthTestEnabled) {
        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    const ProgramInfo info =
        getProgramInfo(
            g_currentProgram
        );


    if (
        info.outlineLocation < 0 ||
        location !=
            info.outlineLocation
    ) {
        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    /*
     * The reference checks the first vec4 only.
     */
    const float r = value[0];
    const float g = value[1];
    const float b = value[2];
    const float a = value[3];


    constexpr float kRgbThreshold =
        0.2f;

    if (
        r >= kRgbThreshold ||
        g >= kRgbThreshold ||
        b >= kRgbThreshold ||
        a <= 0.5f
    ) {
        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    const auto color =
        getOutlineColor();


    /*
     * Preserve array elements when count > 1.
     *
     * The normal Bedrock path is a single vec4, but we
     * avoid corrupting an array uniform.
     */
    if (count == 1) {

        g_uniform4fvOriginal(
            location,
            1,
            color.data()
        );

        return;
    }


    std::vector<GLfloat> modified(
        value,
        value +
            static_cast<std::size_t>(
                count
            ) * 4u
    );


    modified[0] = color[0];
    modified[1] = color[1];
    modified[2] = color[2];
    modified[3] = color[3];


    g_uniform4fvOriginal(
        location,
        count,
        modified.data()
    );
}


/*
 * Wait for libGLESv2.so.
 *
 * The reference waits roughly 500 ms between attempts and
 * makes a finite number of attempts.
 */
void glHookWorker() {
    constexpr int kAttempts = 19;

    constexpr auto kDelay =
        std::chrono::milliseconds(500);


    for (
        int attempt = 0;
        attempt < kAttempts;
        ++attempt
    ) {
        if (
            g_hooksInstalled.load(
                std::memory_order_acquire
            )
        ) {
            return;
        }


        void* gles =
            dlopen(
                "libGLESv2.so",
                RTLD_NOW | RTLD_NOLOAD
            );


        if (gles) {

            const auto useProgram =
                reinterpret_cast<
                    void*
                >(
                    dlsym(
                        gles,
                        "glUseProgram"
                    )
                );


            const auto uniform4fv =
                reinterpret_cast<
                    void*
                >(
                    dlsym(
                        gles,
                        "glUniform4fv"
                    )
                );


            const auto enable =
                reinterpret_cast<
                    void*
                >(
                    dlsym(
                        gles,
                        "glEnable"
                    )
                );


            const auto disable =
                reinterpret_cast<
                    void*
                >(
                    dlsym(
                        gles,
                        "glDisable"
                    )
                );


            g_getProgramiv =
                reinterpret_cast<
                    GlGetProgramivFn
                >(
                    dlsym(
                        gles,
                        "glGetProgramiv"
                    )
                );


            g_getActiveUniform =
                reinterpret_cast<
                    GlGetActiveUniformFn
                >(
                    dlsym(
                        gles,
                        "glGetActiveUniform"
                    )
                );


            g_getUniformLocation =
                reinterpret_cast<
                    GlGetUniformLocationFn
                >(
                    dlsym(
                        gles,
                        "glGetUniformLocation"
                    )
                );


            if (
                useProgram &&
                uniform4fv &&
                enable &&
                disable &&
                g_getProgramiv &&
                g_getActiveUniform &&
                g_getUniformLocation
            ) {

                auto useProgramHandle =
                    bedrocktools::hooks::install(
                        useProgram,
                        reinterpret_cast<void*>(
                            useProgramHook
                        ),
                        reinterpret_cast<void**>(
                            &g_useProgramOriginal
                        )
                    );


                if (!useProgramHandle) {
                    dlclose(gles);
                    std::this_thread::sleep_for(
                        kDelay
                    );
                    continue;
                }


                auto uniformHandle =
                    bedrocktools::hooks::install(
                        uniform4fv,
                        reinterpret_cast<void*>(
                            uniform4fvHook
                        ),
                        reinterpret_cast<void**>(
                            &g_uniform4fvOriginal
                        )
                    );


                if (!uniformHandle) {
                    bedrocktools::hooks::remove(
                        useProgramHandle
                    );

                    g_useProgramOriginal =
                        nullptr;

                    dlclose(gles);

                    std::this_thread::sleep_for(
                        kDelay
                    );

                    continue;
                }


                auto enableHandle =
                    bedrocktools::hooks::install(
                        enable,
                        reinterpret_cast<void*>(
                            enableHook
                        ),
                        reinterpret_cast<void**>(
                            &g_enableOriginal
                        )
                    );


                if (!enableHandle) {
                    bedrocktools::hooks::remove(
                        uniformHandle
                    );

                    bedrocktools::hooks::remove(
                        useProgramHandle
                    );

                    g_uniform4fvOriginal =
                        nullptr;

                    g_useProgramOriginal =
                        nullptr;

                    dlclose(gles);

                    std::this_thread::sleep_for(
                        kDelay
                    );

                    continue;
                }


                auto disableHandle =
                    bedrocktools::hooks::install(
                        disable,
                        reinterpret_cast<void*>(
                            disableHook
                        ),
                        reinterpret_cast<void**>(
                            &g_disableOriginal
                        )
                    );


                if (!disableHandle) {
                    bedrocktools::hooks::remove(
                        enableHandle
                    );

                    bedrocktools::hooks::remove(
                        uniformHandle
                    );

                    bedrocktools::hooks::remove(
                        useProgramHandle
                    );

                    g_enableOriginal =
                        nullptr;

                    g_uniform4fvOriginal =
                        nullptr;

                    g_useProgramOriginal =
                        nullptr;

                    dlclose(gles);

                    std::this_thread::sleep_for(
                        kDelay
                    );

                    continue;
                }


                g_useProgramHook =
                    useProgramHandle;

                g_uniform4fvHook =
                    uniformHandle;

                g_enableHook =
                    enableHandle;

                g_disableHook =
                    disableHandle;


                g_hooksInstalled.store(
                    true,
                    std::memory_order_release
                );


                dlclose(gles);

                return;
            }


            dlclose(gles);
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
        glHookWorker
    ).detach();
}

} // namespace


OutlineRGBModule::OutlineRGBModule()
    : Module(
        "Outline RGB",
        "Changes the Bedrock block-outline colour and optionally cycles RGB."
    ) {
    g_module = this;
}


OutlineRGBModule::~OutlineRGBModule() {
    /*
     * The hook remains installed for the runtime lifetime;
     * when disabled it simply forwards to the original GL calls.
     */
    if (g_module == this)
        g_module = nullptr;
}


void OutlineRGBModule::onInit() {
    /*
     * IMPORTANT:
     *
     * Do not block ModuleRegistry::initialize().
     */
    startWorker();
}


void OutlineRGBModule::onEnable() {
}


void OutlineRGBModule::onDisable() {
}


void OutlineRGBModule::loadConfig(
    const nlohmann::json& j
) {
    Module::loadConfig(j);

    if (j.contains("rgbCycle"))
        rgbCycle =
            j["rgbCycle"].get<bool>();

    if (j.contains("red"))
        red =
            std::clamp(
                j["red"].get<float>(),
                0.0f,
                1.0f
            );

    if (j.contains("green"))
        green =
            std::clamp(
                j["green"].get<float>(),
                0.0f,
                1.0f
            );

    if (j.contains("blue"))
        blue =
            std::clamp(
                j["blue"].get<float>(),
                0.0f,
                1.0f
            );

    if (j.contains("alpha"))
        alpha =
            std::clamp(
                j["alpha"].get<float>(),
                0.0f,
                1.0f
            );
}


void OutlineRGBModule::saveConfig(
    nlohmann::json& j
) {
    Module::saveConfig(j);

    j["rgbCycle"] = rgbCycle;
    j["red"] = red;
    j["green"] = green;
    j["blue"] = blue;
    j["alpha"] = alpha;
}
