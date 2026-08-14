#include "outlinergb.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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

/*
 * ThickBaddie target ABI.
 *
 * The function uses:
 *   x0 = LevelRenderer-like object
 *   x1 = ScreenContext
 *   x4 = BlockPos*
 *
 * x5/x6 are preserved as opaque integer arguments.
 */
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


/*
 * Monotonic-ish animation clock.
 */
static double monotonicSeconds() {
    timespec ts{};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;

    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) * 1.0e-9;
}


/*
 * HSV -> RGB.
 *
 * This gives the continuous UnViableTweaks-style RGB cycle:
 *
 * red -> orange -> yellow -> green -> cyan ->
 * blue -> purple -> red
 */
static Color hsvToRgb(
    float h,
    float s,
    float v,
    float a
) {
    h = h - std::floor(h);

    const float scaled = h * 6.0f;

    const int sector =
        static_cast<int>(std::floor(scaled));

    const float fraction =
        scaled - static_cast<float>(sector);

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


/*
 * Emit one vertex.
 */
static inline void vertex(
    void* tessellator,
    const Vec3& p
) {
    g_tessVertex(
        tessellator,
        p.x,
        p.y,
        p.z
    );
}


/*
 * Draw one face as four line segments.
 *
 * ThickBaddie uses primitive mode 1 with 8 vertices
 * for its outline rendering path.
 */
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

        vertex(
            tessellator,
            a.x - camX,
            a.y - camY,
            a.z - camZ
        );

        vertex(
            tessellator,
            b.x - camX,
            b.y - camY,
            b.z - camZ
        );
    }

    /*
     * BedrockTools' existing rendering modules use this
     * 0x58-byte render-mesh argument block.
     */
    char pad[0x58]{};

    g_renderMesh(
        screenContext,
        tessellator,
        material,
        pad
    );
}


/*
 * Draw a complete block outline.
 *
 * We use multiple parallel copies of the edge geometry
 * to emulate thickness. This avoids GLES glLineWidth,
 * which is unreliable on Android/Mali.
 */
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

    /*
     * The native block outline is based on a one-block
     * bounding box.
     */
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
     * Spacing between parallel outline lines.
     *
     * This is deliberately small because coordinates are
     * Minecraft world units, not screen pixels.
     */
    const float spacing =
        0.006f *
        static_cast<float>(
            thickness
        );

    /*
     * We render several slightly offset copies.
     *
     * This gives an actual visual thickness even on GPUs
     * that clamp GL line width to 1.
     */
    for (int layer = 0;
         layer < thickness;
         ++layer) {

        const float center =
            static_cast<float>(
                layer
            ) -
            static_cast<float>(
                thickness - 1
            ) * 0.5f;

        const float o =
            center * spacing;

        /*
         * XY bottom.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{minX, minY + o, minZ + o},
                Vec3{maxX, minY + o, minZ + o},
                Vec3{maxX, minY + o, maxZ},
                Vec3{minX, minY + o, maxZ}
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * XY top.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{minX, maxY + o, minZ + o},
                Vec3{maxX, maxY + o, minZ + o},
                Vec3{maxX, maxY + o, maxZ},
                Vec3{minX, maxY + o, maxZ}
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * XZ front.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{minX, minY + o, minZ},
                Vec3{maxX, minY + o, minZ},
                Vec3{maxX, maxY + o, minZ},
                Vec3{minX, maxY + o, minZ}
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * XZ back.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{minX, minY + o, maxZ},
                Vec3{maxX, minY + o, maxZ},
                Vec3{maxX, maxY + o, maxZ},
                Vec3{minX, maxY + o, maxZ}
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * YZ left.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{minX + o, minY, minZ},
                Vec3{minX + o, minY, maxZ},
                Vec3{minX + o, maxY, maxZ},
                Vec3{minX + o, maxY, minZ}
            },
            color,
            camX,
            camY,
            camZ
        );

        /*
         * YZ right.
         */
        drawFace(
            screenContext,
            tessellator,
            material,
            {
                Vec3{maxX + o, minY, minZ},
                Vec3{maxX + o, minY, maxZ},
                Vec3{maxX + o, maxY, maxZ},
                Vec3{maxX + o, maxY, minZ}
            },
            color,
            camX,
            camY,
            camZ
        );
    }
}


/*
 * Main replacement for ThickBaddie's first outline target.
 *
 * Verified ABI:
 *
 * x0 = LevelRenderer-like object
 * x1 = ScreenContext
 * x4 = BlockPos*
 *
 * The original function is intentionally NOT called while
 * the module is enabled. ThickBaddie uses the same strategy:
 * its replacement completely owns the outline rendering path.
 */
static void blockOutlineHook(
    void* levelRenderer,
    void* screenContext,
    void* x2,
    void* x3,
    void* blockPosPtr,
    std::uintptr_t x5,
    std::uintptr_t x6
) {
    (void)x2;
    (void)x3;
    (void)x5;
    (void)x6;

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
     * Safety checks.
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
                LevelRenderer::mLevelRendererPlayer
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
                LevelRendererPlayer::mCamPos
        );

    const float camX = cam[0];
    const float camY = cam[1];
    const float camZ = cam[2];

    /*
     * Selection overlay material is an embedded material
     * object at 0x1030, not a pointer that needs to be
     * dereferenced.
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
     * Bedrock's selection rendering uses the color holder.
     *
     * Save it and restore it after our rendering.
     */
    const auto colorHolderPtr =
        *reinterpret_cast<std::uintptr_t*>(
            screen +
            bedrocktools::sdk::offsets::
                ScreenContext::mColorHolder
        );

    float savedColor[4]{};

    bool restoreColor = false;

    if (colorHolderPtr &&
        colorHolderPtr >= 0x1000) {

        float* colorHolder =
            reinterpret_cast<float*>(
                colorHolderPtr
            );

        savedColor[0] = colorHolder[0];
        savedColor[1] = colorHolder[1];
        savedColor[2] = colorHolder[2];
        savedColor[3] = colorHolder[3];

        /*
         * Keep the material neutral. RGB is supplied
         * directly through TessellatorColor.
         */
        colorHolder[0] = 1.0f;
        colorHolder[1] = 1.0f;
        colorHolder[2] = 1.0f;
        colorHolder[3] = 1.0f;

        restoreColor = true;
    }

    const Color color =
        getOutlineColor();

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

    if (restoreColor) {
        float* colorHolder =
            reinterpret_cast<float*>(
                colorHolderPtr
            );

        colorHolder[0] =
            savedColor[0];

        colorHolder[1] =
            savedColor[1];

        colorHolder[2] =
            savedColor[2];

        colorHolder[3] =
            savedColor[3];
    }
}

} // namespace


OutlineRGBModule*
    OutlineRGBModule::instance = nullptr;


OutlineRGBModule::OutlineRGBModule()
    : Module(
        "Outline RGB",
        "RGB 3D block outline with adjustable visual thickness."
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

    g_tessBegin =
        reinterpret_cast<Tessellator_begin_t>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    TessellatorBegin
            )
        );

    g_tessColor =
        reinterpret_cast<Tessellator_color_t>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    TessellatorColor
            )
        );

    g_tessVertex =
        reinterpret_cast<Tessellator_vertex_t>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    TessellatorVertex
            )
        );

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
     * Hook stays installed.
     *
     * blockOutlineHook transparently calls the native
     * function while disabled.
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
