#include "exports.h"
#include "aob_scanner.h"
#include "code_injector.h"
#include "memory_layout.h"
#include "skill_bypass_injector.h"
#include <mutex>
#include <string>

// 全局状态
static HANDLE g_processHandle = nullptr;
static CodeInjector g_weaponInjector;   // 武器Hook
static CodeInjector g_armorInjector;    // 装备Hook
static SkillBypassInjector g_skillBypassInjector; // 技能学习条件绕过
static std::recursive_mutex g_mutex;
static std::string g_lastError;

// 时间戳和基址缓存，用于自动切换
static QWORD g_weaponTimestamp = 0;
static QWORD g_armorTimestamp = 0;
static QWORD g_lastWeaponBase = 0;
static QWORD g_lastArmorBase = 0;

static void SetLastError(const char* msg) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_lastError = msg;
}

// 更新时间戳并返回当前活动的基址
static QWORD GetActiveEquipmentBase() {
    QWORD weaponBase = g_weaponInjector.GetEquipmentBase();
    QWORD armorBase = g_armorInjector.GetEquipmentBase();

    // 检查哪个基址最近被更新
    if (weaponBase != g_lastWeaponBase && weaponBase != 0) {
        g_lastWeaponBase = weaponBase;
        g_weaponTimestamp = GetTickCount64();
    }
    if (armorBase != g_lastArmorBase && armorBase != 0) {
        g_lastArmorBase = armorBase;
        g_armorTimestamp = GetTickCount64();
    }

    // 返回最近更新的基址
    if (g_armorTimestamp > g_weaponTimestamp && armorBase != 0) {
        return armorBase;
    } else if (weaponBase != 0) {
        return weaponBase;
    } else if (armorBase != 0) {
        return armorBase;
    }
    return 0;
}

// 获取当前装备类型
static EquipmentType GetCurrentType() {
    QWORD weaponBase = g_weaponInjector.GetEquipmentBase();
    QWORD armorBase = g_armorInjector.GetEquipmentBase();

    // 更新时间戳
    if (weaponBase != g_lastWeaponBase && weaponBase != 0) {
        g_lastWeaponBase = weaponBase;
        g_weaponTimestamp = GetTickCount64();
    }
    if (armorBase != g_lastArmorBase && armorBase != 0) {
        g_lastArmorBase = armorBase;
        g_armorTimestamp = GetTickCount64();
    }

    // 判断当前类型
    if (g_armorTimestamp > g_weaponTimestamp && armorBase != 0) {
        return EQUIP_TYPE_ARMOR;
    } else if (weaponBase != 0) {
        return EQUIP_TYPE_WEAPON;
    } else if (armorBase != 0) {
        return EQUIP_TYPE_ARMOR;
    }
    return EQUIP_TYPE_UNKNOWN;
}

extern "C" {

NIOH3AFFIXCORE_API bool __cdecl AttachProcess(DWORD processId) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle != nullptr) {
        SetLastError("Already attached to a process");
        return false;
    }

    g_processHandle = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE,
        processId
    );

    if (g_processHandle == nullptr) {
        SetLastError("Failed to open process");
        return false;
    }

    // 重置时间戳和缓存
    g_weaponTimestamp = 0;
    g_armorTimestamp = 0;
    g_lastWeaponBase = 0;
    g_lastArmorBase = 0;

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API void __cdecl DetachProcess() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_weaponInjector.IsEnabled()) {
        g_weaponInjector.Disable();
    }
    if (g_armorInjector.IsEnabled()) {
        g_armorInjector.Disable();
    }
    if (g_skillBypassInjector.IsEnabled()) {
        g_skillBypassInjector.Disable();
    }
    g_skillBypassInjector.Cleanup();

    if (g_processHandle != nullptr) {
        CloseHandle(g_processHandle);
        g_processHandle = nullptr;
    }

    // 重置时间戳和缓存
    g_weaponTimestamp = 0;
    g_armorTimestamp = 0;
    g_lastWeaponBase = 0;
    g_lastArmorBase = 0;

    g_lastError.clear();
}

