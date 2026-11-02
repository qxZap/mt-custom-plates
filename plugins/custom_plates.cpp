// Custom Plates -- real text on number plate attachments, as plugins\mt_plugin_CustomPlates.dll.
//
// The plate itself is a normal attachment (Attachment_SR_Number_Plate_01..08 in More Attachments),
// so it is placed, moved and removed with the game's own attachment tools. This puts text on it.
// A TextRenderComponent rides on each plate, one per face: the meshes are thin along Y and centred
// on the actor, and text only reads from its front, so each face gets its own.
//
// Settings: Binaries\Win64\plugins\custom_plates.ini, [Plate]: Text (QX 123), Size (6, cm),
// X and Z (0, cm across and up the plate), Depth (1.1, cm off the centre of the plate).
//
// shortcut: one text for every plate on this machine, read at startup. Per-car text and the Tuning
// rows come once the text is proven in game. Other players see a plain plate.

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

static uintptr_t cActor, cText, fAdd, fToText, textCDO, fSetText, fWorldSize, fColor, fHorizontal, fVertical;
static std::wstring plateText;
static float size, across, up, depth;
static std::mutex lock;
static std::vector<Ref> waiting;
static LONG queued;

static uintptr_t makeText(uintptr_t actor, double yaw, double y, const unsigned char* text) {
    alignas(16) unsigned char p[0x100] = {0};
    int at = off(fAdd, "RelativeTransform");
    double half = yaw * 3.14159265358979 / 360;
    double spin[4] = {0, 0, sin(half), cos(half)}, where[3] = {across, y, up}, scale[3] = {1, 1, 1};
    memcpy(p + off(fAdd, "Class"), &cText, 8);
    memcpy(p + at, spin, sizeof spin);
    memcpy(p + at + 0x20, where, sizeof where);
    memcpy(p + at + 0x40, scale, sizeof scale);
    api->process_event(actor, fAdd, p);
    uintptr_t c = 0;
    memcpy(&c, p + off(fAdd, "ReturnValue"), 8);
    if (!c) return 0;
    unsigned char t[0x40] = {0};
    memcpy(t, text, 0x18);
    api->process_event(c, fSetText, t);
    api->process_event(c, fWorldSize, &size);
    struct { uint8_t b, g, r, a; } black = {10, 10, 10, 255};
    api->process_event(c, fColor, &black);
    uint8_t centre = 1;  // EHTA_Center, EVRTA_TextCenter
    api->process_event(c, fHorizontal, &centre);
    api->process_event(c, fVertical, &centre);
    return c;
}

static void tick(void*) {
    InterlockedExchange(&queued, 0);
    std::vector<Ref> todo;
    { std::lock_guard<std::mutex> g(lock); todo.swap(waiting); }
    if (todo.empty()) return;
    unsigned char t[0x60] = {0};
    FStr s = {plateText.c_str(), (int)plateText.size() + 1, (int)plateText.size() + 1};
    memcpy(t + off(fToText, "InString"), &s, sizeof s);
    api->process_event(textCDO, fToText, t);
    for (auto& r : todo) {
        if (!alive(r)) continue;
        unsigned char* text = t + off(fToText, "ReturnValue");
        bool ok = makeText(r.ptr, 90, depth, text) && makeText(r.ptr, -90, -depth, text);
        MT_LOG("%s: text %s", objName(classOf(r.ptr)).c_str(), ok ? "added" : "FAILED");
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Custom Plates loaded");
    iniSet(L"custom_plates.ini", L"Plate", L"Text", L"QX 123");
    plateText = iniGet(L"custom_plates.ini", L"Plate", L"Text");
    size = (float)iniNum(L"custom_plates.ini", L"Plate", L"Size", 6);
    across = (float)iniNum(L"custom_plates.ini", L"Plate", L"X", 0);
    up = (float)iniNum(L"custom_plates.ini", L"Plate", L"Z", 0);
    depth = (float)iniNum(L"custom_plates.ini", L"Plate", L"Depth", 1.1);
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }

    cActor = api->find_object("Class", "Actor", "/Script/Engine");
    cText = api->find_object("Class", "TextRenderComponent", "/Script/Engine");
    fAdd = api->find_object("Function", "AddComponentByClass", "Actor");
    fToText = api->find_object("Function", "Conv_StringToText", "KismetTextLibrary");
    textCDO = api->find_object("KismetTextLibrary", "Default__KismetTextLibrary", "/Script/Engine");
    fSetText = api->find_object("Function", "K2_SetText", "TextRenderComponent");
    fWorldSize = api->find_object("Function", "SetWorldSize", "TextRenderComponent");
    fColor = api->find_object("Function", "SetTextRenderColor", "TextRenderComponent");
    fHorizontal = api->find_object("Function", "SetHorizontalAlignment", "TextRenderComponent");
    fVertical = api->find_object("Function", "SetVerticalAlignment", "TextRenderComponent");
    for (uintptr_t f : {cActor, cText, fAdd, fToText, textCDO, fSetText, fWorldSize, fColor, fHorizontal, fVertical})
        if (!f) { MT_LOG("FAILED: text functions not found -- plates stay plain"); return 0; }

    std::unordered_map<uintptr_t, bool> plateClass;  // a name test per class, not per object
    std::map<uintptr_t, Ref> dressed;
    for (;;) {
        Sleep(5000);
        std::vector<Ref> fresh;
        for (int i = 0, n = api->num_objects(); i < n; i++) {
            uintptr_t o = api->object(i);
            if (!o || isCDO(o)) continue;
            uintptr_t k = classOf(o);
            auto known = plateClass.find(k);
            bool plate = known != plateClass.end() ? known->second : (plateClass[k] = objName(k).find("Number_Plate") != std::string::npos);
            if (!plate || !api->is_a(k, cActor)) continue;
            auto had = dressed.find(o);
            if (had != dressed.end() && had->second.index == i) continue;
            dressed[o] = {i, o};
            fresh.push_back({i, o});
        }
        for (auto it = dressed.begin(); it != dressed.end();) it = alive(it->second) ? std::next(it) : dressed.erase(it);
        if (fresh.empty() || !api->game_thread_ready()) continue;
        { std::lock_guard<std::mutex> g(lock); waiting.insert(waiting.end(), fresh.begin(), fresh.end()); }
        if (!InterlockedCompareExchange(&queued, 1, 0)) api->run_on_game_thread(tick, nullptr);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(L"custom_plates.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
