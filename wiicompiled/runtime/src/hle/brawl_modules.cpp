// Brawl's .rel modules (files/module/*.rel). The translation holds every module pre-linked at its own home address
// above the game's MEM2 arena (projects/brawl/tools/build_modules.py), because the game itself loads many of them
// at the same heap address (every stage, every fighter) and translated code needs one address per instruction.
//
// OSLink/OSLinkFixed therefore link a module at its home instead of in the buffer the game read it into: the home is
// reset to the module's pristine pre-linked image, its bss cleared, and the header in the game's buffer is updated
// as the SDK's Link would, with section, prolog and epilog addresses pointing at the home. A module whose bytes are
// not the disc's (a mod's replacement) is not in the translation; it is linked in place by the game's own Link.
// The game's own OSUnlink takes either kind back: it only touches the header and the other modules' imports.
//
// The resident modules (OverlayCommon: scene manager, character/stage select, melee) are the exception: the game
// loads them once at boot, always at the same addresses, and Gecko codes patch them there, so their home is the
// game's own buffer and the pre-linked image replaces the loaded file in place.
#include "hle_stubs.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "abi_bridge.h"
#include "guest_code_patch.h"
#include "hle/gecko.h"
#include "memory.h"
#include "runtime_log.h"

extern "C" void func_801DD964(CpuContext* ctx);  // Link(module, bss, fixed), translated
extern "C" void func_801DD6A4(CpuContext* ctx);  // Relocate(imported-from or null for main, importer), translated

namespace {

struct BrawlModule {
    uint32_t id;
    uint32_t image;    // home of the pre-linked image
    uint32_t bss;      // home of its bss
    uint32_t bssSize;
    uint32_t fixSize;  // bytes OSLinkFixed keeps: header and sections, without the relocation tables
    uint32_t crc32;    // of the unrelocated file from the end of its header (0x4C) to fixSize: else a mod's module
    const char* name;
    bool resident;     // loaded at boot at `image`: the home is the game's own buffer
};

constexpr BrawlModule kModules[] = {
#include "brawl_module_layout.inc"
};

constexpr uint32_t kModuleListHead = 0x800030C8u;  // __OSModuleInfoList.head
constexpr uint32_t kModuleListTail = 0x800030CCu;

std::mutex g_modulesMutex;
std::vector<uint8_t> g_pristine[sizeof(kModules) / sizeof(kModules[0])];  // each pre-linked image, from boot

uint32_t Crc32(uint32_t address, uint32_t size) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    });
    const auto* p = static_cast<const uint8_t*>(Memory::GetPointer(address, size));
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

const BrawlModule* FindModule(uint32_t id, size_t* index) {
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); ++i) {
        if (kModules[i].id == id) {
            *index = i;
            return &kModules[i];
        }
    }
    return nullptr;
}

// The executable sections of a module's pre-linked image: (offset from the image, size).
std::vector<std::pair<uint32_t, uint32_t>> HomeCodeSections(size_t index) {
    const BrawlModule& m = kModules[index];
    const std::vector<uint8_t>& image = g_pristine[index];
    auto read32 = [&](uint32_t at) {
        return at + 4 <= image.size() ? (uint32_t{image[at]} << 24 | uint32_t{image[at + 1]} << 16 |
                                         uint32_t{image[at + 2]} << 8 | image[at + 3])
                                      : 0u;
    };
    std::vector<std::pair<uint32_t, uint32_t>> out;
    uint32_t table = read32(0x10u);
    if (table >= m.image) table -= m.image;  // pre-linked headers may hold absolute addresses
    for (uint32_t i = 1; i < read32(0x0Cu); ++i) {
        uint32_t offset = read32(table + i * 8u);
        const uint32_t size = read32(table + i * 8u + 4u);
        if ((offset & 1u) == 0 || size == 0) continue;
        offset &= ~1u;
        if (offset >= m.image) offset -= m.image;
        out.emplace_back(offset, size);
    }
    return out;
}