NIOH3AFFIXCORE_API bool __cdecl IsAttached() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_processHandle != nullptr;
}

NIOH3AFFIXCORE_API bool __cdecl EnableCapture() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    bool weaponEnabled = g_weaponInjector.IsEnabled();
    bool armorEnabled = g_armorInjector.IsEnabled();

    // 如果两个都已启用，直接返回成功
    if (weaponEnabled && armorEnabled) {
        return true;
    }

    // 启用武器Hook
    if (!weaponEnabled) {
        QWORD weaponInjectionPoint = AobScan(g_processHandle, AobPatterns::WEAPON_CAPTURE_AOB);
        if (weaponInjectionPoint == 0) {
            SetLastError("Weapon AOB pattern not found. Game version may be incompatible.");
            return false;
        }

        if (!g_weaponInjector.Initialize(g_processHandle, weaponInjectionPoint, HookType::Weapon)) {
            SetLastError("Failed to initialize weapon code injector");
            return false;
        }

        if (!g_weaponInjector.Enable()) {
            SetLastError("Failed to enable weapon hook");
            return false;
        }
    }

    // 启用装备Hook
    if (!armorEnabled) {
        QWORD armorInjectionPoint = AobScan(g_processHandle, AobPatterns::ARMOR_CAPTURE_AOB);
        if (armorInjectionPoint == 0) {
            // 装备Hook找不到不算致命错误，只记录警告
            // 武器Hook已经成功，可以继续
            g_lastError = "Armor AOB pattern not found. Armor editing may not work.";
            return true; // 仍然返回成功，因为武器Hook已启用
        }

        if (!g_armorInjector.Initialize(g_processHandle, armorInjectionPoint, HookType::Armor)) {
            // 同样，装备Hook初始化失败不算致命错误
            g_lastError = "Failed to initialize armor code injector. Armor editing may not work.";
            return true;
        }

        if (!g_armorInjector.Enable()) {
            g_lastError = "Failed to enable armor hook. Armor editing may not work.";
            return true;
        }
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API void __cdecl DisableCapture() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_weaponInjector.IsEnabled()) {
        g_weaponInjector.Disable();
    }
    if (g_armorInjector.IsEnabled()) {
        g_armorInjector.Disable();
    }
}

NIOH3AFFIXCORE_API bool __cdecl IsCaptureEnabled() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // 只要武器Hook启用就算启用
    return g_weaponInjector.IsEnabled();
}

NIOH3AFFIXCORE_API int __cdecl GetCurrentEquipmentType() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return (int)GetCurrentType();
}

NIOH3AFFIXCORE_API bool __cdecl IsWeaponMode() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    EquipmentType type = GetCurrentType();
    return type == EQUIP_TYPE_WEAPON || type == EQUIP_TYPE_UNKNOWN;
}

NIOH3AFFIXCORE_API QWORD __cdecl GetEquipmentBase() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return GetActiveEquipmentBase();
}

NIOH3AFFIXCORE_API bool __cdecl IsWeaponHookEnabled() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_weaponInjector.IsEnabled();
}

NIOH3AFFIXCORE_API bool __cdecl IsArmorHookEnabled() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_armorInjector.IsEnabled();
}

NIOH3AFFIXCORE_API QWORD __cdecl GetWeaponBase() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_weaponInjector.GetEquipmentBase();
}

NIOH3AFFIXCORE_API QWORD __cdecl GetArmorBase() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_armorInjector.GetEquipmentBase();
}

