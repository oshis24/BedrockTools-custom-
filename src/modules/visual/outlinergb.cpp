#include "outlinergb.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>

#include "core/memory/Hooks.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <chrono>

namespace {

using TessellatorBeginFn =
    void (*)(void*, void*, int, int, int);

using TessellatorColorFn =
    void (*)(void*, float, float, float, float);

using TessellatorVertexFn =
    void (*)(void*, float, float, float);

using RenderMeshFn =
    void (*)(void*, void*, void*, char*);

using GetHitResultFn =
    void* (*)(void*);


OutlineRGBModule* g_module = nullptr;

void* g_renderLevelTarget = nullptr;

TessellatorBeginFn g_tessBegin = nullptr;
TessellatorColorFn g_tessColor = nullptr;
TessellatorVertexFn g_tessVertex = nullptr;
RenderMeshFn g_renderMesh = nullptr;
GetHitResultFn g_getHitResult = nullptr;

void (*g_renderLevelOriginal)(
    void*,
    void*,
    void*
) = nullptr;

void* g_renderMaterialGroup = nullptr;
void* g_selectionMaterial = nullptr;

bool g_hooked = false;

float g_hue = 0.0f;
std::chrono::steady_clock::time_point g_lastTime =
    std::chrono::steady_clock::now();


struct Line {
    bedrocktools::sdk::Vec3 a;
    bedrocktools::sdk::Vec3 b;
};


struct AABB {
    bedrocktools::sdk::Vec3 min;
    bedrocktools::sdk::Vec3 max;
};


static float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}


static void hsvToRgb(
    float h,
    float s,
    float v,
    float& r,
    float& g,
    float& b
) {
    h = std::fmod(h, 1.0f);

    if (h < 0.0f)
        h += 1.0f;

    const float x = h * 6.0f;
    const int i = static_cast<int>(std::floor(x));
    const float f = x - static_cast<float>(i);

    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    switch (i % 6) {
        case 0:
            r = v; g = t; b = p;
            break;

        case 1:
            r = q; g = v; b = p;
            break;

        case 2:
            r = p; g = v; b = t;
            break;

        case 3:
            r = p; g = q; b = v;
            break;

        case 4:
            r = t; g = p; b = v;
            break;

        default:
            r = v; g = p; b = q;
            break;
    }
}


static void updateChroma() {
    const auto now =
        std::chrono::steady_clock::now();

    const float dt =
        std::chrono::duration<float>(
            now - g_lastTime
        ).count();

    g_lastTime = now;

    if (!g_module || !g_module->rgbCycle)
        return;

    g_hue +=
        dt *
        0.25f *
        std::max(
            0.05f,
            g_module->chromaSpeed
        );

    if (g_hue >= 1.0f)
        g_hue -= std::floor(g_hue);
}


static void getColor(
    float& r,
    float& g,
    float& b,
    float& a
) {
    if (!g_module) {
        r = g = b = 1.0f;
        a = 1.0f;
        return;
    }

    a = clamp01(g_module->alpha);

    if (g_module->rgbCycle) {
        hsvToRgb(
            g_hue,
            1.0f,
            1.0f,
            r,
            g,
            b
        );
        return;
    }

    r = clamp01(g_module->red);
    g = clamp01(g_module->green);
    b = clamp01(g_module->blue);
}


static void addBoxEdges(
    const AABB& box,
    std::vector<Line>& lines
) {
    const auto& mn = box.min;
    const auto& mx = box.max;

    const bedrocktools::sdk::Vec3 p000{
        mn.x, mn.y, mn.z
    };

    const bedrocktools::sdk::Vec3 p100{
        mx.x, mn.y, mn.z
    };

    const bedrocktools::sdk::Vec3 p110{
        mx.x, mx.y, mn.z
    };

    const bedrocktools::sdk::Vec3 p010{
        mn.x, mx.y, mn.z
    };

    const bedrocktools::sdk::Vec3 p001{
        mn.x, mn.y, mx.z
    };

    const bedrocktools::sdk::Vec3 p101{
        mx.x, mn.y, mx.z
    };

    const bedrocktools::sdk::Vec3 p111{
        mx.x, mx.y, mx.z
    };

    const bedrocktools::sdk::Vec3 p011{
        mn.x, mx.y, mx.z
    };


    lines.push_back({p000, p100});
    lines.push_back({p100, p110});
    lines.push_back({p110, p010});
    lines.push_back({p010, p000});

    lines.push_back({p001, p101});
    lines.push_back({p101, p111});
    lines.push_back({p111, p011});
    lines.push_back({p011, p001});

    lines.push_back({p000, p001});
    lines.push_back({p100, p101});
    lines.push_back({p110, p111});
    lines.push_back({p010, p011});
}