// A module the game linked in place (a mod's, or any while codes are active) is still reached at its home by
// translated callers: sora_scene creates Project+'s menus through the disc's sora_menu_main, so its My Music never
// loaded a stage's tracklist. Calls into the home run the in-place code instead, section by section where the
// layout is the disc's (Project+ patches code in place and appends a section).
void RedirectHome(uint32_t header) {
    size_t index = 0;
    const BrawlModule* m = FindModule(Memory::Read32(header), &index);
    if (m == nullptr || m->resident || g_pristine[index].empty()) return;
    const uint32_t sections = Memory::Read32(header + 0x10u);  // absolute after Link
    const uint32_t count = Memory::Read32(header + 0x0Cu);
    for (const auto& [offset, size] : HomeCodeSections(index)) {
        bool same = false;
        uint32_t target = 0;
        for (uint32_t i = 1; i < count && !same; ++i) {
            const uint32_t value = Memory::Read32(sections + i * 8u);
            same = (value & 1u) != 0 && (value & ~1u) - header == offset && Memory::Read32(sections + i * 8u + 4u) == size;
            target = value & ~1u;
        }
        if (same) {
            GuestCodePatch::RedirectModule(m->image + offset, m->image + offset + size, target, m->name);
        } else {
            GuestCodePatch::ClearRedirect(m->image + offset);
            RT_LOGF(RT_TAG_OS, "REL module %s at 0x%08X: code layout differs from the disc's, calls into its home "
                    "0x%08X run the disc's code\n", m->name, header, m->image + offset);
        }
    }
}

