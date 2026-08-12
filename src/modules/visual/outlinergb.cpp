#include "outlinergb.hpp"

#include "core/memory/Hooks.hpp"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <time.h>

namespace {

using GlUseProgramFn =
    void (*)(GLuint);

using GlUniform4fvFn =
    void (*)(GLint, GLsizei, const GLfloat*);

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

GlUseProgramFn g_useProgramOriginal = nullptr;
GlUniform4fvFn g_uniform4fvOriginal = nullptr;

GlGetProgramivFn g_getProgramiv = nullptr;
GlGetActiveUniformFn g_getActiveUniform = nullptr;
GlGetUniformLocationFn g_getUniformLocation = nullptr;

bedrocktools::hooks::Handle g_useProgramHook = nullptr;
bedrocktools::hooks::Handle g_uniform4fvHook = nullptr;

bool g_initialized = false;


/*
 * We keep a small cache because glUseProgram is called often,
 * while shader programs are comparatively few.
 */
struct ProgramInfo {
    bool inspected = false;
    bool likelyEntityShader = false;

    std::vector<GLint> colorLocations;
};

std::unordered_map<GLuint, ProgramInfo>
    g_programs;


/*
 * Current GL program is thread-local because all GL calls
 * relevant here are made on the rendering thread.
 */
thread_local GLuint
    g_currentProgram = 0;


/*
 * Read monotonic time.
 */
double monotonicSeconds() {
    timespec ts{};

    if (clock_gettime(
            CLOCK_MONOTONIC,
            &ts
        ) != 0) {
        return 0.0;
    }

    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) *
               1.0e-9;
}


/*
 * HSV → RGB.
 *
 * h: [0, 1)
 * s: [0, 1]
 * v: [0, 1]
 */
std::array<float, 3> hsvToRgb(
    float h,
    float s,
    float v
) {
    h = h - std::floor(h);

    const float scaled =
        h * 6.0f;

    const int sector =
        static_cast<int>(
            std::floor(scaled)
        );

    const float fraction =
        scaled -
        static_cast<float>(sector);

    const float p =
        v * (1.0f - s);

    const float q =
        v * (
            1.0f -
            s * fraction
        );

    const float t =
        v * (
            1.0f -
            s * (1.0f - fraction)
        );

    switch (sector % 6) {
        case 0:
            return {v, t, p};

        case 1:
            return {q, v, p};

        case 2:
            return {p, v, t};

        case 3:
            return {p, q, v};

        case 4:
            return {t, p, v};

        default:
            return {v, p, q};
    }
}


/*
 * Current outline colour.
 *
 * The cycle is intentionally slow enough to remain visible
 * while keeping the calculation effectively free.
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


    /*
     * Approximately one complete RGB cycle every 4 seconds.
     */
    const float hue =
        static_cast<float>(
            std::fmod(
                monotonicSeconds() * 0.25,
                1.0
            )
        );


    const auto rgb =
        hsvToRgb(
            hue,
            0.90f,
            1.0f
        );


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
 * Determine whether a GLSL uniform name should be treated
 * as a candidate model colour.
 */
bool isColorUniform(
    std::string_view name
) {
    return
        name == "Color" ||
        name == "MatColor";
}


/*
 * Determine whether a program looks like a Bedrock
 * entity/model shader.
 *
 * The old mod inspected uniforms such as Color,
 * MatColor, FogAndDistanceColor and Bones.
 *
 * We use those properties instead of hard-coding
 * program IDs, which are version dependent.
 */
ProgramInfo inspectProgram(
    GLuint program
) {
    ProgramInfo info{};

    if (!g_getProgramiv ||
        !g_getActiveUniform ||
        !g_getUniformLocation) {
        return info;
    }


    GLint uniformCount = 0;

    g_getProgramiv(
        program,
        GL_ACTIVE_UNIFORMS,
        &uniformCount
    );


    if (uniformCount <= 0 ||
        uniformCount > 512) {

        info.inspected = true;
        return info;
    }


    bool hasBones = false;
    bool hasFogDistance = false;
    bool hasMatColor = false;
    bool hasColor = false;


    std::array<char, 256> nameBuffer{};


    for (GLint index = 0;
         index < uniformCount;
         ++index) {

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
            static_cast<std::size_t>(length)
        );


        if (
            name == "Bones" ||
            name.rfind(
                "Bones[",
                0
            ) == 0
        ) {
            hasBones = true;
        }


        if (
            name ==
            "FogAndDistanceColor"
        ) {
            hasFogDistance = true;
        }


        if (name == "MatColor")
            hasMatColor = true;


        if (name == "Color")
            hasColor = true;


        if (isColorUniform(name)) {

            const GLint location =
                g_getUniformLocation(
                    program,
                    nameBuffer.data()
                );

            if (location >= 0) {
                info.colorLocations.push_back(
                    location
                );
            }
        }
    }


    /*
     * Conservative classification.
     *
     * We don't alter arbitrary GUI/particle shaders.
     */
    info.likelyEntityShader =
        (
            hasBones &&
            (hasColor || hasMatColor)
        ) ||
        (
            hasFogDistance &&
            hasColor
        );


    /*
     * If this program doesn't look like a model shader,
     * discard candidate color locations.
     */
    if (!info.likelyEntityShader)
        info.colorLocations.clear();


    info.inspected = true;

    return info;
}