static AABB makeBlockBox(
    const bedrocktools::sdk::Vec3& pos
) {
    return {
        {
            std::floor(pos.x),
            std::floor(pos.y),
            std::floor(pos.z)
        },
        {
            std::floor(pos.x) + 1.0f,
            std::floor(pos.y) + 1.0f,
            std::floor(pos.z) + 1.0f
        }
    };
}


static bool getBlockHit(
    void* level,
    bedrocktools::sdk::Vec3& blockPos
) {
    if (!g_getHitResult || !level)
        return false;

    void* hit =
        g_getHitResult(level);

    if (!hit)
        return false;

    const int type =
        *reinterpret_cast<int*>(
            reinterpret_cast<std::uintptr_t>(hit) +
            bedrocktools::sdk::offsets::HitResult::mType
        );

    /*
     * ReachCounter in this project already treats hit
     * types 0/1 as valid world/entity hit results.
     */
    if (type != 0 && type != 1)
        return false;

    const auto* pos =
        reinterpret_cast<
            const bedrocktools::sdk::Vec3*
        >(
            reinterpret_cast<
                std::uintptr_t
            >(hit) +
            bedrocktools::sdk::offsets::HitResult::mPos
        );

    if (!pos)
        return false;

    blockPos = *pos;

    return true;
}


static void ensureMaterial() {
    if (g_selectionMaterial)
        return;

    /*
     * The existing BedrockTools visual modules use
     * selection_box from RenderMaterialGroup.
     */
    if (!g_renderMaterialGroup)
        return;

    struct HashedString {
        std::uint64_t hash;
        std::string string;
        const void* lastMatch;
    };

    HashedString hs{
        0xcbf29ce484222325ULL,
        "selection_box",
        nullptr
    };

    for (char c : hs.string) {
        hs.hash =
            static_cast<std::uint64_t>(
                static_cast<unsigned char>(c)
            ) ^
            (
                hs.hash *
                0x100000001b3ULL
            );
    }

    void** vtable =
        *reinterpret_cast<void***>(
            g_renderMaterialGroup
        );

    if (!vtable || !vtable[2])
        return;

    using GetMaterialFn =
        void* (*)(void*, const HashedString*);

    auto getMaterial =
        reinterpret_cast<GetMaterialFn>(
            vtable[2]
        );

    g_selectionMaterial =
        getMaterial(
            g_renderMaterialGroup,
            &hs
        );
}


static void drawLines(
    void* screenContext,
    void* tessellator,
    std::vector<Line>& lines,
    float camX,
    float camY,
    float camZ
) {
    if (
        lines.empty() ||
        !g_tessBegin ||
        !g_tessColor ||
        !g_tessVertex ||
        !g_renderMesh ||
        !g_selectionMaterial
    ) {
        return;
    }


    float r, g, b, a;
    getColor(r, g, b, a);


    /*
     * Primitive 4 is the line list already used by the
     * existing Hitbox/Breadcrumb modules.
     */
    g_tessBegin(
        tessellator,
        nullptr,
        4,
        static_cast<int>(
            lines.size() * 2
        ),
        0
    );

    g_tessColor(
        tessellator,
        r,
        g,
        b,
        a
    );


    for (const auto& line : lines) {

        float ax =
            line.a.x - camX;

        float ay =
            line.a.y - camY;

        float az =
            line.a.z - camZ;

        float bx =
            line.b.x - camX;

        float by =
            line.b.y - camY;

        float bz =
            line.b.z - camZ;


        g_tessVertex(
            tessellator,
            ax,
            ay,
            az
        );

        g_tessVertex(
            tessellator,
            bx,
            by,
            bz
        );
    }


    char padding[0x58];
    std::memset(
        padding,
        0,
        sizeof(padding)
    );


    g_renderMesh(
        screenContext,
        tessellator,
        g_selectionMaterial,
        padding
    );
}


