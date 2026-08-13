#include "outlinergb.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include "core/memory/Hooks.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
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


OutlineRGBModule* g_module = nullptr;

void* g_renderLevelTarget = nullptr;

TessellatorBeginFn g_tessBegin = nullptr;
TessellatorColorFn g_tessColor = nullptr;
TessellatorVertexFn g_tessVertex = nullptr;
RenderMeshFn g_renderMesh = nullptr;

void (*g_renderLevelOriginal)(
    void*,
    void*,
    void*
) = nullptr;

/*
 * These two are resolved in onInit() below. Previously
 * g_renderMaterialGroup was declared but never assigned
 * anywhere in this file, so ensureMaterial() always bailed
 * out early and the module never drew anything - this is
 * the main reason nothing rendered at all.
 */
uintptr_t g_renderMaterialGroup = 0;
void* g_selectionMaterial = nullptr;

bool g_hooked = false;

void* g_localPlayerPtr = nullptr;

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


/*
 * Same ADRP/ADD (and literal LDR) resolver already used by
 * HitboxModule to turn a resolved function signature into the
 * actual RenderMaterialGroup pointer it loads.
 */
static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);

            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}


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
    a = 1.0f;

    if (!g_module) {
        r = g = b = 1.0f;
        return;
    }

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

    r = clamp01(g_module->colorRed);
    g = clamp01(g_module->colorGreen);
    b = clamp01(g_module->colorBlue);
}


static void addBoxEdges(
    const AABB& box,
    std::vector<Line>& lines
) {
    const auto& mn = box.min;
    const auto& mx = box.max;

    const bedrocktools::sdk::Vec3 p000{ mn.x, mn.y, mn.z };
    const bedrocktools::sdk::Vec3 p100{ mx.x, mn.y, mn.z };
    const bedrocktools::sdk::Vec3 p110{ mx.x, mx.y, mn.z };
    const bedrocktools::sdk::Vec3 p010{ mn.x, mx.y, mn.z };
    const bedrocktools::sdk::Vec3 p001{ mn.x, mn.y, mx.z };
    const bedrocktools::sdk::Vec3 p101{ mx.x, mn.y, mx.z };
    const bedrocktools::sdk::Vec3 p111{ mx.x, mx.y, mx.z };
    const bedrocktools::sdk::Vec3 p011{ mn.x, mx.y, mx.z };

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


/*
 * Local player pointer captured every tick, exactly the same
 * mechanism ReachCounter/SkinStealer/Hitbox already use
 * successfully in this project - LocalPlayerTickEvent, not
 * ClientInstance::current().
 */
static void onLocalPlayerTick(void* player) {
    if (!g_module || !g_module->enabled)
        return;

    g_localPlayerPtr = player;
}


/*
 * Direct offset chain instead of calling a separately-resolved
 * "LevelGetHitResult" function pointer - matches Hitbox's proven
 * working path: Actor::mLevel -> Level::mHitResultWrapper ->
 * HitResultWrapper::mHitResult.
 */
static bool getBlockHit(bedrocktools::sdk::Vec3& blockPos) {
    if (!g_localPlayerPtr)
        return false;

    const uintptr_t levelPtr =
        *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(g_localPlayerPtr) +
            bedrocktools::sdk::offsets::Actor::mLevel
        );

    if (!levelPtr)
        return false;

    const uintptr_t hitResult =
        levelPtr +
        bedrocktools::sdk::offsets::Level::mHitResultWrapper +
        bedrocktools::sdk::offsets::HitResultWrapper::mHitResult;

    const int type =
        *reinterpret_cast<int*>(
            hitResult +
            bedrocktools::sdk::offsets::HitResult::mType
        );

    if (type != 0 && type != 1)
        return false;

    const auto* pos =
        reinterpret_cast<const bedrocktools::sdk::Vec3*>(
            hitResult +
            bedrocktools::sdk::offsets::HitResult::mPos
        );

    blockPos = *pos;
    return true;
}


static void ensureMaterial() {
    if (g_selectionMaterial)
        return;

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
            reinterpret_cast<void*>(g_renderMaterialGroup),
            &hs
        );
}


static void drawLines(
    void* screenContext,
    void* tessellator,
    void* material,
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
        !material
    ) {
        return;
    }


    float r, g, b, a;
    getColor(r, g, b, a);


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

        float ax = line.a.x - camX;
        float ay = line.a.y - camY;
        float az = line.a.z - camZ;

        float bx = line.b.x - camX;
        float by = line.b.y - camY;
        float bz = line.b.z - camZ;

        g_tessVertex(tessellator, ax, ay, az);
        g_tessVertex(tessellator, bx, by, bz);
    }


    char padding[0x58];
    std::memset(padding, 0, sizeof(padding));

    g_renderMesh(
        screenContext,
        tessellator,
        material,
        padding
    );
}


/*
 * Fixed, no longer user-configurable (thickness slider was
 * removed from the menu per request) - 2 layers gives a subtle
 * but clearly visible thickness without a GL line-width
 * dependency, same geometric-expansion idea ThickBaddieOutline
 * uses.
 */
