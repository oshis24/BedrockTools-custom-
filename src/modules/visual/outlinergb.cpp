#include "outlinergb.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>

namespace {

using Tessellator_begin_t =
    void (*)(void* tessellator,
             void* debugCallback,
             int primitiveMode,
             int vertexCount,
             int noIndices);

using Tessellator_color_t =
    void (*)(void* tessellator,
             float r,
             float g,
             float b,
             float a);

using Tessellator_vertex_t =
    void (*)(void* tessellator,
             float x,
             float y,
             float z);

using MeshHelpers_renderMeshImmediately_t =
    void (*)(void* screenContext,
             void* tessellator,
             void* material,
             char* pad);

using BlockOutlineRender_t =
    void (*)(
        void*,
        void*,
        void*,
        void*,
        void*,
        std::uintptr_t,
        std::uintptr_t
    );

struct BlockPos {
    int x;
    int y;
    int z;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Color {
    float r;
    float g;
    float b;
    float a;
};

OutlineRGBModule* g_module = nullptr;

BlockOutlineRender_t g_blockOutlineOriginal = nullptr;

Tessellator_begin_t g_tessBegin = nullptr;
Tessellator_color_t g_tessColor = nullptr;
Tessellator_vertex_t g_tessVertex = nullptr;

MeshHelpers_renderMeshImmediately_t g_renderMesh = nullptr;

bedrocktools::hooks::Handle g_blockOutlineHook = nullptr;

bool g_initialized = false;


static double monotonicSeconds() {
    timespec ts{};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;

    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) * 1.0e-9;
}


static Color hsvToRgb(
    float h,
    float s,
    float v,
    float a
) {
    h = h - std::floor(h);

    const float scaled = h * 6.0f;

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
        v * (1.0f - s * fraction);

    const float t =
        v * (1.0f - s * (1.0f - fraction));

    switch (sector % 6) {
        case 0:
            return {v, t, p, a};

        case 1:
            return {q, v, p, a};

        case 2:
            return {p, v, t, a};

        case 3:
            return {p, q, v, a};

        case 4:
            return {t, p, v, a};

        default:
            return {v, p, q, a};
    }
}


static Color getOutlineColor() {
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

    const float speed =
        std::max(
            0.01f,
            g_module->rgbSpeed
        );

    const float hue =
        static_cast<float>(
            std::fmod(
                monotonicSeconds() *
                    speed *
                    0.25,
                1.0
            )
        );

    return hsvToRgb(
        hue,
        1.0f,
        1.0f,
        alpha
    );
}


static inline void vertex(
    void* tessellator,
    const Vec3& p
) {
    if (!g_tessVertex)
        return;

    g_tessVertex(
        tessellator,
        p.x,
        p.y,
        p.z
    );
}


static void drawFace(
    void* screenContext,
    void* tessellator,
    void* material,
    const std::array<Vec3, 4>& face,
    const Color& color,
    float camX,
    float camY,
    float camZ
) {
    if (!g_tessBegin ||
        !g_tessColor ||
        !g_tessVertex ||
        !g_renderMesh) {
        return;
    }

    /*
     * Primitive mode 1 = line list.
     *
     * 4 edges × 2 vertices = 8 vertices.
     */
    g_tessBegin(
        tessellator,
        nullptr,
        1,
        8,
        0
    );

    g_tessColor(
        tessellator,
        color.r,
        color.g,
        color.b,
        color.a
    );

    for (int i = 0; i < 4; ++i) {
        const Vec3& a =
            face[i];

        const Vec3& b =
            face[(i + 1) & 3];

        /*
         * Convert world coordinates into camera-relative
         * coordinates.
         */
        const Vec3 aRelative{
            a.x - camX,
            a.y - camY,
            a.z - camZ
        };

        const Vec3 bRelative{
            b.x - camX,
            b.y - camY,
            b.z - camZ
        };

        vertex(
            tessellator,
            aRelative
        );

        vertex(
            tessellator,
            bRelative
        );
    }

    /*
     * Existing BedrockTools rendering code uses the
     * immediate RenderMesh path.
     */
    char pad[0x58]{};

    g_renderMesh(
        screenContext,
        tessellator,
        material,
        pad
    );
}


static void drawBlockOutline(
    void* screenContext,
    void* tessellator,
    void* material,
    const BlockPos& block,
    float camX,
    float camY,
    float camZ,
    const Color& color,
    int thickness
) {
    if (!screenContext ||
        !tessellator ||
        !material) {
        return;
    }

    thickness =
        std::clamp(
            thickness,
            1,
            5
        );

    const float minX =
        static_cast<float>(block.x);

    const float minY =
        static_cast<float>(block.y);

    const float minZ =
        static_cast<float>(block.z);

    const float maxX =
        minX + 1.0f;

    const float maxY =
        minY + 1.0f;

    const float maxZ =
        minZ + 1.0f;

    /*
     * Small geometry offset used to simulate line thickness.
     *
     * We intentionally do not use glLineWidth because
     * Android/Mali implementations commonly clamp it.
     */
    const float spacing =
        0.006f *
        static_cast<float>(thickness);

    for (int layer = 0;
         layer < thickness;
         ++layer) {

        const float center =
            static_cast<float>(layer) -
            static_cast<float>(
                thickness - 1
            ) * 0.5f;

        const float o =
            center * spacing;

        /*
         * Bottom.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{
                    minX,
                    minY + o,
                    minZ + o
                },

                Vec3{
                    maxX,
                    minY + o,
                    minZ + o
                },

                Vec3{
                    maxX,
                    minY + o,
                    maxZ
                },

                Vec3{
                    minX,
                    minY + o,
                    maxZ
                }
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * Top.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{
                    minX,
                    maxY + o,
                    minZ + o
                },

                Vec3{
                    maxX,
                    maxY + o,
                    minZ + o
                },

                Vec3{
                    maxX,
                    maxY + o,
                    maxZ
                },

                Vec3{
                    minX,
                    maxY + o,
                    maxZ
                }
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * Front.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{
                    minX,
                    minY + o,
                    minZ
                },

                Vec3{
                    maxX,
                    minY + o,
                    minZ
                },

                Vec3{
                    maxX,
                    maxY + o,
                    minZ
                },

                Vec3{
                    minX,
                    maxY + o,
                    minZ
                }
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * Back.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{
                    minX,
                    minY + o,
                    maxZ
                },

                Vec3{
                    maxX,
                    minY + o,
                    maxZ
                },

                Vec3{
                    maxX,
                    maxY + o,
                    maxZ
                },

                Vec3{
                    minX,
                    maxY + o,
                    maxZ
                }
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * Left.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{
                    minX + o,
                    minY,
                    minZ
                },

                Vec3{
                    minX + o,
                    minY,
                    maxZ
                },

                Vec3{
                    minX + o,
                    maxY,
                    maxZ
                },

                Vec3{
                    minX + o,
                    maxY,
                    minZ
                }
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * Right.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{
                    maxX + o,
                    minY,
                    minZ
                },

                Vec3{
                    maxX + o,
                    minY,
                    maxZ
                },

                Vec3{
                    maxX + o,
                    maxY,
                    maxZ
                },

                Vec3{
                    maxX + o,
                    maxY,
                    minZ
                }
            },
            color,
            camX,
            camY,
            camZ
        );
    }
}


static void blockOutlineHook(
    void* levelRenderer,
    void* screenContext,
    void* x2,
    void* x3,
    void* blockPosPtr,
    std::uintptr_t x5,
    std::uintptr_t x6
) {
    /*
     * Module disabled:
     * completely preserve vanilla behaviour.
     */
    if (!g_module ||
        !g_module->enabled) {

        if (g_blockOutlineOriginal) {
            g_blockOutlineOriginal(
                levelRenderer,
                screenContext,
                x2,
                x3,
                blockPosPtr,
                x5,
                x6
            );
        }

        return;
    }

    /*
     * Basic pointer validation.
     */
    if (!levelRenderer ||
        reinterpret_cast<std::uintptr_t>(
            levelRenderer
        ) < 0x1000 ||

        !screenContext ||
        reinterpret_cast<std::uintptr_t>(
            screenContext
        ) < 0x1000 ||

        !blockPosPtr ||
        reinterpret_cast<std::uintptr_t>(
            blockPosPtr
        ) < 0x1000) {

        if (g_blockOutlineOriginal) {
            g_blockOutlineOriginal(
                levelRenderer,
                screenContext,
                x2,
                x3,
                blockPosPtr,
                x5,
                x6
            );
        }

        return;
    }

    /*
     * Required functions must exist.
     */
    if (!g_tessBegin ||
        !g_tessColor ||
        !g_tessVertex ||
        !g_renderMesh) {

        if (g_blockOutlineOriginal) {
            g_blockOutlineOriginal(
                levelRenderer,
                screenContext,
                x2,
                x3,
                blockPosPtr,
                x5,
                x6
            );
        }

        return;
    }

    /*
     * ThickBaddie passes the BlockPos through x4.
     */
    const BlockPos block =
        *reinterpret_cast<const BlockPos*>(
            blockPosPtr
        );

    /*
     * ScreenContext::mTessellator = 0xB8.
     */
    const auto screen =
        reinterpret_cast<std::uintptr_t>(
            screenContext
        );

    const auto tessellatorPtr =
        *reinterpret_cast<std::uintptr_t*>(
            screen +
            bedrocktools::sdk::offsets::
                ScreenContext::mTessellator
        );

    if (!tessellatorPtr ||
        tessellatorPtr < 0x1000) {

        if (g_blockOutlineOriginal) {
            g_blockOutlineOriginal(
                levelRenderer,
                screenContext,
                x2,
                x3,
                blockPosPtr,
                x5,
                x6
            );
        }

        return;
    }

    void* tessellator =
        reinterpret_cast<void*>(
            tessellatorPtr
        );

    /*
     * LevelRenderer::mLevelRendererPlayer = 0x420.
     */
    const auto renderer =
        reinterpret_cast<std::uintptr_t>(
            levelRenderer
        );

    const auto lrpPtr =
        *reinterpret_cast<std::uintptr_t*>(
            renderer +
            bedrocktools::sdk::offsets::
                LevelRenderer::
                    mLevelRendererPlayer
        );

    if (!lrpPtr ||
        lrpPtr < 0x1000) {

        if (g_blockOutlineOriginal) {
            g_blockOutlineOriginal(
                levelRenderer,
                screenContext,
                x2,
                x3,
                blockPosPtr,
                x5,
                x6
            );
        }

        return;
    }

    /*
     * LevelRendererPlayer::mCamPos = 0x61C.
     */
    const auto cam =
        reinterpret_cast<const float*>(
            lrpPtr +
            bedrocktools::sdk::offsets::
                LevelRendererPlayer::
                    mCamPos
        );

    const float camX = cam[0];
    const float camY = cam[1];
    const float camZ = cam[2];

    /*
     * Selection overlay material.
     */
    void* material =
        reinterpret_cast<void*>(
            lrpPtr +
            bedrocktools::sdk::offsets::
                LevelRendererPlayer::
                    mSelectionOverlayMaterial
        );

    if (!material) {

        if (g_blockOutlineOriginal) {
            g_blockOutlineOriginal(
                levelRenderer,
                screenContext,
                x2,
                x3,
                blockPosPtr,
                x5,
                x6
            );
        }

        return;
    }

    /*
     * Generate RGB/chroma color.
     */
    const Color color =
        getOutlineColor();

    /*
     * Draw the complete 3D outline.
     */
    drawBlockOutline(
        screenContext,
        tessellator,
        material,
        block,
        camX,
        camY,
        camZ,
        color,
        g_module->thickness
    );
}

} // namespace


OutlineRGBModule*
    OutlineRGBModule::instance =
        nullptr;


OutlineRGBModule::OutlineRGBModule()
    : Module(
        "Outline RGB",
        "RGB 3D block outline with adjustable thickness."
    ) {
    showInMenu = true;

    instance = this;
    g_module = this;
}


OutlineRGBModule::~OutlineRGBModule() {
    if (instance == this)
        instance = nullptr;

    if (g_module == this)
        g_module = nullptr;
}


void OutlineRGBModule::onInit() {
    if (g_initialized)
        return;

    /*
     * ThickBaddie target:
     *
     * FD 7B BA A9 ...
     *
     * resolved through SignatureId::BlockOutlineRender.
     */
    const auto target =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                BlockOutlineRender
        );