/*
 * We create a slightly expanded set of wire boxes to provide
 * a practical thickness control without touching GL line width.
 *
 * This is deliberately conservative: every extra layer is
 * another box, so the module clamps the number of layers.
 */
static void drawThickBox(
    void* screenContext,
    void* tessellator,
    AABB box,
    float camX,
    float camY,
    float camZ
) {
    if (!g_module)
        return;


    const float thickness =
        std::clamp(
            g_module->thickness,
            0.5f,
            4.0f
        );


    const int layers =
        std::clamp(
            static_cast<int>(
                std::ceil(thickness)
            ),
            1,
            4
        );


    std::vector<Line> lines;

    lines.reserve(
        static_cast<std::size_t>(
            layers * 12
        )
    );


    const float step =
        0.0025f;


    for (int i = 0; i < layers; ++i) {

        const float expand =
            step *
            static_cast<float>(i);


        AABB layer{
            {
                box.min.x - expand,
                box.min.y - expand,
                box.min.z - expand
            },
            {
                box.max.x + expand,
                box.max.y + expand,
                box.max.z + expand
            }
        };


        addBoxEdges(
            layer,
            lines
        );
    }


    drawLines(
        screenContext,
        tessellator,
        lines,
        camX,
        camY,
        camZ
    );
}


static void renderLevelHook(
    void* self,
    void* screenContext,
    void* a3
) {
    if (g_renderLevelOriginal) {
        g_renderLevelOriginal(
            self,
            screenContext,
            a3
        );
    }


    if (
        !g_module ||
        !g_module->enabled
    ) {
        return;
    }


    if (
        !screenContext ||
        !self
    ) {
        return;
    }


    if (
        !g_tessBegin ||
        !g_tessColor ||
        !g_tessVertex ||
        !g_renderMesh
    ) {
        return;
    }


    /*
     * ScreenContext::mTessellator
     */
    const auto screen =
        reinterpret_cast<
            std::uintptr_t
        >(screenContext);


    void* tessellator =
        *reinterpret_cast<void**>(
            screen +
            bedrocktools::sdk::offsets::
                ScreenContext::mTessellator
        );


    if (!tessellator)
        return;


    /*
     * LevelRendererPlayer::mCamPos
     */
    const auto renderer =
        reinterpret_cast<
            std::uintptr_t
        >(self);


    void* levelRendererPlayer =
        *reinterpret_cast<void**>(
            renderer +
            bedrocktools::sdk::offsets::
                LevelRenderer::mLevelRendererPlayer
        );


    if (!levelRendererPlayer)
        return;


    const auto lrp =
        reinterpret_cast<
            std::uintptr_t
        >(levelRendererPlayer);


    const float camX =
        *reinterpret_cast<float*>(
            lrp +
            bedrocktools::sdk::offsets::
                LevelRendererPlayer::mCamPos
        );

    const float camY =
        *reinterpret_cast<float*>(
            lrp +
            bedrocktools::sdk::offsets::
                LevelRendererPlayer::mCamPos +
            4
        );

    const float camZ =
        *reinterpret_cast<float*>(
            lrp +
            bedrocktools::sdk::offsets::
                LevelRendererPlayer::mCamPos +
            8
        );


    ensureMaterial();

    if (!g_selectionMaterial)
        return;


    updateChroma();


    /*
     * Same pattern already used by EntityCullingModule:
     * ClientInstance::current() -> localPlayer() -> level().
     * This avoids needing our own actor-tick cache just for
     * the outline module.
     */
    auto* client =
        bedrocktools::sdk::ClientInstance::current();

    if (!client)
        return;

    auto* localPlayer =
        client->localPlayer();

    if (!localPlayer)
        return;

    void* level =
        localPlayer->level();

    if (!level)
        return;


    if (!g_module->blockOutline) {
        (void)a3;
        return;
    }


    bedrocktools::sdk::Vec3 blockPos{};

    if (!getBlockHit(level, blockPos)) {
        (void)a3;
        return;
    }


    const AABB box =
        makeBlockBox(blockPos);


    if (g_module->outline3D) {
        drawThickBox(
            screenContext,
            tessellator,
            box,
            camX,
            camY,
            camZ
        );
    } else {
        std::vector<Line> lines;
        lines.reserve(12);
        addBoxEdges(box, lines);

        drawLines(
            screenContext,
            tessellator,
            lines,
            camX,
            camY,
            camZ
        );
    }


    (void)a3;
}


} // namespace


