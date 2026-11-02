// Custom Plates -- license plates you design in the garage, as plugins\mt_plugin_CustomPlates.dll.
//
// The plate itself is a normal attachment (Attachment_SR_Number_Plate_01..08 in More Attachments),
// so it is placed, moved and removed with the game's own attachment tools. Those meshes are
// finished plates with their own artwork, so the plate gets a new face made of itself: the mesh
// painted in one colour is the border, and a copy of the mesh a little smaller is the blank, which
// keeps the rounded corners. The band is a third copy squeezed flat, one per face, because a
// single band would read left from one side and right from the other. Text sits on both faces,
// because the meshes are thin along Y and text only reads from its front. Bold is the same text
// four times, a hair apart:
// the engine font has no bold cut, and the game cooks no other font this can use.
//
// Tuning > License Plate, per vehicle: a template, nine characters, colours, band, text size,
// height and position. Settings: Binaries\Win64\plugins\custom_plates.ini, [Vehicle<id>].
//
// The design travels on the car. The game sends every vehicle's Net_Tunings to every player and
// saves them, and ServerSetTuning takes any target, so each field of the design is a wheel width
// tuning on a target no axle has (7000 up). Its value stays inside the game's 0.80 - 1.20 so a
// vanilla server keeps it: 39 steps, skipping 1.00, which the game drops as the default.
// Players with this plugin read the plate off the car; to anyone else those entries are nothing.

#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "../include/mt_plugin.hpp"

using namespace mt;

struct Ref { int index; uintptr_t ptr; };
static bool alive(const Ref& r) { return r.ptr && api->object(r.index) == r.ptr; }

struct FStr { const wchar_t* data; int num, max; };
static FStr str(const wchar_t* s) { int n = (int)wcslen(s) + 1; return {s, n, n}; }

// ---- the design ----
static const wchar_t ALPHABET[] = L" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
static const int LETTERS = 37;  // highest index
static const struct { const wchar_t* name; unsigned rgb; } PALETTE[] = {
    {L"White", 0xFFFFFF}, {L"Black", 0x0A0A0A}, {L"Yellow", 0xFFD200}, {L"Red", 0xC8102E},
    {L"Blue", 0x0B3D91}, {L"Green", 0x006B3F}, {L"Orange", 0xFF6A00}, {L"Silver", 0xB0B4BA},
    {L"Navy", 0x10203F}, {L"Gold", 0xC9A227}, {L"Pink", 0xFF4FA3}, {L"Purple", 0x5B2A86}};
static const int COLORS = 11;  // highest index

enum { R_TEMPLATE, R_CHAR, R_INK = R_CHAR + 9, R_PLATE, R_BORDER, R_BAND, R_WHERE, R_LAMP, R_LAMPCOLOR, R_SIZE, R_TALL, R_ACROSS, R_UP, NROWS };
enum { KIND_TEMPLATE, KIND_LETTER, KIND_COLOR, KIND_OPTIONAL, KIND_WHERE, KIND_LAMP, KIND_NUMBER };

static const wchar_t* const WHERE[] = {L"Left", L"Top", L"Bottom"};
static const wchar_t* const LAMP[] = {L"Off", L"Engine on", L"Always"};
// Border and band count 0 as None, 1.. as the palette.
static const struct { const wchar_t* name; int ink, plate, border, band, where; } TEMPLATES[] = {
    {L"European", 1, 0, 2, 5, 0}, {L"UK Rear", 1, 2, 0, 0, 0}, {L"American", 4, 0, 5, 4, 1}, {L"Sunshine", 8, 2, 9, 7, 2},
    {L"Japanese", 5, 0, 6, 0, 0}, {L"Racing", 0, 1, 4, 4, 2}, {L"Classic", 7, 1, 8, 0, 0}, {L"Prestige", 9, 1, 10, 0, 0}};
static const int TEMPLATE_TOP = 7;

static const struct { const wchar_t* label; const wchar_t* key; int top, kind; double zero, step; } ROWS[NROWS] = {
    {L"Template", L"Template", TEMPLATE_TOP, KIND_TEMPLATE},
    {L"Character 1", L"Char1", LETTERS, KIND_LETTER}, {L"Character 2", L"Char2", LETTERS, KIND_LETTER}, {L"Character 3", L"Char3", LETTERS, KIND_LETTER},
    {L"Character 4", L"Char4", LETTERS, KIND_LETTER}, {L"Character 5", L"Char5", LETTERS, KIND_LETTER}, {L"Character 6", L"Char6", LETTERS, KIND_LETTER},
    {L"Character 7", L"Char7", LETTERS, KIND_LETTER}, {L"Character 8", L"Char8", LETTERS, KIND_LETTER}, {L"Character 9", L"Char9", LETTERS, KIND_LETTER},
    {L"Text colour", L"Ink", COLORS, KIND_COLOR}, {L"Plate colour", L"Plate", COLORS, KIND_COLOR},
    {L"Border colour", L"Border", COLORS + 1, KIND_OPTIONAL}, {L"Band colour", L"Band", COLORS + 1, KIND_OPTIONAL},
    {L"Band position", L"BandAt", 2, KIND_WHERE},
    {L"Plate light", L"Lamp", 2, KIND_LAMP}, {L"Light colour", L"LampColour", COLORS, KIND_COLOR},
    {L"Text size", L"Size", 70, KIND_NUMBER, 3, 0.1}, {L"Text height", L"Tall", 100, KIND_NUMBER, 1, 0.02},
    {L"Text across", L"Across", 160, KIND_NUMBER, -8, 0.1}, {L"Text up", L"Up", 80, KIND_NUMBER, -4, 0.1}};

struct Look { int v[NROWS]; };
static Look base;
static std::map<int64_t, Look> looks;
static std::set<std::wstring> tuned;

static double number(const Look& k, int row) { return ROWS[row].zero + ROWS[row].step * k.v[row]; }
static std::wstring section(int64_t id) { return L"Vehicle" + std::to_wstring(id); }

static void applyTemplate(Look& k) {
    auto& t = TEMPLATES[k.v[R_TEMPLATE]];
    k.v[R_INK] = t.ink; k.v[R_PLATE] = t.plate; k.v[R_BORDER] = t.border; k.v[R_BAND] = t.band; k.v[R_WHERE] = t.where;
}