/*
 * glUseProgram detour.
 */
void useProgramHook(
    GLuint program
) {
    if (g_useProgramOriginal) {

        g_useProgramOriginal(
            program
        );
    }


    g_currentProgram =
        program;


    if (!g_module ||
        !g_module->enabled) {
        return;
    }


    auto it =
        g_programs.find(program);


    if (it != g_programs.end())
        return;


    /*
     * Program inspection happens only once per program.
     */
    auto info =
        inspectProgram(program);


    if (g_programs.size() < 256) {

        g_programs.emplace(
            program,
            std::move(info)
        );
    }
}


/*
 * glUniform4fv detour.
 */
void uniform4fvHook(
    GLint location,
    GLsizei count,
    const GLfloat* value
) {
    if (!g_uniform4fvOriginal)
        return;


    if (!g_module ||
        !g_module->enabled ||
        !value ||
        count <= 0) {

        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    auto it =
        g_programs.find(
            g_currentProgram
        );


    if (
        it ==
        g_programs.end() ||
        !it->second.likelyEntityShader
    ) {

        g_uniform4fvOriginal(
            location,
            count,
            value
        );

        return;
    }


    bool isCandidate = false;

    for (const GLint candidate :
         it->second.colorLocations) {

        if (candidate == location) {
            isCandidate = true;
            break;
        }
    }


    if (!isCandidate) {

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
     * We only modify the first vec4. If the caller is
     * uploading an array, preserve the remainder by using
     * a temporary copy.
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
     * Array uniform case.
     */
    std::vector<GLfloat> modified(
        value,
        value +
            static_cast<std::size_t>(count) *
            4u
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


void installHooks() {

    if (g_initialized)
        return;


    auto gles =
        bedrocktools::hooks::openLibrary(
            "libGLESv2.so"
        );


    if (!gles)
        return;


    const auto useProgram =
        bedrocktools::hooks::symbol(
            gles,
            "glUseProgram"
        );


    const auto uniform4fv =
        bedrocktools::hooks::symbol(
            gles,
            "glUniform4fv"
        );


    g_getProgramiv =
        reinterpret_cast<
            GlGetProgramivFn
        >(
            bedrocktools::hooks::symbol(
                gles,
                "glGetProgramiv"
            )
        );


    g_getActiveUniform =
        reinterpret_cast<
            GlGetActiveUniformFn
        >(
            bedrocktools::hooks::symbol(
                gles,
                "glGetActiveUniform"
            )
        );


    g_getUniformLocation =
        reinterpret_cast<
            GlGetUniformLocationFn
        >(
            bedrocktools::hooks::symbol(
                gles,
                "glGetUniformLocation"
            )
        );


    if (!useProgram ||
        !uniform4fv ||
        !g_getProgramiv ||
        !g_getActiveUniform ||
        !g_getUniformLocation) {

        bedrocktools::hooks::closeLibrary(
            gles
        );

        return;
    }


    g_useProgramHook =
        bedrocktools::hooks::install(
            reinterpret_cast<void*>(
                useProgram
            ),
            reinterpret_cast<void*>(
                useProgramHook
            ),
            reinterpret_cast<void**>(
                &g_useProgramOriginal
            )
        );


    g_uniform4fvHook =
        bedrocktools::hooks::install(
            reinterpret_cast<void*>(
                uniform4fv
            ),
            reinterpret_cast<void*>(
                uniform4fvHook
            ),
            reinterpret_cast<void**>(
                &g_uniform4fvOriginal
            )
        );


    bedrocktools::hooks::closeLibrary(
        gles
    );


    g_initialized =
        g_useProgramHook != nullptr &&
        g_uniform4fvHook != nullptr &&
        g_useProgramOriginal != nullptr &&
        g_uniform4fvOriginal != nullptr;
}

} // namespace


OutlineRGBModule::OutlineRGBModule()
    : Module(
        "Outline RGB",
        "Applies an animated RGB colour to compatible entity model shaders."
    ) {
    g_module = this;
}


OutlineRGBModule::~OutlineRGBModule() {

    /*
     * Like the asset hook, leave the GL hooks installed
     * and make the detours transparent when disabled.
     */
    if (g_module == this)
        g_module = nullptr;
}


void OutlineRGBModule::onInit() {
    installHooks();
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