    if (!target)
        return;

    m_patchTarget =
        reinterpret_cast<void*>(
            target
        );

    /*
     * TessellatorBegin.
     */
    const auto begin =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                TessellatorBegin
        );

    /*
     * TessellatorColor.
     */
    const auto color =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                TessellatorColor
        );

    /*
     * TessellatorVertex.
     */
    const auto vertexFn =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                TessellatorVertex
        );

    g_tessBegin =
        reinterpret_cast<
            Tessellator_begin_t
        >(begin);

    g_tessColor =
        reinterpret_cast<
            Tessellator_color_t
        >(color);

    g_tessVertex =
        reinterpret_cast<
            Tessellator_vertex_t
        >(vertexFn);

    /*
     * RenderMesh.
     *
     * Prefer the newer signature if available.
     */
    auto renderMesh =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                MeshHelpersRenderMeshImmediately2
        );

    if (!renderMesh) {
        renderMesh =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    MeshHelpersRenderMeshImmediately
            );
    }

    g_renderMesh =
        reinterpret_cast<
            MeshHelpers_renderMeshImmediately_t
        >(renderMesh);

    g_initialized =
        m_patchTarget != nullptr &&
        g_tessBegin != nullptr &&
        g_tessColor != nullptr &&
        g_tessVertex != nullptr &&
        g_renderMesh != nullptr;
}