static Look& lookOf(int64_t id) {
    if (id <= 0) return base;
    auto known = looks.find(id);
    if (known != looks.end()) return known->second;
    Look k = base;
    if (tuned.count(section(id)))
        for (int i = 0; i < NROWS; i++) {
            std::wstring v = iniGet(L"custom_plates.ini", section(id).c_str(), ROWS[i].key);
            int n = v.empty() ? k.v[i] : _wtoi(v.c_str());
            k.v[i] = n < 0 ? 0 : n > ROWS[i].top ? ROWS[i].top : n;
        }
    return looks[id] = k;
}

static void saveLook(int64_t id, const Look& k) {
    tuned.insert(section(id));
    for (int i = 0; i < NROWS; i++) iniSet(L"custom_plates.ini", section(id).c_str(), ROWS[i].key, std::to_wstring(k.v[i]).c_str(), true);
}

// ---- the plate ----
static uintptr_t cActor, cWords, cMesh, cVeh, fAdd, fFinish, fMobility, fCollision, fShadow, fSetMesh, fMid, fMidColor;
static uintptr_t fToText, textCDO, fWords, fWorldSize, fInk, fAlignX, fAlignY, fTall, fRelLoc, fRelScale, fShow, fParent;
static uintptr_t fToName, stringCDO, fPath, fRef, fLoad, systemCDO, cubeMaterial;
static uintptr_t fSetTuning, fGetPC, fGetPawn, gameplayCDO, fGetMaterial, fGetComp, fBounds, fSetMaterial, fNumMaterials, cLamp, fPower, fLampColor, fLampReach, fLampShadows, fLampUnits;
static int oTunings = -1, tParam, tTarget, tValue, tSize, pSetVehicle, pSetParam, pSetTarget, pSetValue;
static uintptr_t editing;  // the car on the plate rows: its design is still being sent
static int oStaticMesh = -1, oEngine = -1, oHot = -1, oRpm = -1;
static uint64_t colorName;
static int oId = -1, oRelLoc;
static int pTextIn, pTextRet;
// The eight meshes run from 29 x 13 to 46 x 14 cm, 1.8 to 2.6 thick, so everything is measured off
// the one fitted: GetLocalBounds, not numbers from one plate. All of them are centred on the actor.
static const double BORDER = 0.75, BAND_WIDE = 3, BAND_HIGH = 2.4, PROUD = 0.06, REFERENCE_HALF_HEIGHT = 6.56;  // cm

struct Box { Ref c, mid; };
struct Plate { Ref actor; Box border, blank, band[2]; Ref text[2][4], lamp[2]; std::wstring shown; double middle[3], half[3]; int lit; };
static std::map<uintptr_t, Plate> plates;
static std::vector<Ref> waiting, screens;
static std::mutex lock;
static LONG queued;

static uintptr_t loadAsset(const wchar_t* path) {
    unsigned char a[0x80] = {0}, b[0x80] = {0}, c[0x80] = {0};
    FStr s = str(path);
    int in = off(fRef, "SoftObjectPath"), load = off(fLoad, "Asset"), got = off(fLoad, "ReturnValue");
    memcpy(a + off(fPath, "PathString"), &s, sizeof s);
    api->process_event(systemCDO, fPath, a);
    memcpy(b + in, a + off(fPath, "ReturnValue"), off(fRef, "ReturnValue") - in);
    api->process_event(systemCDO, fRef, b);
    memcpy(c + load, b + off(fRef, "ReturnValue"), got - load);
    api->process_event(systemCDO, fLoad, c);
    uintptr_t out = 0;
    memcpy(&out, c + got, 8);
    return out;
}

static Ref refOf(uintptr_t o) { return {o ? get<int>(o + L().obj_index) : 0, o}; }
// In the object table but being destroyed, which alive() alone cannot see.
static bool usable(const Ref& r) { return alive(r) && !(get<uint32_t>(r.ptr + L().obj_flags) & 0x18000); }

static Box makeBox(uintptr_t actor, uintptr_t mesh) {
    Box box = {};
    alignas(16) unsigned char p[0x100] = {0};
    int at = off(fAdd, "RelativeTransform");
    double spin[4] = {0, 0, 0, 1}, scale[3] = {0.01, 0.01, 0.01};
    memcpy(p + off(fAdd, "Class"), &cMesh, 8);
    memcpy(p + at, spin, sizeof spin);
    memcpy(p + at + 0x40, scale, sizeof scale);
    p[off(fAdd, "bDeferredFinish")] = 1;
    api->process_event(actor, fAdd, p);
    uintptr_t c = 0;
    memcpy(&c, p + off(fAdd, "ReturnValue"), 8);
    if (!c) return box;
    uint8_t movable = 2, none = 0;
    api->process_event(c, fMobility, &movable);
    api->process_event(c, fCollision, &none);
    api->process_event(c, fShadow, &none);
    unsigned char m[0x10] = {0};
    memcpy(m, &mesh, 8);
    api->process_event(c, fSetMesh, m);
    alignas(16) unsigned char f[0x100] = {0};
    memcpy(f + off(fFinish, "Component"), &c, 8);
    memcpy(f + off(fFinish, "RelativeTransform"), p + at, 0x60);
    api->process_event(actor, fFinish, f);
    box.c = refOf(c);
    if (!cubeMaterial) return box;
    unsigned char d[0x30] = {0};
    memcpy(d + off(fMid, "SourceMaterial"), &cubeMaterial, 8);
    api->process_event(c, fMid, d);
    uintptr_t mid = 0;
    memcpy(&mid, d + off(fMid, "ReturnValue"), 8);
    box.mid = refOf(mid);
    return box;
}

static void place(const Ref& c, double x, double y, double z) {
    unsigned char p[0x120] = {0};
    double at[3] = {x, y, z};
    memcpy(p + oRelLoc, at, sizeof at);
    api->process_event(c.ptr, fRelLoc, p);
}

static void paint(const Box& b, unsigned rgb) {
    if (!usable(b.mid)) return;
    struct { uint64_t name; float rgba[4]; } color = {colorName, {0, 0, 0, 1}};
    for (int i = 0; i < 3; i++) color.rgba[i] = powf(((rgb >> (16 - 8 * i)) & 255) / 255.0f, 2.2f);  // the palette is sRGB
    api->process_event(b.mid.ptr, fMidColor, &color);
}