NIOH3AFFIXCORE_API bool __cdecl ReadAffix(int slotIndex, int* outId, int* outLevel) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    if (slotIndex < 0 || slotIndex >= MemoryLayout::AFFIX_SLOT_COUNT) {
        SetLastError("Invalid slot index");
        return false;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    // 读取词条 ID
    int idOffset = MemoryLayout::GetAffixIdOffset(slotIndex);
    QWORD idAddr = equipBase + idOffset;
    SIZE_T bytesRead;
    int id = 0;
    if (!ReadProcessMemory(g_processHandle, (LPCVOID)idAddr, &id, sizeof(id), &bytesRead)) {
        SetLastError("Failed to read affix ID");
        return false;
    }

    // 读取词条等级
    int levelOffset = MemoryLayout::GetAffixLevelOffset(slotIndex);
    QWORD levelAddr = equipBase + levelOffset;
    int level = 0;
    if (!ReadProcessMemory(g_processHandle, (LPCVOID)levelAddr, &level, sizeof(level), &bytesRead)) {
        SetLastError("Failed to read affix level");
        return false;
    }

    if (outId) *outId = id;
    if (outLevel) *outLevel = level;

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl WriteAffix(int slotIndex, int id, int level) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    if (slotIndex < 0 || slotIndex >= MemoryLayout::AFFIX_SLOT_COUNT) {
        SetLastError("Invalid slot index");
        return false;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    // 写入词条 ID
    int idOffset = MemoryLayout::GetAffixIdOffset(slotIndex);
    QWORD idAddr = equipBase + idOffset;
    SIZE_T bytesWritten;
    if (!WriteProcessMemory(g_processHandle, (LPVOID)idAddr, &id, sizeof(id), &bytesWritten)) {
        SetLastError("Failed to write affix ID");
        return false;
    }

    // 写入词条等级
    int levelOffset = MemoryLayout::GetAffixLevelOffset(slotIndex);
    QWORD levelAddr = equipBase + levelOffset;
    if (!WriteProcessMemory(g_processHandle, (LPVOID)levelAddr, &level, sizeof(level), &bytesWritten)) {
        SetLastError("Failed to write affix level");
        return false;
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl ReadAffixEx(
    int slotIndex,
    int* outId,
    int* outLevel,
    uint8_t* outPrefix1,
    uint8_t* outPrefix2,
    uint8_t* outPrefix3,
    uint8_t* outPrefix4
) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    if (slotIndex < 0 || slotIndex >= MemoryLayout::AFFIX_SLOT_COUNT) {
        SetLastError("Invalid slot index");
        return false;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    SIZE_T bytesRead;

    // Read affix ID
    int idOffset = MemoryLayout::GetAffixIdOffset(slotIndex);
    QWORD idAddr = equipBase + idOffset;
    int id = 0;
    if (!ReadProcessMemory(g_processHandle, (LPCVOID)idAddr, &id, sizeof(id), &bytesRead)) {
        SetLastError("Failed to read affix ID");
        return false;
    }

    // Read affix level
    int levelOffset = MemoryLayout::GetAffixLevelOffset(slotIndex);
    QWORD levelAddr = equipBase + levelOffset;
    int level = 0;
    if (!ReadProcessMemory(g_processHandle, (LPCVOID)levelAddr, &level, sizeof(level), &bytesRead)) {
        SetLastError("Failed to read affix level");
        return false;
    }

    // Read 4 prefix bytes (level offset + 4..+7)
    int prefixOffset = MemoryLayout::GetAffixPrefixOffset(slotIndex, 0);
    QWORD prefixAddr = equipBase + prefixOffset;
    uint8_t prefixes[4] = { 0, 0, 0, 0 };
    if (!ReadProcessMemory(g_processHandle, (LPCVOID)prefixAddr, prefixes, sizeof(prefixes), &bytesRead)) {
        SetLastError("Failed to read affix prefixes");
        return false;
    }

    if (outId) *outId = id;
    if (outLevel) *outLevel = level;
    if (outPrefix1) *outPrefix1 = prefixes[0];
    if (outPrefix2) *outPrefix2 = prefixes[1];
    if (outPrefix3) *outPrefix3 = prefixes[2];
    if (outPrefix4) *outPrefix4 = prefixes[3];

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl WriteAffixExMasked(
    int slotIndex,
    int id,
    int level,
    uint8_t prefix1,
    uint8_t prefix2,
    uint8_t prefix3,
    uint8_t prefix4,
    uint32_t fieldMask
) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    if (slotIndex < 0 || slotIndex >= MemoryLayout::AFFIX_SLOT_COUNT) {
        SetLastError("Invalid slot index");
        return false;
    }

    if (fieldMask == 0) {
        g_lastError.clear();
        return true;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    SIZE_T bytesWritten;

    // bit0: id
    if ((fieldMask & (1u << 0)) != 0) {
        int idOffset = MemoryLayout::GetAffixIdOffset(slotIndex);
        QWORD idAddr = equipBase + idOffset;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)idAddr, &id, sizeof(id), &bytesWritten)) {
            SetLastError("Failed to write affix ID");
            return false;
        }
    }

    // bit1: level
    if ((fieldMask & (1u << 1)) != 0) {
        int levelOffset = MemoryLayout::GetAffixLevelOffset(slotIndex);
        QWORD levelAddr = equipBase + levelOffset;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)levelAddr, &level, sizeof(level), &bytesWritten)) {
            SetLastError("Failed to write affix level");
            return false;
        }
    }

    // bit2..bit5: prefix1..prefix4
    if ((fieldMask & ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5))) != 0) {
        int prefixOffset = MemoryLayout::GetAffixPrefixOffset(slotIndex, 0);
        QWORD prefixAddr = equipBase + prefixOffset;

        if ((fieldMask & (1u << 2)) != 0) {
            if (!WriteProcessMemory(g_processHandle, (LPVOID)(prefixAddr + 0), &prefix1, sizeof(prefix1), &bytesWritten)) {
                SetLastError("Failed to write affix prefix1");
                return false;
            }
        }
        if ((fieldMask & (1u << 3)) != 0) {
            if (!WriteProcessMemory(g_processHandle, (LPVOID)(prefixAddr + 1), &prefix2, sizeof(prefix2), &bytesWritten)) {
                SetLastError("Failed to write affix prefix2");
                return false;
            }
        }
        if ((fieldMask & (1u << 4)) != 0) {
            if (!WriteProcessMemory(g_processHandle, (LPVOID)(prefixAddr + 2), &prefix3, sizeof(prefix3), &bytesWritten)) {
                SetLastError("Failed to write affix prefix3");
                return false;
            }
        }
        if ((fieldMask & (1u << 5)) != 0) {
            if (!WriteProcessMemory(g_processHandle, (LPVOID)(prefixAddr + 3), &prefix4, sizeof(prefix4), &bytesWritten)) {
                SetLastError("Failed to write affix prefix4");
                return false;
            }
        }
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API const char* __cdecl GetLastErrorMessage() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_lastError.c_str();
}

