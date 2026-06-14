#include <Windows.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// -------------------------- //
//     OFFSETS (cracked 2026)  //
// -------------------------- //
#define OFFSET_GOM               0x2B8F4A0
#define OFFSET_LOCAL_PLAYER      0xB8
#define OFFSET_POS_X             0x90
#define OFFSET_POS_Y             0x94
#define OFFSET_POS_Z             0x98
#define OFFSET_HEALTH            0x430
#define OFFSET_TEAM              0x560
#define OFFSET_NAME              0x600
#define OFFSET_SLEEPING          0x38
#define OFFSET_VIEW_MATRIX       0x3C5A8B0
#define OFFSET_MODEL             0x38
#define OFFSET_BONE_TRANSFORMS   0x28
#define OFFSET_ACTIVE_WEAPON     0x578
#define OFFSET_SHOT_COUNT        0x1A0
#define OFFSET_RECOIL_DELTA_X    0xE8 + 0x10
#define OFFSET_RECOIL_DELTA_Y    0xE8 + 0x14
#define OFFSET_VELOCITY          0x3B0        // for bullet prediction
#define OFFSET_CAMERA_PTR        0x3E5A8B0

// -------------------------- //
//        STRUCTS             //
// -------------------------- //
struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
    float Distance(const Vector3& other) const {
        float dx = x - other.x, dy = y - other.y, dz = z - other.z;
        return sqrt(dx*dx + dy*dy + dz*dz);
    }
    Vector3 operator+(const Vector3& v) const { return Vector3(x+v.x, y+v.y, z+v.z); }
    Vector3 operator-(const Vector3& v) const { return Vector3(x-v.x, y-v.y, z-v.z); }
};

struct Player {
    uintptr_t ptr;
    Vector3 pos;
    Vector3 velocity;
    float health;
    int team;
    char name[64];
    bool sleeping;
    bool alive;
    float distance;
    int weaponId;      // to show icon
};

// -------------------------- //
//      MEMORY HELPERS        //
// -------------------------- //
uintptr_t GetModuleBase(const wchar_t* mod) { return (uintptr_t)GetModuleHandleW(mod); }

template<typename T> T Read(uintptr_t addr) {
    if (!addr || IsBadReadPtr((void*)addr, sizeof(T))) return T();
    return *(T*)addr;
}

template<typename T> void Write(uintptr_t addr, T val) {
    if (!addr || IsBadReadPtr((void*)addr, sizeof(T))) return;
    *(T*)addr = val;
}

uintptr_t GetGOM() {
    uintptr_t unity = GetModuleBase(L"UnityPlayer.dll");
    return unity ? Read<uintptr_t>(unity + OFFSET_GOM) : 0;
}

// -------------------------- //
//         PLAYER LIST        //
// -------------------------- //
std::vector<Player> GetPlayers(uintptr_t localPtr, Vector3 localPos) {
    std::vector<Player> players;
    uintptr_t gom = GetGOM();
    if (!gom) return players;
    for (int i = 0; i < 512; i++) {
        uintptr_t ent = Read<uintptr_t>(gom + 0x20 + i*8);
        if (!ent || ent == localPtr) continue;
        Player p;
        p.ptr = ent;
        p.pos.x = Read<float>(ent + OFFSET_POS_X);
        p.pos.y = Read<float>(ent + OFFSET_POS_Y);
        p.pos.z = Read<float>(ent + OFFSET_POS_Z);
        p.velocity.x = Read<float>(ent + OFFSET_VELOCITY);
        p.velocity.y = Read<float>(ent + OFFSET_VELOCITY+4);
        p.velocity.z = Read<float>(ent + OFFSET_VELOCITY+8);
        p.health = Read<float>(ent + OFFSET_HEALTH);
        p.team = Read<int>(ent + OFFSET_TEAM);
        p.sleeping = (Read<int>(ent + OFFSET_SLEEPING) & 1) != 0;
        p.alive = (p.health > 0 && !p.sleeping);
        p.distance = localPos.Distance(p.pos);
        wchar_t wname[64];
        for (int j=0; j<64; j++) wname[j] = Read<wchar_t>(ent + OFFSET_NAME + j*2);
        wcstombs(p.name, wname, 64);
        p.name[63] = 0;
        if (p.alive && p.distance <= 400.0f) players.push_back(p);
    }
    return players;
}

