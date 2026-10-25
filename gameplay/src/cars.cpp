// Every car model's stock physics data, and the [Physics] Character factors made from it.
//
// The models are the CARS folders; each one's pvehicle collection (Attrib::FindCollection) holds
// RefSpecs to the collections the car is built from: chassis, tires, engine, transmission,
// induction (as Physics::Info::ComputeAccelerationTable, 0x673810, reads them). A collection's
// fixed fields sit in its layout (+0x18), at the offsets physics.cpp reads on the driven car; the
// others are found by name (Attrib::Collection::GetData, which follows the parent collections).
// These are the stock cars: performance parts change the driven car's values, not these.
//
// Character: each player car's springs, anti-roll bars (both per tonne), grip and torque for its
// weight and grip, against the median of the player cars, raised to the power [Physics]
// Character. The game gives each car its own values but keeps them close together; this spreads
// them, so a soft car rolls more and a stiff one less. Grip is only ever taken away (a car below
// the median gets less, none gets more): the cars get harder to drive, never easier. The
// anti-roll bars' front share is moved further from 50% the same way. Downforce is left to
// [Physics] Downforce, on each car's own aero.
// Weight: the game puts 51-55% of every car's weight on the front axle, rear- and mid-engined
// cars too; [Physics] RealWeight uses the real car's (Chassis::SetCOG, 0x68D800, places the
// centre of gravity between the axles by FRONT_WEIGHT_BIAS every step), and a car with more weight
// at the back than the game gave it gets a little more rear grip, as its wider rear tyres would.
#include "common.h"
#include "cars.h"
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr uintptr_t kFindCollection = 0x455FD0;     // const Collection* Attrib::FindCollection(class, key), cdecl
constexpr uintptr_t kCollectionGetData = 0x454190;  // void* Collection::GetData(attrib, index), thiscall; null past the end
constexpr uintptr_t kStringToKey = 0x454640;        // unsigned Attrib::StringToKey(const char*), cdecl
constexpr uintptr_t kRefSpecGetCollection = 0x4560D0;   // const Collection* RefSpec::GetCollection(), thiscall
constexpr int kCol_Layout = 0x18;

// Layout offsets (the same physics.cpp reads on the driven car).
constexpr int kChL_Spring = 0x38, kChL_Swaybar = 0x48, kChL_WeightBias = 0x6C;
constexpr int kTyL_StaticGrip = 0x38;
constexpr int kEnL_MaxRpm = 0x54, kEnL_RedLine = 0x58, kEnL_Idle = 0x5C;
constexpr int kInL_LowBoost = 0x00, kInL_HighBoost = 0x18;     // torque gained, as a share
constexpr int kTrL_TorqueSplit = 0x70;

typedef void*(__cdecl* FindCollection_t)(uint32_t cls, uint32_t key);
typedef void*(__thiscall* GetData_t)(void* collection, uint32_t attrib, uint32_t index);
typedef uint32_t(__cdecl* StringToKey_t)(const char*);
typedef void*(__thiscall* RefGet_t)(void* ref);

// Real front weight, percent: published figures for the road cars (the race M3 and C6.R as their
// road versions).
const struct { const char* car; float front; } kRealWeight[] = {
    {"911gt2", 38}, {"911turbo", 39}, {"997s", 38}, {"a3", 60}, {"a4", 59}, {"bmwm3gtr", 51},
    {"bmwm3gtre46", 50}, {"camaro", 56}, {"carreragt", 42}, {"caymans", 45}, {"clio", 38},
    {"clk500", 53}, {"cobaltss", 62}, {"corvette", 51}, {"corvettec6r", 50}, {"cts", 52},
    {"db9", 50}, {"eclipsegt", 64}, {"elise", 38}, {"fordgt", 43}, {"gallardo", 42}, {"gti", 62},
    {"gto", 55}, {"imprezawrx", 59}, {"is300", 52}, {"lancerevo8", 60}, {"monaro", 55},
    {"murcielago", 42}, {"mustanggt", 53}, {"punto", 62}, {"rx7", 50}, {"rx8", 50}, {"sl500", 52},
    {"sl65", 54}, {"slr", 51}, {"supra", 53}, {"tt", 58}, {"viper", 50},
};

struct Stock {
    std::string name;
    bool usable = false;
    float mass = 0, weightFront = 0, spring = 0, swaybar = 0, swaybarFront = 0.5f, grip = 0, torque = 0;
    float torqueSplit = 0, hp = 0;
};

uint32_t Key(const char* s) { return ((StringToKey_t)kStringToKey)(s); }
void* Data(void* col, const char* attrib, uint32_t index = 0) {
    return col ? ((GetData_t)kCollectionGetData)(col, Key(attrib), index) : nullptr;
}
uint8_t* Layout(void* col) { return col ? *(uint8_t**)((uint8_t*)col + kCol_Layout) : nullptr; }
float F(uint8_t* layout, int off) { return *(float*)(layout + off); }
void* Ref(void* pv, const char* attrib) {
    void* ref = Data(pv, attrib);
    return ref ? ((RefGet_t)kRefSpecGetCollection)(ref) : nullptr;
}