static void drawThickBox(
    void* screenContext,
    void* tessellator,
    void* material,
    const AABB& box,
    float camX,
    float camY,
    float camZ
) {
    constexpr int kLayers = 2;
    constexpr float kStep = 0.0025f;

    std::vector<Line> lines;
    lines.reserve(kLayers * 12);

    for (int i = 0; i < kLayers; ++i) {
        const float expand = kStep * static_cast<float>(i);

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

        addBoxEdges(layer, lines);
    }

    drawLines(
        screenContext,
        tessellator,
        material,
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
        g_renderLevelOriginal(self, screenContext, a3);
    }

    if (!g_module || !g_module->enabled)
        return;

    if (!screenContext || !self)
        return;

    if (
        !g_tessBegin ||
        !g_tessColor ||
        !g_tessVertex ||
        !g_renderMesh
    ) {
        return;
    }


    const auto screen =
        reinterpret_cast<std::uintptr_t>(screenContext);

    void* tessellator =
        *reinterpret_cast<void**>(
            screen +
            bedrocktools::sdk::offsets::ScreenContext::mTessellator
        );

    if (!tessellator)
        return;


    const auto renderer =
        reinterpret_cast<std::uintptr_t>(self);

    void* levelRendererPlayer =
        *reinterpret_cast<void**>(
            renderer +
            bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer
        );

    if (!levelRendererPlayer)
        return;

    const auto lrp =
        reinterpret_cast<std::uintptr_t>(levelRendererPlayer);

    const float camX =
        *reinterpret_cast<float*>(
            lrp + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos
        );

    const float camY =
        *reinterpret_cast<float*>(
            lrp + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4
        );

    const float camZ =
        *reinterpret_cast<float*>(
            lrp + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8
        );


    ensureMaterial();

    /*
     * Same fallback Hitbox uses: if the material-group vtable
     * lookup didn't resolve, fall back to the material pointer
     * already cached on LevelRendererPlayer.
     */
    void* material =
        g_selectionMaterial
            ? g_selectionMaterial
            : reinterpret_cast<void*>(
                  lrp +
                  bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial
              );

    if (!material)
        return;


    updateChroma();


    bedrocktools::sdk::Vec3 blockPos{};

    if (!getBlockHit(blockPos))
        return;

    const AABB box = makeBlockBox(blockPos);


    if (g_module->outline3D) {
        drawThickBox(
            screenContext,
            tessellator,
            material,
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
            material,
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
        "RGB block outline with optional 3D AABB rendering."
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
            bedrocktools::memory::SignatureId::RenderLevel
        );

    if (renderLevel) {
        g_renderLevelTarget = reinterpret_cast<void*>(renderLevel);
    }


    const auto begin =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorBegin
        );

    const auto color =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorColor
        );

    const auto vertex =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorVertex
        );

    auto mesh =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2
        );

    if (!mesh) {
        mesh =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately
            );
    }

    g_tessBegin = reinterpret_cast<TessellatorBeginFn>(begin);
    g_tessColor = reinterpret_cast<TessellatorColorFn>(color);
    g_tessVertex = reinterpret_cast<TessellatorVertexFn>(vertex);
    g_renderMesh = reinterpret_cast<RenderMeshFn>(mesh);


    /*
     * This is the piece that was completely missing before:
     * g_renderMaterialGroup was never assigned, so the module
     * silently drew nothing every single frame.
     */
    const auto rmg =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderMaterialGroupCommon
        );

    if (rmg) {
        const auto groupAddr =
            resolveADRP(reinterpret_cast<uint32_t*>(rmg), 8, 0);

        if (groupAddr) {
            g_renderMaterialGroup =
                groupAddr +
                bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }


    if (
        !g_renderLevelTarget ||
        !g_tessBegin ||
        !g_tessColor ||
        !g_tessVertex ||
        !g_renderMesh
    ) {
        return;
    }


    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { onLocalPlayerTick(event.player); }
    );


    if (!g_hooked) {
        auto hook =
            bedrocktools::hooks::install(
                g_renderLevelTarget,
                reinterpret_cast<void*>(renderLevelHook),
                reinterpret_cast<void**>(&g_renderLevelOriginal)
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

    rgbCycle = j.value("rgbCycle", rgbCycle);
    outline3D = j.value("outline3D", outline3D);

    colorRed =
        std::clamp(j.value("colorRed", colorRed), 0.0f, 1.0f);

    colorGreen =
        std::clamp(j.value("colorGreen", colorGreen), 0.0f, 1.0f);

    colorBlue =
        std::clamp(j.value("colorBlue", colorBlue), 0.0f, 1.0f);

    chromaSpeed =
        std::clamp(j.value("chromaSpeed", chromaSpeed), 0.05f, 1.0f);
}


void OutlineRGBModule::saveConfig(
    nlohmann::json& j
) {
    Module::saveConfig(j);

    j["rgbCycle"] = rgbCycle;
    j["outline3D"] = outline3D;

    j["colorRed"] = colorRed;
    j["colorGreen"] = colorGreen;
    j["colorBlue"] = colorBlue;

    j["chromaSpeed"] = chromaSpeed;
}