// -------------------------- //
//       WORLD TO SCREEN      //
// -------------------------- //
float viewMatrix[16];
void UpdateViewMatrix() {
    uintptr_t addr = GetModuleBase(L"UnityPlayer.dll") + OFFSET_VIEW_MATRIX;
    memcpy(viewMatrix, (void*)addr, sizeof(viewMatrix));
}
bool WorldToScreen(Vector3 world, int& sx, int& sy, int sw, int sh) {
    float w = viewMatrix[12]*world.x + viewMatrix[13]*world.y + viewMatrix[14]*world.z + viewMatrix[15];
    if (w < 0.01f) return false;
    float inv = 1.0f/w;
    float x = (viewMatrix[0]*world.x + viewMatrix[1]*world.y + viewMatrix[2]*world.z + viewMatrix[3]) * inv;
    float y = (viewMatrix[4]*world.x + viewMatrix[5]*world.y + viewMatrix[6]*world.z + viewMatrix[7]) * inv;
    sx = (int)((x+1.0f)*0.5f*sw);
    sy = (int)((1.0f-(y+1.0f)*0.5f)*sh);
    return true;
}

// -------------------------- //
//          BONES             //
// -------------------------- //
Vector3 GetBonePos(uintptr_t player, int bone) {
    uintptr_t model = Read<uintptr_t>(player + OFFSET_MODEL);
    if (!model) return Vector3();
    uintptr_t bones = Read<uintptr_t>(model + OFFSET_BONE_TRANSFORMS);
    if (!bones) return Vector3();
    return Vector3(
        Read<float>(bones + bone*48 + 0x10),
        Read<float>(bones + bone*48 + 0x14),
        Read<float>(bones + bone*48 + 0x18)
    );
}
Vector3 GetHeadPos(uintptr_t player) { return GetBonePos(player, 8); }

// -------------------------- //
//      AIMBOT + PREDICTION    //
// -------------------------- //
Vector3 PredictPosition(Vector3 targetPos, Vector3 targetVel, float bulletSpeed = 300.0f) {
    float dist = targetPos.Distance(Read<Vector3>(Read<uintptr_t>(GetGOM()+OFFSET_LOCAL_PLAYER)+OFFSET_POS_X));
    float time = dist / bulletSpeed;
    return targetPos + Vector3(targetVel.x * time, targetVel.y * time, targetVel.z * time);
}

void AimAt(Vector3 target, Vector3 localPos, float smooth, bool silent = false) {
    float dx = target.x - localPos.x, dy = target.y - localPos.y, dz = target.z - localPos.z;
    float yaw = atan2(dy, dx) * 180.0f / 3.14159265f;
    float pitch = -atan2(dz, sqrt(dx*dx+dy*dy)) * 180.0f / 3.14159265f;
    uintptr_t cam = Read<uintptr_t>(GetModuleBase(L"UnityPlayer.dll") + OFFSET_CAMERA_PTR);
    if (!cam) return;
    float* curYaw = (float*)(cam + 0x3C);
    float* curPitch = (float*)(cam + 0x40);
    if (silent) {
        // silent aim: write angles directly before shot – requires frame-perfect timing
        *curYaw = yaw;
        *curPitch = pitch;
    } else {
        *curYaw += (yaw - *curYaw) * smooth;
        *curPitch += (pitch - *curPitch) * smooth;
    }
}

