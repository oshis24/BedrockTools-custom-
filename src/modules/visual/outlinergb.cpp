#include "outlinergb.hpp"

#include "core/memory/Hooks.hpp"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>


namespace {


using GlUseProgramFn =
    void (*)(GLuint);


using GlUniform4fvFn =
    void (*)(GLint, GLsizei, const GLfloat*);


using GlEnableFn =
    void (*)(GLenum);


using GlDisableFn =
    void (*)(GLenum);


using GlDrawElementsInstancedFn =
    void (*)(
        GLenum,
        GLsizei,
        GLenum,
        const void*,
        GLsizei
    );


using GlGetProgramivFn =
    void (*)(
        GLuint,
        GLenum,
        GLint*
    );


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
    GLint (*)(
        GLuint,
        const GLchar*
    );


OutlineRGBModule* g_module = nullptr;


GlUseProgramFn
    g_useProgramOriginal = nullptr;


GlUniform4fvFn
    g_uniform4fvOriginal = nullptr;


GlEnableFn
    g_enableOriginal = nullptr;


GlDisableFn
    g_disableOriginal = nullptr;


GlDrawElementsInstancedFn
    g_drawElementsInstancedOriginal = nullptr;


GlGetProgramivFn
    g_getProgramiv = nullptr;


GlGetActiveUniformFn
    g_getActiveUniform = nullptr;


GlGetUniformLocationFn
    g_getUniformLocation = nullptr;


bedrocktools::hooks::Handle
    g_useProgramHook = nullptr;


bedrocktools::hooks::Handle
    g_uniform4fvHook = nullptr;


bedrocktools::hooks::Handle
    g_enableHook = nullptr;


bedrocktools::hooks::Handle
    g_disableHook = nullptr;


bedrocktools::hooks::Handle
    g_drawElementsInstancedHook = nullptr;


std::atomic_bool
    g_workerStarted{false};


std::atomic_bool
    g_hooksInstalled{false};


thread_local GLuint
    g_currentProgram = 0;


thread_local bool
    g_depthTestEnabled = false;


/*
 * Reference target uniforms.
 *
 * THESE NAMES ARE IMPORTANT.
 */
constexpr std::string_view
    kFogAndDistanceControl =
        "FogAndDistanceControl";


constexpr std::string_view
    kRenderChunkFogAlpha =
        "RenderChunkFogAlpha";


struct ProgramInfo {

    bool inspected = false;

    GLint fogAndDistanceLocation = -1;

    GLint renderChunkFogAlphaLocation = -1;
};


std::mutex g_programMutex;


std::unordered_map<
    GLuint,
    ProgramInfo
> g_programs;


/*
 * Reference uses clock_gettime(CLOCK_MONOTONIC).
 */
float monotonicSeconds() {

    timespec ts{};


    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &ts
        ) != 0
    ) {
        return 0.0f;
    }


    return
        static_cast<float>(
            ts.tv_sec
        ) +
        static_cast<float>(
            ts.tv_nsec
        ) *
            1.0e-9f;
}


/*
 * Reference RGB generator.
 *
 * It divides the cycle into six phases.
 */
std::array<float, 3>
referenceRgbCycle() {

    const float value =
        std::fmod(
            monotonicSeconds() *
                0.5f *
                6.0f,
            6.0f
        );


    const int phase =
        static_cast<int>(
            std::floor(value)
        );


    const float f =
        value -
        static_cast<float>(phase);


    switch (phase) {

        case 0:
            return {
                1.0f,
                0.0f,
                f
            };


        case 1:
            return {
                1.0f - f,
                1.0f,
                1.0f
            };


        case 2:
            return {
                0.0f,
                1.0f,
                f
            };


        case 3:
            return {
                0.0f,
                1.0f - f,
                1.0f
            };


        case 4:
            return {
                f,
                0.0f,
                1.0f
            };


        default:
            return {
                1.0f,
                0.0f,
                1.0f - f
            };
    }
}


/*
 * Get configured outline colour.
 */
std::array<float, 4>
getOutlineColor() {

    if (!g_module) {

        return {
            1.0f,
            1.0f,
            1.0f,
            1.0f
        };
    }


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

            std::clamp(
                g_module->alpha,
                0.0f,
                1.0f
            )
        };
    }


    const auto rgb =
        referenceRgbCycle();


    return {

        rgb[0],
        rgb[1],
        rgb[2],

        std::clamp(
            g_module->alpha,
            0.0f,
            1.0f
        )
    };
}