bool Read(void* pv, Stock* s) {
    void* chassis = Ref(pv, "chassis");
    void* tires = Ref(pv, "tires");
    void* engine = Ref(pv, "engine");
    void* induction = Ref(pv, "induction");
    if (uint8_t* lr = Layout(Ref(pv, "transmission"))) s->torqueSplit = F(lr, kTrL_TorqueSplit);
    uint8_t *lc = Layout(chassis), *lt = Layout(tires), *le = Layout(engine), *li = Layout(induction);
    float* mass = (float*)Data(pv, "MASS");
    if (!lc || !lt || !le || !mass || !(*mass > 100.0f)) return false;
    if (bool* u = (bool*)Data(pv, "PlayerUsable")) s->usable = *u;
    float t = *mass / 1000.0f;
    s->mass = *mass;
    s->weightFront = F(lc, kChL_WeightBias);
    s->spring = (F(lc, kChL_Spring) + F(lc, kChL_Spring + 4)) / 2 / t;
    float bf = F(lc, kChL_Swaybar), br = F(lc, kChL_Swaybar + 4);
    s->swaybar = (bf + br) / 2 / t;
    s->swaybarFront = bf + br > 0 ? bf / (bf + br) : 0.5f;
    s->grip = (F(lt, kTyL_StaticGrip) + F(lt, kTyL_StaticGrip + 4)) / 2;
    // The highest point of the torque curve (lb-ft, evenly spread from IDLE to MAX_RPM, as
    // Physics::Info::Torque reads it) up to the red line, with the turbo's or supercharger's boost.
    float lo = li ? F(li, kInL_LowBoost) : 0.0f, hi = li ? F(li, kInL_HighBoost) : 0.0f;
    float idle = F(le, kEnL_Idle), maxRpm = F(le, kEnL_MaxRpm), red = F(le, kEnL_RedLine);
    int n = 0;
    float curve[32];
    while (n < 32 && Data(engine, "TORQUE", n)) curve[n] = *(float*)Data(engine, "TORQUE", n), n++;
    for (int i = 0; i < n && n > 1; i++) {
        float x = (float)i / (n - 1);
        if (idle + (maxRpm - idle) * x > red + 1.0f) break;
        s->torque = std::max(s->torque, curve[i] * (1.0f + lo + (hi - lo) * x));
    }
    // Peak power (hp = lb-ft * rpm / 5252), the curve read between its points every 50 rpm.
    for (float rpm = idle; n > 1 && maxRpm > idle && rpm <= red; rpm += 50.0f) {
        float x = (rpm - idle) / (maxRpm - idle) * (n - 1);
        int i = std::min((int)x, n - 2);
        float f = x - i, b = 1.0f + lo + (hi - lo) * x / (n - 1);
        s->hp = std::max(s->hp, (curve[i] * (1.0f - f) + curve[i + 1] * f) * b * rpm / 5252.0f);
    }
    return s->grip > 0.0f && s->spring > 0.0f;
}

std::vector<Stock> s_cars;
struct {
    float spring = 0, swaybar = 0, grip = 0, throttle = 0;
} s_median;

float Median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

float Throttle(const Stock& s) { return s.torque / (s.mass / 1000.0f) / s.grip; }

void ReadAll() {
    static bool read;
    if (read) return;
    read = true;
    uint32_t pvehicle = Key("pvehicle");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(GameDir()) + "CARS\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        Stock s;
        s.name = fd.cFileName;
        for (auto& c : s.name) c = (char)tolower((unsigned char)c);
        void* pv = ((FindCollection_t)kFindCollection)(pvehicle, Key(s.name.c_str()));
        if (pv && Read(pv, &s)) s_cars.push_back(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::vector<float> spring, swaybar, grip, throttle;
    for (const Stock& s : s_cars)
        if (s.usable) {
            spring.push_back(s.spring), swaybar.push_back(s.swaybar), grip.push_back(s.grip);
            if (s.torque > 0.0f) throttle.push_back(Throttle(s));
        }
    s_median.spring = Median(spring), s_median.swaybar = Median(swaybar), s_median.grip = Median(grip);
    s_median.throttle = Median(throttle);
    Log("cars: %u models, %u player cars; medians: springs %.0f lb/in per tonne, anti-roll bars %.0f per tonne, "
        "grip %.2f, %.0f lb-ft per tonne per grip", (unsigned)s_cars.size(), (unsigned)grip.size(),
        s_median.spring, s_median.swaybar, s_median.grip, s_median.throttle);
}

// (value / median)^k within [lo, hi]; 1 without a median.
float Spread(float v, float median, float k, float lo, float hi) {
    if (!(v > 0.0f) || !(median > 0.0f)) return 1.0f;
    return Clamp(powf(v / median, k), lo, hi);
}

}  // namespace