// -------------------------- //
//       ANTI-RECOIL (PER WEAPON)
// -------------------------- //
struct RecoilPattern { float x, y; };
std::map<int, std::vector<RecoilPattern>> weaponPatterns;
void InitRecoilPatterns() {
    // AK (id 0)
    weaponPatterns[0] = {{-0.5,1.2},{-0.3,1.5},{0.2,1.8},{0.7,2.0},{1.2,2.1},{1.5,2.0},{1.2,1.8},{0.8,1.5},{0.3,1.2},{-0.2,0.8}};
    // LR300 (id 1)
    weaponPatterns[1] = {{-0.3,0.9},{-0.1,1.1},{0.2,1.3},{0.5,1.4},{0.8,1.4},{1.0,1.3},{0.8,1.1},{0.4,0.9}};
    // M2 (id 2)
    weaponPatterns[2] = {{-0.8,1.5},{-0.5,2.0},{0.0,2.5},{0.6,2.8},{1.2,2.9},{1.8,2.8},{2.0,2.5},{1.5,2.0}};
}

int currentWeaponId = 0;
int shotCounter = 0;
float recoilPercent = 85.0f;
void ApplyAntiRecoil() {
    uintptr_t local = Read<uintptr_t>(GetGOM() + OFFSET_LOCAL_PLAYER);
    if (!local) return;
    uintptr_t weapon = Read<uintptr_t>(local + OFFSET_ACTIVE_WEAPON);
    if (!weapon) return;
    int newShot = Read<int>(weapon + OFFSET_SHOT_COUNT);
    if (newShot == shotCounter) return;
    shotCounter = newShot;
    // read weapon ID (simplified)
    currentWeaponId = Read<int>(weapon + 0x30);
    auto it = weaponPatterns.find(currentWeaponId);
    if (it == weaponPatterns.end()) return;
    int idx = (shotCounter-1) % it->second.size();
    Vector3 recoilDelta = { it->second[idx].x, it->second[idx].y, 0.0f };
    Vector3 correction;
    correction.x = -recoilDelta.x * (recoilPercent / 100.0f);
    correction.y = -recoilDelta.y * (recoilPercent / 100.0f);
    Write<Vector3>(weapon + OFFSET_RECOIL_DELTA_X, correction);
}

// -------------------------- //
//      CONFIG SYSTEM         //
// -------------------------- //
struct Config {
    bool espEnabled = true;
    bool aimbotEnabled = true;
    bool recoilEnabled = true;
    bool radarEnabled = false;
    bool silentAim = false;
    bool predictionEnabled = true;
    float aimSmooth = 0.3f;
    int aimFOV = 150;
    float maxRange = 400.0f;
    float bulletSpeed = 300.0f;
    int toggleKey = VK_F5;
    int aimbotKey = VK_RBUTTON;
    int triggerKey = 0;
} g_config;

void SaveConfig() {
    std::ofstream f("RustGodMode.cfg");
    f << g_config.espEnabled << "\n" << g_config.aimbotEnabled << "\n" << g_config.recoilEnabled << "\n"
      << g_config.radarEnabled << "\n" << g_config.silentAim << "\n" << g_config.predictionEnabled << "\n"
      << g_config.aimSmooth << "\n" << g_config.aimFOV << "\n" << g_config.maxRange << "\n"
      << g_config.bulletSpeed << "\n" << g_config.toggleKey << "\n" << g_config.aimbotKey << "\n" << g_config.triggerKey;
}
void LoadConfig() {
    std::ifstream f("RustGodMode.cfg");
    if (f) f >> g_config.espEnabled >> g_config.aimbotEnabled >> g_config.recoilEnabled
           >> g_config.radarEnabled >> g_config.silentAim >> g_config.predictionEnabled
           >> g_config.aimSmooth >> g_config.aimFOV >> g_config.maxRange
           >> g_config.bulletSpeed >> g_config.toggleKey >> g_config.aimbotKey >> g_config.triggerKey;
}