// A copy of the plate's mesh at these shares of its size, centred at x, y, z off the plate's middle.
static void shape(const Plate& p, const Box& b, double x, double y, double z, double sx, double sy, double sz) {
    if (!alive(b.c)) return;
    double size[3] = {sx, sy, sz};
    api->process_event(b.c.ptr, fRelScale, size);
    place(b.c, x - p.middle[0] * sx, y - p.middle[1] * sy, z - p.middle[2] * sz);
}

static void show(const Ref& c, bool on) {
    if (!alive(c)) return;
    struct { bool visible, propagate; } p = {on, false};
    api->process_event(c.ptr, fShow, &p);
}

static Ref makeWords(uintptr_t actor, double yaw) {
    alignas(16) unsigned char p[0x100] = {0};
    int at = off(fAdd, "RelativeTransform");
    double half = yaw * 3.14159265358979 / 360;
    double spin[4] = {0, 0, sin(half), cos(half)}, scale[3] = {1, 1, 1};
    memcpy(p + off(fAdd, "Class"), &cWords, 8);
    memcpy(p + at, spin, sizeof spin);
    memcpy(p + at + 0x40, scale, sizeof scale);
    api->process_event(actor, fAdd, p);
    uintptr_t c = 0;
    memcpy(&c, p + off(fAdd, "ReturnValue"), 8);
    if (!c) return {};
    uint8_t centre = 1;  // EHTA_Center, EVRTA_TextCenter
    api->process_event(c, fAlignX, &centre);
    api->process_event(c, fAlignY, &centre);
    return refOf(c);
}

static Ref makeLamp(uintptr_t actor, double y, double z) {
    alignas(16) unsigned char p[0x100] = {0};
    int at = off(fAdd, "RelativeTransform");
    double spin[4] = {0, 0, 0, 1}, where[3] = {0, y, z}, scale[3] = {1, 1, 1};
    memcpy(p + off(fAdd, "Class"), &cLamp, 8);
    memcpy(p + at, spin, sizeof spin);
    memcpy(p + at + 0x20, where, sizeof where);
    memcpy(p + at + 0x40, scale, sizeof scale);
    p[off(fAdd, "bDeferredFinish")] = 1;
    api->process_event(actor, fAdd, p);
    uintptr_t c = 0;
    memcpy(&c, p + off(fAdd, "ReturnValue"), 8);
    if (!c) return {};
    uint8_t movable = 2, lumens = 2;
    unsigned char shadows[8] = {0};
    float reach = 45, power = 25;
    api->process_event(c, fMobility, &movable);
    api->process_event(c, fLampShadows, shadows);
    api->process_event(c, fLampUnits, &lumens);
    api->process_event(c, fLampReach, &reach);
    api->process_event(c, fPower, &power);
    alignas(16) unsigned char f[0x100] = {0};
    memcpy(f + off(fFinish, "Component"), &c, 8);
    memcpy(f + off(fFinish, "RelativeTransform"), p + at, 0x60);
    api->process_event(actor, fFinish, f);
    return refOf(c);
}

// The plate's own mesh, in one colour, is the border. The game can put the mesh's material back
// whenever it likes, and then nothing references ours and it is collected: a freed material
// painted through is a crash, so this checks every pass and takes the mesh back.
static bool ownBorder(Plate& p) {
    if (!alive(p.border.c)) return false;
    uintptr_t c = p.border.c.ptr;
    unsigned char g[0x20] = {0};
    api->process_event(c, fGetMaterial, g);
    uintptr_t on = 0;
    memcpy(&on, g + off(fGetMaterial, "ReturnValue"), 8);
    if (usable(p.border.mid) && on == p.border.mid.ptr) return false;
    if (!usable(p.border.mid)) {
        unsigned char d[0x30] = {0};
        memcpy(d + off(fMid, "SourceMaterial"), &cubeMaterial, 8);
        api->process_event(c, fMid, d);
        uintptr_t mid = 0;
        memcpy(&mid, d + off(fMid, "ReturnValue"), 8);
        p.border.mid = refOf(mid);
        if (!mid) return false;
    }
    struct { int ret; } slots = {0};
    api->process_event(c, fNumMaterials, &slots);
    for (int i = 0; i < slots.ret; i++) {
        unsigned char q[0x20] = {0};
        memcpy(q + off(fSetMaterial, "ElementIndex"), &i, 4);
        memcpy(q + off(fSetMaterial, "Material"), &p.border.mid.ptr, 8);
        api->process_event(c, fSetMaterial, q);
    }
    return true;
}