/*
 * Inspect one shader program.
 *
 * Reference first verifies GL_LINK_STATUS and then
 * enumerates GL_ACTIVE_UNIFORMS.
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
        return info;
    }


    GLint linked = 0;


    g_getProgramiv(
        program,
        GL_LINK_STATUS,
        &linked
    );


    if (!linked) {
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


    std::array<char, 128>
        nameBuffer{};


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


        const std::string_view
            name(
                nameBuffer.data(),
                static_cast<std::size_t>(
                    length
                )
            );


        if (
            name ==
            kFogAndDistanceControl
        ) {

            info.fogAndDistanceLocation =
                g_getUniformLocation(
                    program,
                    nameBuffer.data()
                );

            continue;
        }


        if (
            name ==
            kRenderChunkFogAlpha
        ) {

            info.renderChunkFogAlphaLocation =
                g_getUniformLocation(
                    program,
                    nameBuffer.data()
                );
        }
    }


    info.inspected = true;

    return info;
}


/*
 * Get cached shader information.
 */
ProgramInfo getProgramInfo(
    GLuint program
) {

    {
        std::lock_guard<std::mutex>
            lock(g_programMutex);


        const auto it =
            g_programs.find(program);


        if (
            it != g_programs.end()
        ) {
            return it->second;
        }
    }


    ProgramInfo info =
        inspectProgram(program);


    {
        std::lock_guard<std::mutex>
            lock(g_programMutex);


        if (
            g_programs.size() < 512
        ) {
            g_programs.emplace(
                program,
                info
            );
        }
    }


    return info;
}


/*
 * glUseProgram
 */
void useProgramHook(
    GLuint program
) {

    if (g_useProgramOriginal) {

        g_useProgramOriginal(
            program
        );
    }


    /*
     * Reference stores the current program.
     */
    g_currentProgram =
        program;


    /*
     * Inspecting is cheap because every program is cached.
     *
     * We inspect even before the module is enabled so that
     * enabling the module later does not miss already-created
     * shader programs.
     */
    if (program != 0) {

        (void)getProgramInfo(
            program
        );
    }
}


/*
 * glEnable
 *
 * Reference specifically tracks GL_DEPTH_TEST.
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
 * glDisable
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
 * glDrawElementsInstanced
 *
 * RE shows that the reference callback forwards this call.
 *
 * We retain the hook because the original module installs it
 * as part of the same GL hook group.
 */
void drawElementsInstancedHook(
    GLenum mode,
    GLsizei count,
    GLenum type,
    const void* indices,
    GLsizei instancecount
) {

    if (
        g_drawElementsInstancedOriginal
    ) {

        g_drawElementsInstancedOriginal(
            mode,
            count,
            type,
            indices,
            instancecount
        );
    }
}


/*
 * glUniform4fv
 */