// -------------------------- //
//      IMGUI + RADAR         //
// -------------------------- //
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dContext = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
HWND g_hwnd = nullptr;
bool g_menuOpen = true;

void RenderRadar(const std::vector<Player>& players, Vector3 localPos, int localTeam, int radarSize=200) {
    ImVec2 pos = ImVec2(10, 10);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(radarSize, radarSize));
    ImGui::Begin("Radar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 center = ImVec2(pos.x + radarSize/2, pos.y + radarSize/2);
    draw->AddCircle(center, 5, IM_COL32(255,255,0,255), 12);
    for (auto& p : players) {
        float dx = p.pos.x - localPos.x;
        float dz = p.pos.z - localPos.z;
        float scale = radarSize / 100.0f;
        int x = (int)(center.x + dx * scale);
        int y = (int)(center.y + dz * scale);
        if (x > pos.x && x < pos.x+radarSize && y > pos.y && y < pos.y+radarSize) {
            ImColor col = (p.team == localTeam) ? ImColor(0,255,0) : ImColor(255,0,0);
            draw->AddCircleFilled(ImVec2((float)x, (float)y), 3, col);
        }
    }
    ImGui::End();
}

void RenderUI() {
    if (!g_menuOpen) return;
    ImGui::Begin("Rust GodMode v2", &g_menuOpen, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TabBar("Tabs");
    if (ImGui::BeginTabItem("ESP")) {
        ImGui::Checkbox("Enable ESP", &g_config.espEnabled);
        ImGui::SliderFloat("Max Range", &g_config.maxRange, 50, 400, "%.0f m");
        ImGui::Checkbox("Radar Overlay", &g_config.radarEnabled);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Aimbot")) {
        ImGui::Checkbox("Enable Aimbot", &g_config.aimbotEnabled);
        ImGui::SliderFloat("Smooth", &g_config.aimSmooth, 0.05f, 0.95f, "%.2f");
        ImGui::SliderInt("FOV (pixels)", &g_config.aimFOV, 30, 300);
        ImGui::Checkbox("Silent Aim", &g_config.silentAim);
        ImGui::Checkbox("Bullet Prediction", &g_config.predictionEnabled);
        if (g_config.predictionEnabled)
            ImGui::SliderFloat("Bullet Speed", &g_config.bulletSpeed, 150, 600, "%.0f m/s");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Recoil")) {
        ImGui::Checkbox("Anti-Recoil", &g_config.recoilEnabled);
        ImGui::SliderFloat("Recoil %", &recoilPercent, 0, 100, "%.0f%%");
        ImGui::Text("Per-weapon patterns loaded");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Keybinds")) {
        int keys[] = {VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12};
        const char* keyNames[] = {"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"};
        ImGui::Combo("Toggle Menu", (int*)&g_config.toggleKey, keyNames, 12);
        ImGui::Combo("Aimbot Hold Key", (int*)&g_config.aimbotKey, keyNames, 12);
        ImGui::Combo("Triggerbot Key", (int*)&g_config.triggerKey, keyNames, 12);
        if (ImGui::Button("Save Config")) SaveConfig();
        ImGui::SameLine();
        if (ImGui::Button("Load Config")) LoadConfig();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    ImGui::End();
}

void RenderESP(const std::vector<Player>& players, int localTeam, int sw, int sh) {
    if (!g_config.espEnabled) return;
    for (const auto& p : players) {
        int headX, headY, feetX, feetY;
        Vector3 head = GetHeadPos(p.ptr);
        if (!WorldToScreen(head, headX, headY, sw, sh)) continue;
        if (!WorldToScreen(p.pos, feetX, feetY, sw, sh)) continue;
        float height = feetY - headY;
        float width = height * 0.6f;
        int left = headX - (int)(width/2);
        int top = headY;
        ImColor color = (p.team == localTeam) ? ImColor(0,255,0) : ImColor(255,0,0);
        ImGui::GetBackgroundDrawList()->AddRect(ImVec2(left,top), ImVec2(left+width,top+height), color, 2.0f);
        float healthWidth = width * (p.health/100.0f);
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(left,top+height+2), ImVec2(left+healthWidth,top+height+6), ImColor(0,255,0));
        char buf[64];
        sprintf_s(buf, "%s [%.0fm]", p.name, p.distance);
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(left,top-15), ImColor(255,255,255), buf);
    }
}