// Link (OSLink.c) for a vanilla module, with the code at its home. Returns false when the module is not one of
// the disc's (unknown id, or modified bytes): the caller links it in place.
bool LinkAtHome(uint32_t header, uint32_t gameBss) {
    std::lock_guard<std::mutex> lock(g_modulesMutex);
    size_t index = 0;
    const BrawlModule* m = FindModule(Memory::Read32(header), &index);
    if (m == nullptr || Memory::Read32(header + 0x1Cu) != 3u) {
        return false;
    }
    // Brawl's loader rewrites the header's relOffset/impOffset to relocation tables it keeps elsewhere, so only the
    // rest of the image identifies the disc's module.
    if (Crc32(header + 0x4Cu, m->fixSize - 0x4Cu) != m->crc32) {
        RT_LOGF(RT_TAG_OS, "REL module %s at 0x%08X is not the disc's (a mod): linking it in place, untranslated\n",
                m->name, header);
        // At its retail address the disc's translated functions stay registered over the mod's code (Project+Ex
        // ships its own sora_menu_sel_char): calls into them must run the mod's bytes.
        if (m->resident && header == m->image) GuestCodePatch::InterpretReplacedModule(m->image, m->image + m->fixSize, m->name);
        return false;
    }

    // Gecko codes patch modules where the game loads them (Project+ turns st_final into its custom stages): with codes
    // active, a module loaded on demand runs in place, where they reach it, not at its translated home.
    // Cheats alone (no mod's code list) keep the translated homes: in place, the modules would run interpreted.
    if (!m->resident && GuestCodePatch::Active() && Gecko::ModCodesInstalled()) {
        return false;
    }
    // A resident module somewhere else (a mod moved the heap) is not the translated one.
    if (m->resident && (header != m->image || gameBss != m->bss)) {
        RT_LOGF(RT_TAG_OS, "REL module %s loaded at 0x%08X (bss 0x%08X), not at 0x%08X (bss 0x%08X): linking it in "
                "place, untranslated\n", m->name, header, gameBss, m->image, m->bss);
        return false;
    }
    // Every link starts from the pristine image: the game expects a module's data as the file has it. A resident
    // module's home is the game's buffer, whose header (rewritten by the game's loader) stays as it is.
    const auto& pristine = g_pristine[index];
    const uint32_t keep = m->resident ? 0x4Cu : 0u;
    if (!m->resident) {
        for (const auto& [offset, size] : HomeCodeSections(index)) GuestCodePatch::ClearRedirect(m->image + offset);
    }
    std::memcpy(Memory::GetPointer(m->image + keep, m->fixSize - keep), pristine.data() + keep, m->fixSize - keep);
    std::memset(Memory::GetPointer(m->bss, m->bssSize ? m->bssSize : 4), 0, m->bssSize);

    // The header in the game's buffer, as Link leaves it: tables absolute, sections and entry points at the home.
    const uint32_t tail = Memory::Read32(kModuleListTail);
    Memory::Write32(header + 0x04u, 0);     // link.next
    Memory::Write32(header + 0x08u, tail);  // link.prev
    if (tail == 0) {
        Memory::Write32(kModuleListHead, header);
    } else {
        Memory::Write32(tail + 0x04u, header);
    }
    Memory::Write32(kModuleListTail, header);

    const uint32_t numSections = Memory::Read32(header + 0x0Cu);
    const uint32_t sections = header + Memory::Read32(header + 0x10u);
    Memory::Write32(header + 0x10u, sections);
    Memory::Write32(header + 0x24u, header + Memory::Read32(header + 0x24u));  // relOffset
    Memory::Write32(header + 0x28u, header + Memory::Read32(header + 0x28u));  // impOffset
    // Its imports were all resolved when it was pre-linked, and the game may free the tables after a fixed link:
    // modules linked in place (a mod's) must not relocate or unrelocate anything here through them.
    Memory::Write32(header + 0x2Cu, 0);  // impSize
    Memory::Write32(header + 0x48u, header + Memory::Read32(header + 0x48u));  // fixSize
    uint8_t bssSection = 0;
    for (uint32_t i = 1; i < numSections; ++i) {
        const uint32_t entry = sections + i * 8u;
        const uint32_t offset = Memory::Read32(entry);
        if (offset != 0) {
            Memory::Write32(entry, m->image + offset);  // keeps the exec bit (bit 0)
        } else if (Memory::Read32(entry + 4u) != 0) {
            bssSection = static_cast<uint8_t>(i);
            Memory::Write32(entry, m->bss);
        }
    }
    Memory::Write8(header + 0x33u, bssSection);
    if (GuestCodePatch::Active()) {
        // Mods patch module code at run time: guard each code section, pristine again after this link.
        for (uint32_t i = 1; i < numSections; ++i) {
            const uint32_t value = Memory::Read32(sections + i * 8u);
            const uint32_t size = Memory::Read32(sections + i * 8u + 4u);
            if ((value & 1u) != 0 && size != 0) {
                GuestCodePatch::Protect(value & ~1u, (value & ~1u) + size, m->name);
                GuestCodePatch::ResetPristine(value & ~1u, (value & ~1u) + size);
            }
        }
        // The pristine image just replaced the code handler's hooks, and the module's prolog may run them right away
        // (Project+ loads its stock icons from one in sora_scene).
        GuestCodePatch::Reapply(m->image + keep, m->image + m->fixSize);
    }
    for (uint32_t field = 0; field < 3; ++field) {  // prolog, epilog, unresolved
        const uint8_t section = Memory::Read8(header + 0x30u + field);
        if (section != 0) {
            const uint32_t at = header + 0x34u + field * 4u;
            Memory::Write32(at, Memory::Read32(at) + (Memory::Read32(sections + section * 8u) & ~1u));
        }
    }
    RT_LOGF(RT_TAG_OS, "REL module %s (%u) linked at its home 0x%08X (game buffer 0x%08X, bss 0x%08X)\n", m->name,
            m->id, m->image, header, gameBss);
    return true;
}

// Bugs in mods that a Wii hides by luck of what the heap held, fixed in the linked module (header and sections
// absolute). Syriinge 0.6.0's core (sy_core, Project+ 3.x): the InjectionAbs constructor leaves Hook::trampoline
// unset, and onModuleLoaded follows it when it is not null, so a replace-function hook without an "original" crashes
// on the 0xCC the game fills its arena with. The constructor now nulls it instead of setting type and target, which
// both of its callers set afterwards.
void FixKnownModBugs(uint32_t header) {
    static constexpr uint32_t kSyCoreCtor[6] = {0x3800FFFF, 0x90030000, 0x98030004, 0x90030008, 0x9003000C, 0x4E800020};
    static constexpr uint32_t kSyCoreCtorFixed[6] = {0x3800FFFF, 0x38800000, 0x98030004, 0x90830014, 0x9003000C,
                                                     0x4E800020};
    if (Memory::Read32(header) != 224) return;  // sy_core's module id
    const uint32_t sections = Memory::Read32(header + 0x10u);
    for (uint32_t i = 1; i < Memory::Read32(header + 0x0Cu); ++i) {
        const uint32_t start = Memory::Read32(sections + i * 8u), size = Memory::Read32(sections + i * 8u + 4u);
        if ((start & 1u) == 0) continue;
        for (uint32_t at = start & ~1u; at + sizeof(kSyCoreCtor) <= (start & ~1u) + size; at += 4) {
            uint32_t k = 0;
            while (k < 6 && Memory::Read32(at + 4 * k) == kSyCoreCtor[k]) ++k;
            if (k < 6) continue;
            for (k = 0; k < 6; ++k) Memory::Write32(at + 4 * k, kSyCoreCtorFixed[k]);
            RT_LOGF(RT_TAG_MOD, "[mods] Syriinge 0.6.0: hook trampolines start null (constructor at 0x%08X)\n", at);
            return;
        }
    }
}