static void dress(Plate& p) {
    struct { uintptr_t cls, ret; } own = {cMesh, 0};
    api->process_event(p.actor.ptr, fGetComp, &own);
    uintptr_t mesh = own.ret ? get<uintptr_t>(own.ret + oStaticMesh) : 0;
    if (!mesh) return;  // spawned a moment ago: the next pass tries again
    // Asked for each plate: with no plate fitted nothing holds it, and a kept pointer would go stale.
    cubeMaterial = loadAsset(L"/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
    if (!cubeMaterial) return;
    double low[3], high[3];
    unsigned char b[0x60] = {0};
    api->process_event(own.ret, fBounds, b);
    memcpy(low, b + off(fBounds, "Min"), sizeof low);
    memcpy(high, b + off(fBounds, "Max"), sizeof high);
    for (int i = 0; i < 3; i++) { p.middle[i] = (low[i] + high[i]) / 2; p.half[i] = (high[i] - low[i]) / 2; }
    p.border.c = refOf(own.ret);
    p.blank = makeBox(p.actor.ptr, mesh);
    p.band[0] = makeBox(p.actor.ptr, mesh);
    p.band[1] = makeBox(p.actor.ptr, mesh);
    for (int side = 0; side < 2; side++) {
        for (int i = 0; i < 4; i++) p.text[side][i] = makeWords(p.actor.ptr, side ? -90 : 90);
        p.lamp[side] = makeLamp(p.actor.ptr, (side ? -1 : 1) * (p.half[1] + 2.5), p.half[2] + 1.5);
        show(p.lamp[side], false);
    }
    p.lit = -1;
}

static uintptr_t carOf(uintptr_t actor) {
    struct { uintptr_t ret; } parent = {0};
    api->process_event(actor, fParent, &parent);
    return parent.ret && api->is_a(classOf(parent.ret), cVeh) ? parent.ret : 0;
}

static void light(Plate& p, const Look& k, uintptr_t car) {
    uintptr_t engine = car && oEngine >= 0 ? get<uintptr_t>(car + oEngine) : 0;
    bool running = engine && oHot >= 0 && get<float>(engine + oHot + oRpm) > 0;
    int want = k.v[R_LAMP] == 2 || (k.v[R_LAMP] == 1 && running) ? 1 + k.v[R_LAMPCOLOR] : 0;
    if (want == p.lit) return;
    p.lit = want;
    for (const Ref& c : p.lamp) {
        show(c, want != 0);
        if (!want || !alive(c)) continue;
        unsigned rgb = PALETTE[want - 1].rgb;
        struct { float rgba[4]; bool srgb; } tint = {{((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1}, true};
        api->process_event(c.ptr, fLampColor, &tint);
    }
}

static void wear(Plate& p, const Look& k) {
    if (ownBorder(p)) p.shown.clear();  // a new material has no colour yet
    std::wstring seen;
    for (int i = 0; i < NROWS; i++) if (i != R_LAMP && i != R_LAMPCOLOR) seen += std::to_wstring(k.v[i]) + L",";
    if (seen == p.shown) return;
    p.shown = seen;
    int border = k.v[R_BORDER], band = k.v[R_BAND];
    unsigned back = PALETTE[k.v[R_PLATE]].rgb;
    paint(p.border, border ? PALETTE[border - 1].rgb : back);
    paint(p.blank, back);
    double hx = p.half[0], hy = p.half[1], hz = p.half[2];
    if (hx <= 0 || hz <= 0) return;  // not dressed
    // The same border all round: a share of each side would be thick on the long plates.
    double wide = 1 - BORDER / hx, high = 1 - BORDER / hz;
    shape(p, p.blank, 0, 0, 0, wide, 1.1, high);
    double top = hy * 1.1;  // the blank's face
    for (int side = 0; side < 2; side++) {
        const Box& b = p.band[side];
        show(b.c, band != 0);
        if (!band) continue;
        paint(b, PALETTE[band - 1].rgb);
        double y = (side ? -1 : 1) * (top + PROUD - hy * 0.3);
        // Flush with the blank's own edge. Left is -X from the +Y face and +X from the other.
        double x = hx - BORDER - BAND_WIDE / 2, z = hz - BORDER - BAND_HIGH / 2;
        if (k.v[R_WHERE] == 0) shape(p, b, side ? x : -x, y, 0, BAND_WIDE / 2 / hx, 0.3, high);
        else shape(p, b, 0, y, k.v[R_WHERE] == 1 ? z : -z, wide, 0.3, BAND_HIGH / 2 / hz);
    }
    std::wstring words;
    for (int i = 0; i < 9; i++) words += ALPHABET[k.v[R_CHAR + i]];
    while (!words.empty() && words.back() == L' ') words.pop_back();
    unsigned char t[0x60] = {0};
    FStr s = str(words.empty() ? L" " : words.c_str());
    memcpy(t + pTextIn, &s, sizeof s);
    api->process_event(textCDO, fToText, t);
    static const double NUDGE[4][2] = {{-0.05, -0.05}, {0.05, -0.05}, {-0.05, 0.05}, {0.05, 0.05}};
    float size = (float)(number(k, R_SIZE) * hz / REFERENCE_HALF_HEIGHT), tall = (float)number(k, R_TALL);
    double face = top + PROUD + 0.1;
    unsigned ink = PALETTE[k.v[R_INK]].rgb;
    struct { uint8_t b, g, r, a; } color = {(uint8_t)ink, (uint8_t)(ink >> 8), (uint8_t)(ink >> 16), 255};
    double across = number(k, R_ACROSS), up = number(k, R_UP);
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < 4; i++) {
            const Ref& c = p.text[side][i];
            if (!alive(c)) continue;
            unsigned char w[0x40] = {0};
            memcpy(w, t + pTextRet, 0x18);
            api->process_event(c.ptr, fWords, w);
            api->process_event(c.ptr, fWorldSize, &size);
            api->process_event(c.ptr, fTall, &tall);
            api->process_event(c.ptr, fInk, &color);
            place(c, (side ? -across : across) + NUDGE[i][0], side ? -face : face, up + NUDGE[i][1]);
        }
}

// ---- the design on the car ----
static const int WIDTH_PARAM = 1, FIRST_TARGET = 7000, SENDS_PER_PASS = 6;

static float valueOf(int digit) { return (digit < 20 ? 80 + digit : 81 + digit) / 100.0f; }
static int digitOf(float value) {
    int n = (int)floor((value - 0.8f) * 100 + 0.5f);
    return n < 0 || n > 39 || n == 20 ? -1 : n < 20 ? n : n - 1;
}

static bool net(uintptr_t car, int target, int& digit) {
    uintptr_t d = get<uintptr_t>(car + oTunings);
    int n = get<int>(car + oTunings + 8);
    for (int i = 0; d && i < n && i < 256; i++) {
        uintptr_t e = d + (uintptr_t)i * tSize;
        if (get<uint8_t>(e + tParam) != WIDTH_PARAM || get<int>(e + tTarget) != target) continue;
        digit = digitOf(get<float>(e + tValue));
        return digit >= 0;
    }
    return false;
}

// Rows from R_CHAR on, one digit each, or two for the rows with more steps than a digit holds.
template <class Each> static void fields(Each each) {
    int target = FIRST_TARGET;
    for (int r = R_CHAR; r < NROWS; r++) {
        each(r, target, ROWS[r].top < 39 ? 1 : 2);
        target += ROWS[r].top < 39 ? 1 : 2;
    }
}

static bool readCar(uintptr_t car, Look& out) {
    if (oTunings < 0) return false;
    bool whole = true;
    out = base;
    fields([&](int r, int target, int digits) {
        int a = 0, b = 0;
        whole = whole && net(car, target, a) && (digits == 1 || net(car, target + 1, b));
        int v = digits == 1 ? a : a * 39 + b;
        if (whole && v <= ROWS[r].top) out.v[r] = v;
        else whole = false;
    });
    return whole;
}

static void sendCar(uintptr_t car, const Look& k) {
    struct { uintptr_t context; int index; uintptr_t ret; } pc = {car, 0, 0};
    api->process_event(gameplayCDO, fGetPC, &pc);
    if (!pc.ret) return;
    int left = SENDS_PER_PASS;
    auto send = [&](int target, int digit) {
        int have = -1;
        if (left <= 0 || (net(car, target, have) && have == digit)) return;
        left--;
        unsigned char q[0x20] = {0};
        float value = valueOf(digit);
        memcpy(q + pSetVehicle, &car, 8);
        q[pSetParam] = WIDTH_PARAM;
        memcpy(q + pSetTarget, &target, 4);
        memcpy(q + pSetValue, &value, 4);
        api->process_event(pc.ret, fSetTuning, q);
        static int said;
        if (said < 40) { said++; MT_LOG("sent target %d value %.2f, car had %d tunings", target, value, get<int>(car + oTunings + 8)); }
    };
    fields([&](int r, int target, int digits) {
        int v = k.v[r];
        if (digits == 1) send(target, v);
        else { send(target, v / 39); send(target + 1, v % 39); }
    });
}

// ---- the Tuning screen: the same rows ECU Tuning uses ----
struct Row { uintptr_t widget, spin, value; };
struct Panel {
    Ref screen;
    uintptr_t button, inner;
    bool buttonHeld, open;
    int labelFrames;
    Row rows[NROWS];
    std::vector<uintptr_t> hidden;
    int64_t id;
    DWORD dirty;
};
static Panel panel;
static uintptr_t cSetting, cButton, cButtonBase, cTextBlock, widgetCDO;
static uintptr_t fCreate, fAddChild, fChildIndex, fChildAt, fChildCount, fSetText, fSetRich, fPressed, fRemove, fSetVis, fIsVisible, fPadding;
static int oVehicle, oBox, oRowName, oRowSpin, oSpinText, oSpinValue, oBtnName, oBtnRich, oBtnText, oBtnInner;
static void (__fastcall* spinRange)(uintptr_t self, float min, float max, float step, bool snap);
static void (__fastcall* spinSet)(uintptr_t self, float value);
static const int LABEL_FRAMES = 60;

static bool isA(uintptr_t o, uintptr_t cls) { return o && api->is_a(classOf(o), cls); }

static bool flag(uintptr_t obj, uintptr_t fn) {
    unsigned char p[0x10] = {0};
    api->process_event(obj, fn, p);
    return p[0] != 0;
}

static void setText(uintptr_t block, const wchar_t* text) {
    unsigned char p[0x40] = {0};
    FStr s = str(text);
    memcpy(p + pTextIn, &s, sizeof s);
    api->process_event(textCDO, fToText, p);
    api->process_event(block, isA(block, cTextBlock) ? fSetText : fSetRich, p + pTextRet);
}

static uintptr_t create(uintptr_t screen, uintptr_t cls, uintptr_t box) {
    struct { uintptr_t context, cls, player, ret; } c = {screen, cls, 0, 0};
    api->process_event(widgetCDO, fCreate, &c);
    if (!c.ret) return 0;
    struct { uintptr_t content, slot; } add = {c.ret, 0};
    api->process_event(box, fAddChild, &add);
    float pad[4] = {5, 5, 5, 5};
    if (add.slot) api->process_event(add.slot, fPadding, pad);
    return c.ret;
}

static int childIndex(uintptr_t box, uintptr_t child) {
    struct { uintptr_t content; int ret; } at = {child, -1};
    if (child) api->process_event(box, fChildIndex, &at);
    return at.ret;
}

static void setVisible(uintptr_t widget, bool visible) {
    uint8_t v = visible ? 0 : 1;
    api->process_event(widget, fSetVis, &v);
}

static bool clicked(uintptr_t button, bool& held) {
    bool now = flag(button, fPressed);
    bool up = held && !now;
    held = now;
    return up;
}

// The spin box scales its step by the value's magnitude, so it holds a step count in 1.00 .. 9.99.
static float toSpin(int v) { return (float)(1 + 0.01 * v); }
static int fromSpin(float v) { return (int)floor((v - 1) / 0.01 + 0.5); }

static void showRow(int i, const Look& k) {
    wchar_t text[32];
    int v = k.v[i];
    switch (ROWS[i].kind) {
        case KIND_TEMPLATE: swprintf_s(text, L"%s", TEMPLATES[v].name); break;
        case KIND_LETTER: if (v) swprintf_s(text, L"%c", ALPHABET[v]); else swprintf_s(text, L"Blank"); break;
        case KIND_COLOR: swprintf_s(text, L"%s", PALETTE[v].name); break;
        case KIND_OPTIONAL: swprintf_s(text, L"%s", v ? PALETTE[v - 1].name : L"None"); break;
        case KIND_WHERE: swprintf_s(text, L"%s", WHERE[v]); break;
        case KIND_LAMP: swprintf_s(text, L"%s", LAMP[v]); break;
        default: swprintf_s(text, L"%.1f", number(k, i));
    }
    setText(panel.rows[i].value, text);
}

static void openRows(uintptr_t screen, uintptr_t box, const Look& k) {
    struct { int ret; } count = {0};
    api->process_event(box, fChildCount, &count);
    panel.hidden.clear();
    for (int i = 0; i < count.ret; i++) {
        struct { int index; uintptr_t ret; } at = {i, 0};
        api->process_event(box, fChildAt, &at);
        if (at.ret) { setVisible(at.ret, false); panel.hidden.push_back(at.ret); }
    }
    for (int i = 0; i < NROWS; i++) {
        uintptr_t w = create(screen, cSetting, box);
        uintptr_t spin = w ? get<uintptr_t>(w + oRowSpin) : 0;
        if (!spin) { MT_LOG("could not create the plate rows"); return; }
        panel.rows[i] = {w, spin, get<uintptr_t>(spin + oSpinText)};
        setText(get<uintptr_t>(w + oRowName), ROWS[i].label);
        if (spinRange) {
            spinRange(spin, 1, toSpin(ROWS[i].top), 0.01f, true);
            spinSet(spin, toSpin(k.v[i]));
        }
        showRow(i, k);
    }
    panel.open = true;
}

static void closeRows() {
    for (auto& r : panel.rows)
        if (r.widget) api->process_event(r.widget, fRemove, nullptr);
    for (uintptr_t w : panel.hidden) setVisible(w, true);
    panel.hidden.clear();
    memset(panel.rows, 0, sizeof panel.rows);
    panel.open = false;
}

static void tickPanel(DWORD now) {
    if (panel.dirty && now - panel.dirty > 800) { saveLook(panel.id, lookOf(panel.id)); panel.dirty = 0; }
    uintptr_t screen = 0, box = 0;
    for (auto& r : screens) {
        if (!alive(r)) continue;
        uintptr_t b = get<uintptr_t>(r.ptr + oBox);
        if (b && flag(r.ptr, fIsVisible)) { screen = r.ptr; box = b; break; }
    }
    if (!screen) {
        if (panel.open && alive(panel.screen)) closeRows();
        editing = 0;
        return;
    }
    uintptr_t vehicle = get<uintptr_t>(screen + oVehicle);
    int64_t id = vehicle && oId >= 0 ? get<long long>(vehicle + oId) : 0;
    if (id <= 0) return;
    if (panel.screen.ptr != screen) {
        if (panel.dirty) saveLook(panel.id, lookOf(panel.id));
        panel = {};
        panel.screen = refOf(screen);
    }
    Look& k = lookOf(id);
    if (panel.open) editing = vehicle;
    if (panel.open) {
        if (childIndex(box, panel.rows[0].widget) < 0) {  // the game rebuilt the list and our rows with it
            panel.open = false; panel.button = 0; panel.hidden.clear();
            memset(panel.rows, 0, sizeof panel.rows);
            return;
        }
        if (panel.id != id) {
            if (panel.dirty) { saveLook(panel.id, lookOf(panel.id)); panel.dirty = 0; }
            panel.id = id;
            for (int i = 0; i < NROWS; i++) {
                if (spinSet) spinSet(panel.rows[i].spin, toSpin(k.v[i]));
                showRow(i, k);
            }
            return;
        }
        for (int i = 0; spinRange && i < NROWS; i++) {
            int v = fromSpin(get<float>(panel.rows[i].spin + oSpinValue));
            // Held at the end of its range the spin box keeps writing its own number over the row.
            if (v == k.v[i]) { showRow(i, k); continue; }
            k.v[i] = v < 0 ? 0 : v > ROWS[i].top ? ROWS[i].top : v;
            panel.dirty = now;
            showRow(i, k);
            if (i != R_TEMPLATE) continue;
            applyTemplate(k);  // the template picks the colours, which can then be changed
            for (int r = R_INK; r <= R_WHERE; r++) { spinSet(panel.rows[r].spin, toSpin(k.v[r])); showRow(r, k); }
        }
        return;
    }
    if (childIndex(box, panel.button) >= 0) {
        if (panel.labelFrames > 0) {
            panel.labelFrames--;
            for (int at : {oBtnName, oBtnRich}) {
                uintptr_t block = get<uintptr_t>(panel.button + at);
                if (block) setText(block, L"License Plate");
            }
        }
        if (clicked(panel.inner, panel.buttonHeld)) { panel.id = id; openRows(screen, box, k); }
        return;
    }
    struct { int index; uintptr_t ret; } first = {0, 0};
    api->process_event(box, fChildAt, &first);
    if (!isA(first.ret, cButtonBase) || !cButton) { panel.button = 0; return; }
    panel.button = create(screen, cButton, box);
    if (!panel.button) return;
    unsigned char p[0x40] = {0};
    FStr s = str(L"License Plate");
    memcpy(p + pTextIn, &s, sizeof s);
    api->process_event(textCDO, fToText, p);
    wr(panel.button + oBtnText, p + pTextRet, 16);  // moved in raw: nothing destroys the buffer
    panel.labelFrames = LABEL_FRAMES;
    panel.inner = get<uintptr_t>(panel.button + oBtnInner);
    panel.buttonHeld = false;
}

static void tick(void*) {
    InterlockedExchange(&queued, 0);
    std::lock_guard<std::mutex> g(lock);
    if (!colorName) {
        unsigned char n[0x40] = {0};
        FStr name = str(L"Color");
        memcpy(n + off(fToName, "InString"), &name, sizeof name);
        api->process_event(stringCDO, fToName, n);
        memcpy(&colorName, n + off(fToName, "ReturnValue"), 8);
    }
    for (auto& r : waiting) {
        if (!alive(r)) continue;
        Plate& p = plates[r.ptr];
        p.actor = r;
        dress(p);
        MT_LOG("%s: plate built", objName(classOf(r.ptr)).c_str());
    }
    waiting.clear();
    DWORD now = GetTickCount();
    if (spinRange || !screens.empty()) tickPanel(now);
    static DWORD wearAt, sendAt;
    if ((int)(now - wearAt) < 0) return;
    wearAt = now + 100;
    uintptr_t mine = editing;
    static uintptr_t announced;
    if (!mine) {
        for (auto& entry : plates) {
            uintptr_t car = alive(entry.second.actor) ? carOf(entry.first) : 0;
            if (!car) continue;
            struct { uintptr_t context; int index; uintptr_t ret; } who = {car, 0, 0};
            api->process_event(gameplayCDO, fGetPawn, &who);
            if (who.ret == car) { mine = car; break; }
        }
    }
    if (mine != announced) {
        announced = mine;
        if (mine) MT_LOG("my plate car: %s #%lld", objName(classOf(mine)).c_str(), oId >= 0 ? get<long long>(mine + oId) : 0);
    }
    if (mine && oTunings >= 0 && oId >= 0 && get<long long>(mine + oId) > 0 && (int)(now - sendAt) >= 0) {
        sendAt = now + 300;
        sendCar(mine, lookOf(get<long long>(mine + oId)));
    }
    for (auto it = plates.begin(); it != plates.end();) {
        if (!alive(it->second.actor)) { it = plates.erase(it); continue; }
        uintptr_t car = carOf(it->first);
        int64_t id = car && oId >= 0 ? get<long long>(car + oId) : 0;
        Look carried;
        // The plate on the rows is mid-send. Any other car shows what it carries, or else a saved look.
        const Look& k = car && car != mine && readCar(car, carried) ? carried : lookOf(id);
        if (it->second.half[0] <= 0) dress(it->second);
        wear(it->second, k);
        light(it->second, k, car);
        ++it;
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Custom Plates loaded");
    {
        static wchar_t names[65536];
        GetPrivateProfileSectionNamesW(names, 65536, iniPath(L"custom_plates.ini").c_str());
        for (const wchar_t* n = names; *n; n += wcslen(n) + 1) tuned.insert(n);
    }
    iniSet(L"custom_plates.ini", L"Plate", L"Text", L"QX 123");
    std::wstring start = iniGet(L"custom_plates.ini", L"Plate", L"Text");
    for (int i = 0; i < NROWS; i++) base.v[i] = 0;
    for (int i = 0; i < 9 && i < (int)start.size(); i++) {
        const wchar_t* at = wcschr(ALPHABET, towupper(start[i]));
        base.v[R_CHAR + i] = at ? (int)(at - ALPHABET) : 0;
    }
    applyTemplate(base);
    base.v[R_SIZE] = 30; base.v[R_TALL] = 30; base.v[R_ACROSS] = 80; base.v[R_UP] = 40;  // 6 cm, 1.6 tall, centred
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }

    cActor = api->find_object("Class", "Actor", "/Script/Engine");
    cWords = api->find_object("Class", "TextRenderComponent", "/Script/Engine");
    cMesh = api->find_object("Class", "StaticMeshComponent", "/Script/Engine");
    cVeh = api->find_object("Class", "MTVehicle", "/Script/MotorTown");
    fAdd = api->find_object("Function", "AddComponentByClass", "Actor");
    fFinish = api->find_object("Function", "FinishAddComponent", "Actor");
    fParent = api->find_object("Function", "GetAttachParentActor", "Actor");
    fMobility = api->find_object("Function", "SetMobility", "SceneComponent");
    fRelLoc = api->find_object("Function", "K2_SetRelativeLocation", "SceneComponent");
    fRelScale = api->find_object("Function", "SetRelativeScale3D", "SceneComponent");
    fShow = api->find_object("Function", "SetVisibility", "SceneComponent");
    fCollision = api->find_object("Function", "SetCollisionEnabled", "PrimitiveComponent");
    fShadow = api->find_object("Function", "SetCastShadow", "PrimitiveComponent");
    fMid = api->find_object("Function", "CreateDynamicMaterialInstance", "PrimitiveComponent");
    fSetMesh = api->find_object("Function", "SetStaticMesh", "StaticMeshComponent");
    fMidColor = api->find_object("Function", "SetVectorParameterValue", "MaterialInstanceDynamic");
    fToText = api->find_object("Function", "Conv_StringToText", "KismetTextLibrary");
    textCDO = api->find_object("KismetTextLibrary", "Default__KismetTextLibrary", "/Script/Engine");
    fWords = api->find_object("Function", "K2_SetText", "TextRenderComponent");
    fWorldSize = api->find_object("Function", "SetWorldSize", "TextRenderComponent");
    fInk = api->find_object("Function", "SetTextRenderColor", "TextRenderComponent");
    fAlignX = api->find_object("Function", "SetHorizontalAlignment", "TextRenderComponent");
    fAlignY = api->find_object("Function", "SetVerticalAlignment", "TextRenderComponent");
    fTall = api->find_object("Function", "SetYScale", "TextRenderComponent");
    fToName = api->find_object("Function", "Conv_StringToName", "KismetStringLibrary");
    stringCDO = api->find_object("KismetStringLibrary", "Default__KismetStringLibrary", "/Script/Engine");
    systemCDO = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    fPath = api->find_object("Function", "MakeSoftObjectPath", "KismetSystemLibrary");
    fRef = api->find_object("Function", "Conv_SoftObjPathToSoftObjRef", "KismetSystemLibrary");
    fLoad = api->find_object("Function", "LoadAsset_Blocking", "KismetSystemLibrary");
    auto either = [](const char* name, const char* a, const char* b) {
        uintptr_t f = api->find_object("Function", name, a);
        return f ? f : api->find_object("Function", name, b);
    };
    fGetComp = api->find_object("Function", "GetComponentByClass", "Actor");
    fBounds = api->find_object("Function", "GetLocalBounds", "StaticMeshComponent");
    fSetMaterial = api->find_object("Function", "SetMaterial", "PrimitiveComponent");
    fGetMaterial = api->find_object("Function", "GetMaterial", "PrimitiveComponent");
    fNumMaterials = api->find_object("Function", "GetNumMaterials", "PrimitiveComponent");
    cLamp = api->find_object("Class", "PointLightComponent", "/Script/Engine");
    fPower = either("SetIntensity", "LightComponent", "LocalLightComponent");
    fLampColor = either("SetLightColor", "LightComponent", "LightComponentBase");
    fLampReach = either("SetAttenuationRadius", "LocalLightComponent", "PointLightComponent");
    fLampShadows = either("SetCastShadows", "LightComponentBase", "LightComponent");
    fLampUnits = either("SetIntensityUnits", "LocalLightComponent", "PointLightComponent");
    uintptr_t need[] = {fGetMaterial, fGetComp, fBounds, fSetMaterial, fNumMaterials, cLamp, fPower, fLampColor, fLampReach, fLampShadows, fLampUnits, cActor, cWords, cMesh, cVeh, fAdd, fFinish, fParent, fMobility, fRelLoc, fRelScale, fShow, fCollision, fShadow, fMid, fSetMesh,
                        fMidColor, fToText, textCDO, fWords, fWorldSize, fInk, fAlignX, fAlignY, fTall, fToName, stringCDO, systemCDO, fPath, fRef, fLoad};
    for (uintptr_t f : need)
        if (!f) { MT_LOG("FAILED: engine functions not found -- plates stay plain"); return 0; }
    oId = off(cVeh, "Net_VehicleId");
    oRelLoc = off(fRelLoc, "NewLocation");
    uintptr_t cPC = api->find_object("Class", "MotorTownPlayerController", "/Script/MotorTown");
    uintptr_t sEntry = api->find_object("ScriptStruct", "MTTuningEntry", "/Script/MotorTown");
    gameplayCDO = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
    fGetPC = api->find_object("Function", "GetPlayerController", "GameplayStatics");
    fGetPawn = api->find_object("Function", "GetPlayerPawn", "GameplayStatics");
    fSetTuning = api->find_object("Function", "ServerSetTuning", "MotorTownPlayerController");
    oTunings = off(cVeh, "Net_Tunings");
    if (sEntry) { tParam = off(sEntry, "Param"); tTarget = off(sEntry, "TargetId"); tValue = off(sEntry, "Value"); tSize = get<int>(sEntry + L().struct_child_props + 8); }
    if (fSetTuning) { pSetVehicle = off(fSetTuning, "Vehicle"); pSetParam = off(fSetTuning, "Param"); pSetTarget = off(fSetTuning, "TargetId"); pSetValue = off(fSetTuning, "Value"); }
    if (!cPC || !sEntry || !gameplayCDO || !fGetPC || !fGetPawn || !fSetTuning || oTunings < 0 || tSize <= 0 || pSetValue < 0) {
        MT_LOG("tuning calls not found -- plates stay on this machine");
        oTunings = -1;
    }
    oStaticMesh = off(cMesh, "StaticMesh");
    oEngine = off(cVeh, "EngineComponent");
    uintptr_t cEngine = api->find_object("Class", "MTEngineComponent", "/Script/MotorTown");
    uintptr_t sHot = api->find_object("ScriptStruct", "MTEngineHotState", "/Script/MotorTown");
    if (cEngine && sHot) { oHot = off(cEngine, "NetLC_EngineHotState"); oRpm = off(sHot, "CurrentRPM"); }
    pTextIn = off(fToText, "InString"); pTextRet = off(fToText, "ReturnValue");

    uintptr_t cScreen = api->find_object("Class", "GarageTuningWidget", "/Script/MotorTown");
    uintptr_t cChild = api->find_object("Class", "GarageChildWidget", "/Script/MotorTown");
    uintptr_t cRowBase = api->find_object("Class", "SettingWidget", "/Script/MotorTown");
    uintptr_t cSpinRow = api->find_object("Class", "SpinBoxString", "/Script/MotorTown");
    cButtonBase = api->find_object("Class", "MTButtonWidget", "/Script/MotorTown");
    cTextBlock = api->find_object("Class", "TextBlock", "/Script/UMG");
    widgetCDO = api->find_object("WidgetBlueprintLibrary", "Default__WidgetBlueprintLibrary", "/Script/UMG");
    fCreate = api->find_object("Function", "Create", "WidgetBlueprintLibrary");
    fAddChild = api->find_object("Function", "AddChild", "PanelWidget");
    fChildIndex = api->find_object("Function", "GetChildIndex", "PanelWidget");
    fChildAt = api->find_object("Function", "GetChildAt", "PanelWidget");
    fChildCount = api->find_object("Function", "GetChildrenCount", "PanelWidget");
    fSetText = api->find_object("Function", "SetText", "TextBlock");
    fSetRich = api->find_object("Function", "SetText", "RichTextBlock");
    fPressed = api->find_object("Function", "IsPressed", "Button");
    fRemove = api->find_object("Function", "RemoveFromParent", "Widget");
    fSetVis = api->find_object("Function", "SetVisibility", "Widget");
    fIsVisible = api->find_object("Function", "IsVisible", "Widget");
    fPadding = api->find_object("Function", "SetPadding", "VerticalBoxSlot");
    bool canTune = cScreen && cChild && cRowBase && cSpinRow && cButtonBase && cTextBlock && widgetCDO && fCreate && fAddChild && fChildIndex &&
                   fChildAt && fChildCount && fSetText && fSetRich && fPressed && fRemove && fSetVis && fIsVisible && fPadding;
    if (canTune) {
        oVehicle = off(cChild, "Vehicle"); oBox = off(cScreen, "TuningVerticalBox");
        oRowName = off(cRowBase, "NameTextBlock"); oRowSpin = off(cRowBase, "SpinBoxString");
        oSpinText = off(cSpinRow, "TextBlock");
        oBtnName = off(cButtonBase, "NameTextBlock"); oBtnRich = off(cButtonBase, "NameRichTextBlock");
        oBtnText = off(cButtonBase, "ButtonName"); oBtnInner = off(cButtonBase, "MenuButton");
        for (int o : {oVehicle, oBox, oRowName, oRowSpin, oSpinText, oBtnName, oBtnRich, oBtnText, oBtnInner}) canTune = canTune && o >= 0;
    }
    auto range = find_sig("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 44 24 50 48 8B D9 88 81 ?? ?? ?? ?? F3 0F 11 89");
    auto value = find_sig("40 53 48 83 EC 20 80 B9 ?? ?? ?? ?? 00 48 8B D9 F3 0F 10 81 ?? ?? ?? ?? F3 0F 5D C1 F3 0F 5F 81");
    if (range.size() == 1 && value.size() == 1) {
        spinRange = (decltype(spinRange))range[0];
        spinSet = (decltype(spinSet))value[0];
        oSpinValue = get<int>(range[0] + 57);  // the fourth store: value = min
    } else { MT_LOG("number rows not found -- no plate menu"); canTune = false; }

    std::unordered_map<uintptr_t, bool> plateClass;  // a name test per class, not per object
    for (int beat = 0;; beat++) {
        if (beat % 60 == 0) {  // about two seconds
            std::vector<Ref> fresh, w;
            for (int i = 0, n = api->num_objects(); i < n; i++) {
                uintptr_t o = api->object(i);
                if (!o || isCDO(o)) continue;
                uintptr_t k = classOf(o);
                if (canTune && api->is_a(k, cScreen)) { w.push_back({i, o}); continue; }
                auto known = plateClass.find(k);
                bool plate = known != plateClass.end() ? known->second : (plateClass[k] = objName(k).find("Number_Plate") != std::string::npos);
                if (plate && api->is_a(k, cActor)) fresh.push_back({i, o});
            }
            if (canTune && !cButton && !w.empty()) {  // the blueprints only exist once a garage has opened
                cSetting = api->find_object("WidgetBlueprintGeneratedClass", "W_Setting_C", "/Game/UI/Component/W_Setting");
                cButton = api->find_object("WidgetBlueprintGeneratedClass", "W_Button_C", "/Game/UI/Button/W_Button");
                if (!cSetting) cButton = 0;
            }
            std::lock_guard<std::mutex> g(lock);
            screens.swap(w);
            for (auto& r : fresh)
                if (!plates.count(r.ptr)) waiting.push_back(r);
        }
        if (api->game_thread_ready() && !InterlockedCompareExchange(&queued, 1, 0)) api->run_on_game_thread(tick, nullptr);
        Sleep(33);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(L"custom_plates.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