NIOH3AFFIXCORE_API bool __cdecl ReadEquipmentBasics(
    short* outItemId,
    short* outTransmogId,
    short* outLevel,
    int* outUnderworldSkillId,
    int* outFamiliarity,
    bool* outIsUnderworld
) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    SIZE_T bytesRead;
    bool isWeapon = IsWeaponMode();

    // 读取物品ID (2 bytes)
    if (outItemId) {
        short itemId = 0;
        QWORD addr = equipBase + EquipmentLayout::ITEM_ID_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &itemId, sizeof(itemId), &bytesRead)) {
            SetLastError("Failed to read item ID");
            return false;
        }
        *outItemId = itemId;
    }

    // 读取幻化ID (2 bytes)
    if (outTransmogId) {
        short transmogId = 0;
        QWORD addr = equipBase + EquipmentLayout::TRANSMOG_ID_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &transmogId, sizeof(transmogId), &bytesRead)) {
            SetLastError("Failed to read transmog ID");
            return false;
        }
        *outTransmogId = transmogId;
    }

    // 读取等级 (2 bytes)
    if (outLevel) {
        short level = 0;
        QWORD addr = equipBase + EquipmentLayout::LEVEL_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &level, sizeof(level), &bytesRead)) {
            SetLastError("Failed to read level");
            return false;
        }
        *outLevel = level;
    }

    // 以下字段只有武器才有
    if (isWeapon) {
        // 读取地狱技能ID (4 bytes)
        if (outUnderworldSkillId) {
            int skillId = 0;
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_SKILL_ID_OFFSET;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &skillId, sizeof(skillId), &bytesRead)) {
                SetLastError("Failed to read underworld skill ID");
                return false;
            }
            *outUnderworldSkillId = skillId;
        }

        // 读取爱用度 (4 bytes)
        if (outFamiliarity) {
            int familiarity = 0;
            QWORD addr = equipBase + EquipmentLayout::FAMILIARITY_OFFSET;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &familiarity, sizeof(familiarity), &bytesRead)) {
                SetLastError("Failed to read familiarity");
                return false;
            }
            *outFamiliarity = familiarity;
        }

        // 读取是否是地狱武器 (1 bit)
        if (outIsUnderworld) {
            BYTE flagByte = 0;
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_FLAG_OFFSET;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &flagByte, sizeof(flagByte), &bytesRead)) {
                SetLastError("Failed to read underworld flag");
                return false;
            }
            *outIsUnderworld = (flagByte & (1 << EquipmentLayout::UNDERWORLD_FLAG_BIT)) != 0;
        }
    } else {
        // 装备模式下，武器独有字段返回默认值
        if (outUnderworldSkillId) *outUnderworldSkillId = 0;
        if (outFamiliarity) *outFamiliarity = 0;
        if (outIsUnderworld) *outIsUnderworld = false;
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl ReadEquipmentBasicsEx(
    short* outItemId,
    short* outTransmogId,
    short* outLevel,
    uint8_t* outEquipPlusValue,
    int* outQuality,
    int* outUnderworldSkillId,
    int* outFamiliarity,
    bool* outIsUnderworld
) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    SIZE_T bytesRead;
    bool isWeapon = IsWeaponMode();

    // 读取物品ID (2 bytes)
    if (outItemId) {
        short itemId = 0;
        QWORD addr = equipBase + EquipmentLayout::ITEM_ID_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &itemId, sizeof(itemId), &bytesRead)) {
            SetLastError("Failed to read item ID");
            return false;
        }
        *outItemId = itemId;
    }

    // 读取幻化ID (2 bytes)
    if (outTransmogId) {
        short transmogId = 0;
        QWORD addr = equipBase + EquipmentLayout::TRANSMOG_ID_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &transmogId, sizeof(transmogId), &bytesRead)) {
            SetLastError("Failed to read transmog ID");
            return false;
        }
        *outTransmogId = transmogId;
    }

    // 读取等级 (2 bytes)
    if (outLevel) {
        short level = 0;
        QWORD addr = equipBase + EquipmentLayout::LEVEL_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &level, sizeof(level), &bytesRead)) {
            SetLastError("Failed to read level");
            return false;
        }
        *outLevel = level;
    }

    if (outEquipPlusValue) {
        uint8_t equipPlusValue = 0;
        QWORD addr = equipBase + EquipmentLayout::EQUIPMENT_PLUS_VALUE_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &equipPlusValue, sizeof(equipPlusValue), &bytesRead)) {
            SetLastError("Failed to read equip plus value");
            return false;
        }
        *outEquipPlusValue = equipPlusValue;
    }

    // 读取品质/稀有度 (4 bytes)
    if (outQuality) {
        int quality = 0;
        QWORD addr = equipBase + EquipmentLayout::QUALITY_OFFSET;
        if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &quality, sizeof(quality), &bytesRead)) {
            SetLastError("Failed to read quality");
            return false;
        }
        *outQuality = quality;
    }

    // 以下字段只有武器才有
    if (isWeapon) {
        // 读取地狱技能ID (4 bytes)
        if (outUnderworldSkillId) {
            int skillId = 0;
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_SKILL_ID_OFFSET;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &skillId, sizeof(skillId), &bytesRead)) {
                SetLastError("Failed to read underworld skill ID");
                return false;
            }
            *outUnderworldSkillId = skillId;
        }

        // 读取爱用度 (4 bytes)
        if (outFamiliarity) {
            int familiarity = 0;
            QWORD addr = equipBase + EquipmentLayout::FAMILIARITY_OFFSET;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &familiarity, sizeof(familiarity), &bytesRead)) {
                SetLastError("Failed to read familiarity");
                return false;
            }
            *outFamiliarity = familiarity;
        }

        // 读取是否是地狱武器 (1 bit)
        if (outIsUnderworld) {
            BYTE flagByte = 0;
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_FLAG_OFFSET;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &flagByte, sizeof(flagByte), &bytesRead)) {
                SetLastError("Failed to read underworld flag");
                return false;
            }
            *outIsUnderworld = (flagByte & (1 << EquipmentLayout::UNDERWORLD_FLAG_BIT)) != 0;
        }
    } else {
        // 装备模式下，武器独有字段返回默认值
        if (outUnderworldSkillId) *outUnderworldSkillId = 0;
        if (outFamiliarity) *outFamiliarity = 0;
        if (outIsUnderworld) *outIsUnderworld = false;
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl WriteEquipmentBasics(
    short itemId,
    short transmogId,
    short level,
    int underworldSkillId,
    int familiarity,
    bool isUnderworld
) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    SIZE_T bytesWritten;
    SIZE_T bytesRead;
    bool isWeapon = IsWeaponMode();

    // 写入物品ID (2 bytes)
    {
        QWORD addr = equipBase + EquipmentLayout::ITEM_ID_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &itemId, sizeof(itemId), &bytesWritten)) {
            SetLastError("Failed to write item ID");
            return false;
        }
    }

    // 写入幻化ID (2 bytes)
    {
        QWORD addr = equipBase + EquipmentLayout::TRANSMOG_ID_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &transmogId, sizeof(transmogId), &bytesWritten)) {
            SetLastError("Failed to write transmog ID");
            return false;
        }
    }

    // 写入等级 (2 bytes)
    {
        QWORD addr = equipBase + EquipmentLayout::LEVEL_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &level, sizeof(level), &bytesWritten)) {
            SetLastError("Failed to write level");
            return false;
        }
    }

    // 以下字段只有武器才写入
    if (isWeapon) {
        // 写入地狱技能ID (4 bytes)
        {
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_SKILL_ID_OFFSET;
            if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &underworldSkillId, sizeof(underworldSkillId), &bytesWritten)) {
                SetLastError("Failed to write underworld skill ID");
                return false;
            }
        }

        // 写入爱用度 (4 bytes)
        {
            QWORD addr = equipBase + EquipmentLayout::FAMILIARITY_OFFSET;
            if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &familiarity, sizeof(familiarity), &bytesWritten)) {
                SetLastError("Failed to write familiarity");
                return false;
            }
        }

        // 写入是否是地狱武器 (1 bit) - 需要读取-修改-写入
        {
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_FLAG_OFFSET;
            BYTE flagByte = 0;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &flagByte, sizeof(flagByte), &bytesRead)) {
                SetLastError("Failed to read underworld flag for modification");
                return false;
            }

            if (isUnderworld) {
                flagByte |= (1 << EquipmentLayout::UNDERWORLD_FLAG_BIT);
            } else {
                flagByte &= ~(1 << EquipmentLayout::UNDERWORLD_FLAG_BIT);
            }

            if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &flagByte, sizeof(flagByte), &bytesWritten)) {
                SetLastError("Failed to write underworld flag");
                return false;
            }
        }
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl WriteEquipmentBasicsEx(
    short itemId,
    short transmogId,
    short level,
    uint8_t equipPlusValue,
    int quality,
    int underworldSkillId,
    int familiarity,
    bool isUnderworld
) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    QWORD equipBase = GetActiveEquipmentBase();
    if (equipBase == 0) {
        SetLastError("Equipment base address not captured yet");
        return false;
    }

    SIZE_T bytesWritten;
    SIZE_T bytesRead;
    bool isWeapon = IsWeaponMode();

    // 写入物品ID (2 bytes)
    {
        QWORD addr = equipBase + EquipmentLayout::ITEM_ID_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &itemId, sizeof(itemId), &bytesWritten)) {
            SetLastError("Failed to write item ID");
            return false;
        }
    }

    // 写入幻化ID (2 bytes)
    {
        QWORD addr = equipBase + EquipmentLayout::TRANSMOG_ID_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &transmogId, sizeof(transmogId), &bytesWritten)) {
            SetLastError("Failed to write transmog ID");
            return false;
        }
    }

    // 写入等级 (2 bytes)
    {
        QWORD addr = equipBase + EquipmentLayout::LEVEL_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &level, sizeof(level), &bytesWritten)) {
            SetLastError("Failed to write level");
            return false;
        }
    }

    {
        QWORD addr = equipBase + EquipmentLayout::EQUIPMENT_PLUS_VALUE_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &equipPlusValue, sizeof(equipPlusValue), &bytesWritten)) {
            SetLastError("Failed to write equip plus value");
            return false;
        }
    }

    // 写入品质/稀有度 (4 bytes)
    {
        QWORD addr = equipBase + EquipmentLayout::QUALITY_OFFSET;
        if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &quality, sizeof(quality), &bytesWritten)) {
            SetLastError("Failed to write quality");
            return false;
        }
    }

    // 以下字段只有武器才写入
    if (isWeapon) {
        // 写入地狱技能ID (4 bytes)
    if (isWeapon) {
        {
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_SKILL_ID_OFFSET;
            if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &underworldSkillId, sizeof(underworldSkillId), &bytesWritten)) {
                SetLastError("Failed to write underworld skill ID");
                return false;
            }
        }

        // 写入爱用度 (4 bytes)
        {
            QWORD addr = equipBase + EquipmentLayout::FAMILIARITY_OFFSET;
            if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &familiarity, sizeof(familiarity), &bytesWritten)) {
                SetLastError("Failed to write familiarity");
                return false;
            }
        }

        // 写入是否是地狱武器 (1 bit) - 需要读-改-写
        {
        {
            QWORD addr = equipBase + EquipmentLayout::UNDERWORLD_FLAG_OFFSET;
            BYTE flagByte = 0;
            if (!ReadProcessMemory(g_processHandle, (LPCVOID)addr, &flagByte, sizeof(flagByte), &bytesRead)) {
                SetLastError("Failed to read underworld flag for modification");
                return false;
            }

            if (isUnderworld) {
                flagByte |= (1 << EquipmentLayout::UNDERWORLD_FLAG_BIT);
            } else {
                flagByte &= ~(1 << EquipmentLayout::UNDERWORLD_FLAG_BIT);
            }

            if (!WriteProcessMemory(g_processHandle, (LPVOID)addr, &flagByte, sizeof(flagByte), &bytesWritten)) {
                SetLastError("Failed to write underworld flag");
                return false;
            }
        }
    }
}
}
        

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl EnableSkillBypass() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_processHandle == nullptr) {
        SetLastError("Not attached to any process");
        return false;
    }

    // 如果已经启用，直接返回成功
    if (g_skillBypassInjector.IsEnabled()) {
        return true;
    }

    // 初始化（如果还没初始化）
    if (!g_skillBypassInjector.Initialize(g_processHandle)) {
        SetLastError("Failed to find skill bypass hook points. Game version may be incompatible.");
        return false;
    }

    if (!g_skillBypassInjector.Enable()) {
        SetLastError("Failed to enable skill bypass");
        return false;
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl DisableSkillBypass() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (!g_skillBypassInjector.IsEnabled()) {
        return true;
    }

    if (!g_skillBypassInjector.Disable()) {
        SetLastError("Failed to disable skill bypass");
        return false;
    }

    g_lastError.clear();
    return true;
}

NIOH3AFFIXCORE_API bool __cdecl IsSkillBypassEnabled() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_skillBypassInjector.IsEnabled();
}

} // extern "C"