OutlineRGBModule::OutlineRGBModule()
    : Module(
        "Outline RGB",
        "RGB block/entity outline with optional 3D AABB rendering."
    ) {
    g_module = this;
}


OutlineRGBModule::~OutlineRGBModule() {
    if (g_module == this)
        g_module = nullptr;
}


void OutlineRGBModule::onInit() {

    const auto renderLevel =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                RenderLevel
        );


    if (renderLevel) {
        g_renderLevelTarget =
            reinterpret_cast<void*>(
                renderLevel
            );
    }


    const auto begin =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                TessellatorBegin
        );

    const auto color =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                TessellatorColor
        );

    const auto vertex =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                TessellatorVertex
        );

    auto mesh =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                MeshHelpersRenderMeshImmediately2
        );

    if (!mesh) {
        mesh =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    MeshHelpersRenderMeshImmediately
            );
    }


    const auto hitResult =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::
                LevelGetHitResult
        );


    g_tessBegin =
        reinterpret_cast<
            TessellatorBeginFn
        >(begin);

    g_tessColor =
        reinterpret_cast<
            TessellatorColorFn
        >(color);

    g_tessVertex =
        reinterpret_cast<
            TessellatorVertexFn
        >(vertex);

    g_renderMesh =
        reinterpret_cast<
            RenderMeshFn
        >(mesh);

    g_getHitResult =
        reinterpret_cast<
            GetHitResultFn
        >(hitResult);


    if (
        !g_renderLevelTarget ||
        !g_tessBegin ||
        !g_tessColor ||
        !g_tessVertex ||
        !g_renderMesh
    ) {
        return;
    }


    /*
     * RenderLevelPlayer's selection material path is already
     * used by Hitbox/Breadcrumbs.
     *
     * We resolve its material group lazily when rendering.
     */


    if (!g_hooked) {

        auto hook =
            bedrocktools::hooks::install(
                g_renderLevelTarget,
                reinterpret_cast<void*>(
                    renderLevelHook
                ),
                reinterpret_cast<void**>(
                    &g_renderLevelOriginal
                )
            );


        if (hook) {
            g_hooked = true;
        }
    }
}


void OutlineRGBModule::onEnable() {
}


void OutlineRGBModule::onDisable() {
}


void OutlineRGBModule::loadConfig(
    const nlohmann::json& j
) {
    Module::loadConfig(j);

    blockOutline =
        j.value(
            "blockOutline",
            blockOutline
        );

    entityOutline =
        j.value(
            "entityOutline",
            entityOutline
        );

    outline3D =
        j.value(
            "outline3D",
            outline3D
        );

    rgbCycle =
        j.value(
            "rgbCycle",
            rgbCycle
        );

    red =
        std::clamp(
            j.value("red", red),
            0.0f,
            1.0f
        );

    green =
        std::clamp(
            j.value("green", green),
            0.0f,
            1.0f
        );

    blue =
        std::clamp(
            j.value("blue", blue),
            0.0f,
            1.0f
        );

    alpha =
        std::clamp(
            j.value("alpha", alpha),
            0.0f,
            1.0f
        );

    chromaSpeed =
        std::clamp(
            j.value("chromaSpeed", chromaSpeed),
            0.05f,
            1.0f
        );

    thickness =
        std::clamp(
            j.value("thickness", thickness),
            0.5f,
            4.0f
        );
}


void OutlineRGBModule::saveConfig(
    nlohmann::json& j
) {
    Module::saveConfig(j);

    j["blockOutline"] = blockOutline;
    j["entityOutline"] = entityOutline;
    j["outline3D"] = outline3D;
    j["rgbCycle"] = rgbCycle;

    j["red"] = red;
    j["green"] = green;
    j["blue"] = blue;
    j["alpha"] = alpha;

    j["chromaSpeed"] = chromaSpeed;
    j["thickness"] = thickness;
}
