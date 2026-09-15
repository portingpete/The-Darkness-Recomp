#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// Verified original prompt identity. Names come from original GUI.xtc
// IMAGEDIRECTORY5 records and CubeWnd.xcr BUTTON_DESCRIPTOR strings; the
// guest container layouts were verified against original vtables 0x82097EF0
// (VirtualXTC2) and 0x82097B70 (older VirtualXTC). Decorative GUI_Arrow_*
// textures are deliberately NOT origins: only GUI_Button_* records classify.
namespace DarkRecomp::Native {
bool copyRenderMemory(uint8_t* base, uint64_t address, void* output, size_t size);
}
namespace DarkRecomp::Prompts {
enum class Origin : uint8_t {
    Unknown = 0,
    A, B, X, Y, LB, RB, LT, RT, LS, RS, Start, Back,
    DUp, DDown, DLeft, DRight, DUD, DRL,
    LeftStick, RightStick,
    // Generic 360 stick tilt/click art shared by two actions (CubeWnd.xcr
    // LMENU_CONTROLLER rows bind BOTH move and look to 360_S/SLR/SUD, and
    // BOTH crouch and aim to 360_C). No single-side meaning is correct.
    MoveLook, CrouchAim,
};
inline Origin originFromName(std::string_view name) {
    if (name == "GUI_Button_A") return Origin::A;
    if (name == "GUI_Button_B") return Origin::B;
    if (name == "GUI_Button_X") return Origin::X;
    if (name == "GUI_Button_Y") return Origin::Y;
    if (name == "GUI_Button_LB") return Origin::LB;
    if (name == "GUI_Button_RB") return Origin::RB;
    if (name == "GUI_Button_LT") return Origin::LT;
    if (name == "GUI_Button_RT") return Origin::RT;
    if (name == "GUI_Button_L") return Origin::LeftStick;
    if (name == "GUI_Button_R") return Origin::RightStick;
    if (name == "GUI_Button_LC") return Origin::LS;
    if (name == "GUI_Button_RC") return Origin::RS;
    if (name == "GUI_Button_LS") return Origin::LeftStick;
    if (name == "GUI_Button_360_S" || name == "GUI_Button_360_SLR" || name == "GUI_Button_360_SUD")
        return Origin::MoveLook;
    if (name == "GUI_Button_360_C") return Origin::CrouchAim;
    if (name == "GUI_Button_L_LR" || name == "GUI_Button_L_UD") return Origin::LeftStick;
    if (name == "GUI_Button_R_LR" || name == "GUI_Button_R_UD") return Origin::RightStick;
    if (name == "GUI_Button_Start") return Origin::Start;
    if (name == "GUI_Button_Back") return Origin::Back;
    if (name == "GUI_Button_DUp") return Origin::DUp;
    if (name == "GUI_Button_DDown") return Origin::DDown;
    if (name == "GUI_Button_DLeft") return Origin::DLeft;
    if (name == "GUI_Button_DRight") return Origin::DRight;
    if (name == "GUI_Button_DUD") return Origin::DUD;
    if (name == "GUI_Button_DRL") return Origin::DRL;
    return Origin::Unknown;
}
inline const char* originName(Origin origin) {
    switch (origin) {
    case Origin::A: return "GUI_Button_A";
    case Origin::B: return "GUI_Button_B";
    case Origin::X: return "GUI_Button_X";
    case Origin::Y: return "GUI_Button_Y";
    case Origin::LB: return "GUI_Button_LB";
    case Origin::RB: return "GUI_Button_RB";
    case Origin::LT: return "GUI_Button_LT";
    case Origin::RT: return "GUI_Button_RT";
    case Origin::LS: return "GUI_Button_LC";
    case Origin::RS: return "GUI_Button_RC";
    case Origin::Start: return "GUI_Button_Start";
    case Origin::Back: return "GUI_Button_Back";
    case Origin::DUp: return "GUI_Button_DUp";
    case Origin::DDown: return "GUI_Button_DDown";
    case Origin::DLeft: return "GUI_Button_DLeft";
    case Origin::DRight: return "GUI_Button_DRight";
    case Origin::DUD: return "GUI_Button_DUD";
    case Origin::DRL: return "GUI_Button_DRL";
    case Origin::LeftStick: return "GUI_Button_L";
    case Origin::RightStick: return "GUI_Button_R";
    case Origin::MoveLook: return "GUI_Button_360_S/SLR/SUD";
    case Origin::CrouchAim: return "GUI_Button_360_C";
    default: return "";
    }
}
// Production bounded readers shared by the render-trace hooks and the native
// tests. Exact vtable, checked 64-bit arithmetic, every guest read and
// bounded NUL termination; fail closed for unknown/malformed metadata.
inline bool promptReadBe32(uint8_t* base, uint64_t address, uint32_t& out) {
    if (!base || !address || address + 4 > 0x100000000ull) return false;
    uint8_t bytes[4];
    if (!Native::copyRenderMemory(base, address, bytes, 4)) return false;
    out = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
    return true;
}
inline bool promptReadNulName(uint8_t* base, uint32_t address, char out[64]) {
    if (!base || !address || uint64_t(address) + 64 > 0x100000000ull) return false;
    uint8_t bytes[64];
    if (!Native::copyRenderMemory(base, address, bytes, sizeof(bytes))) return false;
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        if (!bytes[i]) {
            if (!i) return false;
            for (unsigned j = 0; j < i; ++j) out[j] = char(bytes[j]);
            out[i] = 0;
            return true;
        }
    }
    return false;
}
// VirtualXTC2: table=BE32(container+136), count=BE32(table),
// name pointer=BE32(table+52*actualIndex+44). No extra +4.
inline uint8_t resolveXtc2Origin(uint8_t* base, uint32_t container, uint32_t actualIndex, char nameOut[64] = nullptr) {
    uint32_t vtable = 0;
    if (!promptReadBe32(base, container, vtable) || vtable != 0x82097EF0) return 0;
    const uint64_t tableAddr = uint64_t(container) + 136;
    uint32_t table = 0;
    if (tableAddr + 4 > 0x100000000ull || !promptReadBe32(base, tableAddr, table) || !table) return 0;
    uint32_t count = 0;
    if (!promptReadBe32(base, table, count) || actualIndex >= count) return 0;
    const uint64_t slot = uint64_t(table) + uint64_t(52) * actualIndex + 44;
    if (slot + 4 > 0x100000000ull) return 0;
    uint32_t namePtr = 0;
    if (!promptReadBe32(base, slot, namePtr) || !namePtr) return 0;
    char name[64]{};
    if (!promptReadNulName(base, namePtr, name)) return 0;
    if (nameOut) for (unsigned i = 0; i < 64; ++i) { nameOut[i] = name[i]; if (!name[i]) break; }
    return uint8_t(originFromName(name));
}
// Older VirtualXTC: collection=BE32(container+56), count=BE32(collection+4),
// pointer table=BE32(collection+24), record=BE32(pointers+4*index),
// inline NUL name at record+24 bounded before field record+56.
inline uint8_t resolveXtcOrigin(uint8_t* base, uint32_t container, uint32_t actualIndex) {
    uint32_t vtable = 0;
    if (!promptReadBe32(base, container, vtable) || vtable != 0x82097B70) return 0;
    const uint64_t collectionAddr = uint64_t(container) + 56;
    uint32_t collection = 0;
    if (collectionAddr + 4 > 0x100000000ull || !promptReadBe32(base, collectionAddr, collection) || !collection)
        return 0;
    uint32_t count = 0, pointers = 0, record = 0;
    if (!promptReadBe32(base, uint64_t(collection) + 4, count) || actualIndex >= count) return 0;
    if (!promptReadBe32(base, uint64_t(collection) + 24, pointers) || !pointers) return 0;
    const uint64_t entry = uint64_t(pointers) + uint64_t(4) * actualIndex;
    if (entry + 4 > 0x100000000ull || !promptReadBe32(base, entry, record) || !record) return 0;
    const uint64_t nameAddr = uint64_t(record) + 24;
    if (nameAddr + 1 > 0x100000000ull || nameAddr + 32 > uint64_t(record) + 56) return 0;
    uint8_t bytes[32]{};
    if (!Native::copyRenderMemory(base, nameAddr, bytes, sizeof(bytes))) return 0;
    char name[32]{};
    unsigned length = 0;
    for (; length < sizeof(bytes) && length < 31; ++length) {
        if (!bytes[length]) break;
        name[length] = char(bytes[length]);
    }
    if (!length || length >= sizeof(bytes) || bytes[length] != 0) return 0;
    return uint8_t(originFromName(name));
}
} // namespace DarkRecomp::Prompts