void LinkModule(CpuContext* ctx, bool fixed) {
    const uint32_t header = ctx->gpr[3];
    if (LinkAtHome(header, ctx->gpr[4])) {
        // Modules linked in place before it (a mod's, Syriinge plugins) may import from it: relocate those imports,
        // as Link does for every module already loaded. Home-linked ones have nothing to relocate.
        for (uint32_t m = Memory::Read32(kModuleListHead); m != 0; m = Memory::Read32(m + 4u)) {
            if (m == header || Memory::Read32(m + 0x2Cu) == 0) continue;
            ctx->gpr[3] = header;
            ctx->gpr[4] = m;
            func_801DD6A4(ctx);  // Relocate(imported-from, importer)
        }
        ctx->gpr[3] = 1;
        return;
    }
    ctx->gpr[5] = fixed ? 1u : 0u;
    func_801DD964(ctx);
    RT_LOGF(RT_TAG_OS, "REL module %u linked in place at 0x%08X (%s)\n", Memory::Read32(header), header,
            ctx->gpr[3] ? "ok" : "FAILED");
    if (ctx->gpr[3]) {
        FixKnownModBugs(header);
        RedirectHome(header);
    }
}

}  // namespace

// Once the embedded pre-linked images are in guest memory and before the game runs (a resident module's home is about
// to receive the game's own load of the file): every module's pristine image.
void BrawlModulesSnapshotImages() {
    std::lock_guard<std::mutex> lock(g_modulesMutex);
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); ++i) {
        const auto* image = static_cast<const uint8_t*>(Memory::GetPointer(kModules[i].image, kModules[i].fixSize));
        g_pristine[i].assign(image, image + kModules[i].fixSize);
    }
}

// After the code handler's boot pass (Gecko::Install): what it wrote into the resident modules' pre-linked images,
// which no write trap covers yet. Each link at home starts from the pristine image and puts these back.
void BrawlModulesRecordBootPatches() {
    std::lock_guard<std::mutex> lock(g_modulesMutex);
    for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); ++i) {
        const BrawlModule& m = kModules[i];
        if (!m.resident) continue;
        const auto* now = static_cast<const uint8_t*>(Memory::GetPointer(m.image, m.fixSize));
        for (uint32_t offset = 0x4Cu; offset + 4 <= m.fixSize; offset += 4) {
            if (std::memcmp(now + offset, g_pristine[i].data() + offset, 4) == 0) continue;
            uint32_t value = 0;
            std::memcpy(&value, now + offset, 4);
            GuestCodePatch::RecordPatch(m.image + offset, value);
        }
    }
}

// 0x801DDC44 OSLink(module, bss) = Link(module, bss, FALSE); 0x801DDC4C OSLinkFixed: version 3 only, then
// Link(module, bss, TRUE).
extern "C" void Brawl_OSLink_HLE(CpuContext* ctx) { LinkModule(ctx, false); }
PPC_NATIVE_OVERRIDE_VOID(801DDC44, Brawl_OSLink_HLE, (CpuContext* ctx), (ctx));

extern "C" void Brawl_OSLinkFixed_HLE(CpuContext* ctx)
{
    if (Memory::Read32(ctx->gpr[3] + 0x1Cu) != 3u) {
        ctx->gpr[3] = 0;
        return;
    }
    LinkModule(ctx, true);
}
PPC_NATIVE_OVERRIDE_VOID(801DDC4C, Brawl_OSLinkFixed_HLE, (CpuContext* ctx), (ctx));