bool OutlineRGBModule::installHooks() {
    if (m_patched)
        return true;

    if (!m_patchTarget)
        return false;

    if (!g_initialized)
        return false;

    g_blockOutlineHook =
        bedrocktools::hooks::install(
            m_patchTarget,
            reinterpret_cast<void*>(
                blockOutlineHook
            ),
            reinterpret_cast<void**>(
                &g_blockOutlineOriginal
            )
        );

    if (!g_blockOutlineHook)
        return false;

    m_patched = true;

    return true;
}


void OutlineRGBModule::onEnable() {
    installHooks();
}


void OutlineRGBModule::onDisable() {
    /*
     * Hook remains installed.
     *
     * The hook calls the original function when
     * the module is disabled.
     */
}


void OutlineRGBModule::onFrame() {
    rgbSpeed =
        std::clamp(
            rgbSpeed,
            0.01f,
            5.0f
        );

    alpha =
        std::clamp(
            alpha,
            0.0f,
            1.0f
        );

    red =
        std::clamp(
            red,
            0.0f,
            1.0f
        );

    green =
        std::clamp(
            green,
            0.0f,
            1.0f
        );

    blue =
        std::clamp(
            blue,
            0.0f,
            1.0f
        );

    thickness =
        std::clamp(
            thickness,
            1,
            5
        );
}


void OutlineRGBModule::loadConfig(
    const nlohmann::json& j
) {
    Module::loadConfig(j);

    rgbCycle =
        j.value(
            "rgbCycle",
            rgbCycle
        );

    red =
        j.value(
            "red",
            red
        );

    green =
        j.value(
            "green",
            green
        );

    blue =
        j.value(
            "blue",
            blue
        );

    alpha =
        j.value(
            "alpha",
            alpha
        );

    rgbSpeed =
        j.value(
            "rgbSpeed",
            rgbSpeed
        );

    thickness =
        j.value(
            "thickness",
            thickness
        );

    red =
        std::clamp(
            red,
            0.0f,
            1.0f
        );

    green =
        std::clamp(
            green,
            0.0f,
            1.0f
        );

    blue =
        std::clamp(
            blue,
            0.0f,
            1.0f
        );

    alpha =
        std::clamp(
            alpha,
            0.0f,
            1.0f
        );

    rgbSpeed =
        std::clamp(
            rgbSpeed,
            0.01f,
            5.0f
        );

    thickness =
        std::clamp(
            thickness,
            1,
            5
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

    j["rgbSpeed"] =
        rgbSpeed;

    j["thickness"] =
        thickness;
}