// The player cars' names.
const struct { const char* car; const char* name; } kNames[] = {
    {"911gt2", "Porsche 911 GT2"}, {"911turbo", "Porsche 911 Turbo S"}, {"997s", "Porsche 911 Carrera S"},
    {"a3", "Audi A3 3.2 quattro"}, {"a4", "Audi A4 3.2 FSI quattro"}, {"bmwm3gtr", "BMW M3 GTR"},
    {"bmwm3gtre46", "BMW M3 GTR (race)"}, {"camaro", "Chevrolet Camaro SS"}, {"carreragt", "Porsche Carrera GT"},
    {"caymans", "Porsche Cayman S"}, {"clio", "Renault Clio V6"}, {"clk500", "Mercedes-Benz CLK 500"},
    {"cobaltss", "Chevrolet Cobalt SS"}, {"corvette", "Chevrolet Corvette C6"}, {"corvettec6r", "Chevrolet Corvette C6.R"},
    {"cts", "Cadillac CTS"}, {"db9", "Aston Martin DB9"}, {"eclipsegt", "Mitsubishi Eclipse GT"},
    {"elise", "Lotus Elise"}, {"fordgt", "Ford GT"}, {"gallardo", "Lamborghini Gallardo"},
    {"gti", "Volkswagen Golf GTI"}, {"gto", "Pontiac GTO"}, {"imprezawrx", "Subaru Impreza WRX STi"},
    {"is300", "Lexus IS 300"}, {"lancerevo8", "Mitsubishi Lancer Evolution VIII"}, {"monaro", "Vauxhall Monaro VXR"},
    {"murcielago", "Lamborghini Murcielago"}, {"mustanggt", "Ford Mustang GT"}, {"punto", "Fiat Punto"},
    {"rx7", "Mazda RX-7"}, {"rx8", "Mazda RX-8"}, {"sl500", "Mercedes-Benz SL 500"},
    {"sl65", "Mercedes-Benz SL 65 AMG"}, {"slr", "Mercedes-Benz SLR McLaren"}, {"supra", "Toyota Supra"},
    {"tt", "Audi TT 3.2 quattro"}, {"viper", "Dodge Viper SRT-10"},
};

std::string Cars_DisplayName(const std::string& model) {
    for (const auto& n : kNames)
        if (_stricmp(n.car, model.c_str()) == 0) return n.name;
    return model;
}

std::vector<std::string> Cars_PlayerModels() {
    ReadAll();
    std::vector<std::string> cars;
    for (const Stock& s : s_cars)
        if (s.usable) cars.push_back(s.name);
    std::sort(cars.begin(), cars.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(Cars_DisplayName(a).c_str(), Cars_DisplayName(b).c_str()) < 0;
    });
    return cars;
}

CarCharacter Cars_Character(const char* model, float k, bool realWeight) {
    ReadAll();
    CarCharacter c;
    const Stock* s = nullptr;
    for (const Stock& x : s_cars)
        if (model && _stricmp(x.name.c_str(), model) == 0) s = &x;
    if (!s) return c;
    c.known = true;
    c.mass = s->mass, c.gameWeightFront = s->weightFront, c.torqueSplit = s->torqueSplit, c.hp = s->hp;
    if (k > 0.0f) {
        c.spring = Spread(s->spring, s_median.spring, k, 0.6f, 1.6f);
        c.damper = sqrtf(c.spring);     // the damping ratio kept
        c.swaybar = Spread(s->swaybar, s_median.swaybar, k, 0.6f, 1.6f);
        c.grip = std::min(1.0f, Spread(s->grip, s_median.grip, k * 0.5f, 0.85f, 1.0f));
        c.throttle = s->torque > 0.0f ? Spread(Throttle(*s), s_median.throttle, k, 0.75f, 1.6f) : 1.0f;
        // RollBalance r moves the bars to the front: front share f(1 + r) / (f(1 + r) + (1 - f)(1 - r)).
        float f = s->swaybarFront, want = 0.5f + (f - 0.5f) * (1.0f + k);
        if (f > 0.0f && f < 1.0f) c.rollBalance = (want - f) / (f + want - 2.0f * f * want);
    }
    if (realWeight)
        for (const auto& w : kRealWeight)
            if (_stricmp(w.car, s->name.c_str()) == 0) {
                c.weightFront = w.front;
                // Rear grip / front grip: half of what load sensitivity takes from the heavier rear.
                float game = s->weightFront / 100.0f, real = w.front / 100.0f;
                if (real < game && game > 0.0f && game < 1.0f) {
                    float stagger = powf((1.0f - real) / real / ((1.0f - game) / game), 0.1f);
                    c.gripBalance = (1.0f - stagger) / (1.0f + stagger);   // front 1 + this, rear 1 - this
                }
            }
    return c;
}