// -------------------------- //
//      MAIN THREAD           //
// -------------------------- //
DWORD WINAPI MainThread(LPVOID) {
    while (!(g_hwnd = FindWindowA(NULL, "Rust")) && !(g_hwnd = FindWindowA(NULL, "Rust Client"))) Sleep(100);
    CreateDeviceD3D(g_hwnd);
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);
    InitRecoilPatterns();
    LoadConfig();
    
    MSG msg;
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        
        if (GetAsyncKeyState(g_config.toggleKey) & 0x8001) { g_menuOpen = !g_menuOpen; Sleep(200); }
        
        uintptr_t local = Read<uintptr_t>(GetGOM() + OFFSET_LOCAL_PLAYER);
        if (!local) { Sleep(100); continue; }
        Vector3 localPos;
        localPos.x = Read<float>(local+OFFSET_POS_X);
        localPos.y = Read<float>(local+OFFSET_POS_Y);
        localPos.z = Read<float>(local+OFFSET_POS_Z);
        int localTeam = Read<int>(local+OFFSET_TEAM);
        auto players = GetPlayers(local, localPos);
        UpdateViewMatrix();
        RECT rect; GetClientRect(g_hwnd, &rect);
        int sw = rect.right-rect.left, sh = rect.bottom-rect.top;
        
        RenderESP(players, localTeam, sw, sh);
        if (g_config.radarEnabled) RenderRadar(players, localPos, localTeam);
        
        if (g_config.aimbotEnabled && (GetAsyncKeyState(g_config.aimbotKey) & 0x8000)) {
            Player best;
            float bestDist = (float)g_config.aimFOV;
            int cx = sw/2, cy = sh/2;
            for (auto& p : players) {
                if (p.team == localTeam) continue;
                int x,y;
                if (WorldToScreen(GetHeadPos(p.ptr), x, y, sw, sh)) {
                    float d = sqrt((x-cx)*(x-cx)+(y-cy)*(y-cy));
                    if (d < bestDist) { bestDist = d; best = p; }
                }
            }
            if (bestDist < g_config.aimFOV) {
                Vector3 target = GetHeadPos(best.ptr);
                if (g_config.predictionEnabled) target = PredictPosition(target, best.velocity, g_config.bulletSpeed);
                AimAt(target, localPos, g_config.aimSmooth, g_config.silentAim);
            }
        }
        if (g_config.recoilEnabled) ApplyAntiRecoil();
        
        // Triggerbot
        if (g_config.triggerKey && (GetAsyncKeyState(g_config.triggerKey) & 0x8000)) {
            // find if crosshair on enemy
            int cx = sw/2, cy = sh/2;
            for (auto& p : players) {
                int x,y;
                if (p.team != localTeam && WorldToScreen(GetHeadPos(p.ptr), x, y, sw, sh)) {
                    if (abs(x-cx) < 15 && abs(y-cy) < 15) {
                        // simulate click
                        INPUT ip = {0}; ip.type = INPUT_MOUSE; ip.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                        SendInput(1, &ip, sizeof(INPUT));
                        ip.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                        SendInput(1, &ip, sizeof(INPUT));
                        Sleep(10);
                        break;
                    }
                }
            }
        }
        
        if (g_menuOpen) {
            ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
            RenderUI();
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        g_swapchain->Present(1, 0);
        Sleep(5);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}