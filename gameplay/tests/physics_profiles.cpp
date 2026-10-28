// Exercise the production profile paths without installing hooks or accessing game memory.
#include "../src/physics.cpp"
#include <assert.h>

Config g_cfg;
static char root[MAX_PATH], ini[MAX_PATH];
void Log(const char*, ...) {}
double Now() { assert(false && "profiles must not simulate the game"); return 0; }
void* Game_PlayerInterface(void*) { assert(false); return nullptr; }
InputSnapshot Input_Get() { assert(false); return {}; }
CarCharacter Cars_Character(const char*, float, bool) { assert(false); return {}; }
bool LegacyGripBalance(const char*, const char*, float*, float*) { return false; }
const char* ModDir() { return root; }
const char* IniPath() { return ini; }
void IniSave_Flush() {}
void IniSave_Queue(const std::vector<IniWrite>& writes) {
    for (const auto& w : writes)
        assert(WritePrivateProfileStringA(w.section.c_str(), w.key.empty() ? nullptr : w.key.c_str(),
                                         w.erase || w.key.empty() ? nullptr : w.value.c_str(), w.file.c_str()));
}

int main() {
    char temp[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp));
    assert(GetTempFileNameA(temp, "nfw", 0, root));
    assert(DeleteFileA(root));
    assert(CreateDirectoryA(root, nullptr));
    strcat(root, "\\");
    snprintf(ini, sizeof(ini), "%snfsmw_gameplay.ini", root);
    assert(WritePrivateProfileStringA("Physics", "PremadeGroups", "1", ini));
    assert(WritePrivateProfileStringA("Group.Test", "Cars", "clio", ini));
    assert(WritePrivateProfileStringA("Group.Test", "RealWeight", "0", ini));
    assert(WritePrivateProfileStringA("Car.legacy", "Grip", "1", ini));
    assert(Physics_IsPerCar(&g_cfg.realWeight));
    Physics_SetCar("clio");
    assert(g_cfg.realWeight == 0 && Physics_Section() == "Group.Test");
    Physics_SetCar("legacy");
    assert(g_cfg.realWeight == 1); // Missing key inherits Shared, not the previous group.
    Physics_SetCar("shared");
    assert(g_cfg.realWeight == 1);
    Physics_SetCar("clio");
    assert(g_cfg.realWeight == 0);
    std::string group = Physics_ExportGroup("Test");
    assert(GetPrivateProfileIntA("Values", "RealWeight", -1, group.c_str()) == 0);
    assert(WritePrivateProfileStringA("Values", "RealWeight", "1", group.c_str()));
    Physics_ImportProfile("Group - Test.ini");
    assert(g_cfg.realWeight == 1);
    g_cfg.realWeight = 0;
    Physics_UseOwn();
    assert(GetPrivateProfileIntA("Car.clio", "RealWeight", -1, ini) == 0);
    std::string car = Physics_ExportCar("clio");
    assert(GetPrivateProfileIntA("Values", "RealWeight", -1, car.c_str()) == 0);
    assert(WritePrivateProfileStringA("Values", "RealWeight", "1", car.c_str()));
    Physics_ImportProfile("Car - clio.ini");
    assert(g_cfg.realWeight == 1);
    Physics_SetCar("shared");
    assert(g_cfg.realWeight == 1);
    Physics_SetCar("clio");
    assert(g_cfg.realWeight == 1); // Imported value survives reloading the car's section.
    assert(DeleteFileA(group.c_str()));
    assert(DeleteFileA(car.c_str()));
    assert(RemoveDirectoryA(ProfileDir().c_str()));
    assert(DeleteFileA(ini));
    root[strlen(root) - 1] = 0;
    assert(RemoveDirectoryA(root));
    puts("physics profile regression tests passed");
}