void uniform4fvHook(
    GLint location,
    GLsizei count,
    const GLfloat* value
) {

    if (
        !g_uniform4fvOriginal
    ) {
        return;
    }


    /*
     * Transparent pass-through.
     */
    if (
        !g_module ||
        !g_module->enabled ||
        !value ||
        count <= 0
    ) {

        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    /*
     * Reference only modifies the relevant depth-tested
     * rendering state.
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


    const bool isFogControl =
        (
            info.fogAndDistanceLocation >= 0 &&
            location ==
                info.fogAndDistanceLocation
        );


    const bool isChunkFogAlpha =
        (
            info.renderChunkFogAlphaLocation >= 0 &&
            location ==
                info.renderChunkFogAlphaLocation
        );


    if (
        !isFogControl &&
        !isChunkFogAlpha
    ) {

        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    const float r = value[0];

    const float g = value[1];

    const float b = value[2];

    const float a = value[3];


    bool matches =
        false;


    /*
     * Reference path for FogAndDistanceControl:
     *
     * RGB < 0.4
     * alpha > 0.5
     */
    if (isFogControl) {

        matches =
            r < 0.4f &&
            g < 0.4f &&
            b < 0.4f &&
            a > 0.5f;
    }


    /*
     * Reference path for RenderChunkFogAlpha:
     *
     * RGB < 0.01
     * 0.2 < alpha <= 0.3
     */
    if (isChunkFogAlpha) {

        matches =
            r < 0.01f &&
            g < 0.01f &&
            b < 0.01f &&
            a > 0.2f &&
            a <= 0.3f;
    }


    if (!matches) {

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
     * Normal Bedrock path = one vec4.
     */
    if (count == 1) {

        g_uniform4fvOriginal(
            location,
            1,
            color.data()
        );

        return;
    }


    /*
     * Preserve array elements if count > 1.
     */
    std::vector<GLfloat>
        modified(
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
 * Install all GL hooks.
 *
 * The reference resolves libGLESv2.so only after Minecraft's
 * native library is present.
 */
void glHookWorker() {

    constexpr int kMaxAttempts = 100;

    constexpr auto kDelay =
        std::chrono::milliseconds(100);


    for (
        int attempt = 0;
        attempt < kMaxAttempts;
        ++attempt
    ) {

        if (
            g_hooksInstalled.load(
                std::memory_order_acquire
            )
        ) {
            return;
        }


        void* minecraft =
            dlopen(
                "libminecraftpe.so",
                RTLD_NOW | RTLD_NOLOAD
            );


        if (!minecraft) {

            std::this_thread::sleep_for(
                kDelay
            );

            continue;
        }


        dlclose(minecraft);


        void* gles =
            dlopen(
                "libGLESv2.so",
                RTLD_NOW | RTLD_NOLOAD
            );


        if (!gles) {

            std::this_thread::sleep_for(
                kDelay
            );

            continue;
        }


        const auto useProgram =
            dlsym(
                gles,
                "glUseProgram"
            );


        const auto uniform4fv =
            dlsym(
                gles,
                "glUniform4fv"
            );


        const auto enable =
            dlsym(
                gles,
                "glEnable"
            );


        const auto disable =
            dlsym(
                gles,
                "glDisable"
            );


        const auto drawElementsInstanced =
            dlsym(
                gles,
                "glDrawElementsInstanced"
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
            !useProgram ||
            !uniform4fv ||
            !enable ||
            !disable ||
            !drawElementsInstanced ||
            !g_getProgramiv ||
            !g_getActiveUniform ||
            !g_getUniformLocation
        ) {

            dlclose(gles);

            std::this_thread::sleep_for(
                kDelay
            );

            continue;
        }


        /*
         * Install in the same five-function group as reference.
         */
        g_useProgramHook =
            bedrocktools::hooks::install(
                useProgram,
                reinterpret_cast<void*>(
                    useProgramHook
                ),
                reinterpret_cast<void**>(
                    &g_useProgramOriginal
                )
            );


        g_uniform4fvHook =
            bedrocktools::hooks::install(
                uniform4fv,
                reinterpret_cast<void*>(
                    uniform4fvHook
                ),
                reinterpret_cast<void**>(
                    &g_uniform4fvOriginal
                )
            );


        g_enableHook =
            bedrocktools::hooks::install(
                enable,
                reinterpret_cast<void*>(
                    enableHook
                ),
                reinterpret_cast<void**>(
                    &g_enableOriginal
                )
            );


        g_disableHook =
            bedrocktools::hooks::install(
                disable,
                reinterpret_cast<void*>(
                    disableHook
                ),
                reinterpret_cast<void**>(
                    &g_disableOriginal
                )
            );


        g_drawElementsInstancedHook =
            bedrocktools::hooks::install(
                drawElementsInstanced,
                reinterpret_cast<void*>(
                    drawElementsInstancedHook
                ),
                reinterpret_cast<void**>(
                    &g_drawElementsInstancedOriginal
                )
            );


        const bool success =
            g_useProgramHook &&
            g_uniform4fvHook &&
            g_enableHook &&
            g_disableHook &&
            g_drawElementsInstancedHook &&
            g_useProgramOriginal &&
            g_uniform4fvOriginal &&
            g_enableOriginal &&
            g_disableOriginal &&
            g_drawElementsInstancedOriginal;


        if (success) {

            g_hooksInstalled.store(
                true,
                std::memory_order_release
            );

            dlclose(gles);

            return;
        }


        /*
         * Roll back partial installation.
         */
        if (g_drawElementsInstancedHook) {
            bedrocktools::hooks::remove(
                g_drawElementsInstancedHook
            );
            g_drawElementsInstancedHook =
                nullptr;
        }


        if (g_disableHook) {
            bedrocktools::hooks::remove(
                g_disableHook
            );
            g_disableHook =
                nullptr;
        }


        if (g_enableHook) {
            bedrocktools::hooks::remove(
                g_enableHook
            );
            g_enableHook =
                nullptr;
        }


        if (g_uniform4fvHook) {
            bedrocktools::hooks::remove(
                g_uniform4fvHook
            );
            g_uniform4fvHook =
                nullptr;
        }


        if (g_useProgramHook) {
            bedrocktools::hooks::remove(
                g_useProgramHook
            );
            g_useProgramHook =
                nullptr;
        }


        g_drawElementsInstancedOriginal =
            nullptr;

        g_disableOriginal =
            nullptr;

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
        "Changes the compatible Bedrock outline shader colour."
    ) {

    g_module = this;
}


OutlineRGBModule::~OutlineRGBModule() {

    if (g_module == this) {
        g_module = nullptr;
    }
}


void OutlineRGBModule::onInit() {

    /*
     * Do not block ModuleRegistry initialization.
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


    j["rgbCycle"] =
        rgbCycle;


    j["red"] =
        red;


    j["green"] =
        green;


    j["blue"] =
        blue;


    j["alpha"] =
        alpha;
}
