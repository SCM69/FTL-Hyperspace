// FTL co-op: on-screen status (top left) and the host's hotkeys.
//  F9  host: start the run (when not starting automatically)
//  F10 host: resync everyone from the host's save
//  F11 host: switch ship sharing (free-for-all <-> own crew)
#include "CoopCommands.h"
#include "Global.h"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

namespace CoopOverlay {

struct Line { std::string text; double until; };
static std::deque<Line> g_lines;

static double Now() { return (double)time(nullptr); }

void Post(const std::string& text)
{
    g_lines.push_back(Line{ text, Now() + 6.0 });
    while (g_lines.size() > 5) g_lines.pop_front();
}

static std::string ModeName(CoopGame::Mode m) { return m == CoopGame::Mode::OwnCrew ? "own crew" : "free-for-all"; }

void Render()
{
    if (!CoopGame::Active()) return;
    if (CoopGame::WaitingForRoleChoice())
    {
        if (!G_->GetCApp()->menu.bOpen) return;
        std::vector<std::string> lines = CoopGame::RoleMenuLines();
        // Dark box behind the text (the menu background is busy), clear of the mouse pointer's usual corner.
        const float x = 24.f, y0 = 24.f, lineH = 16.f;
        float w = 0.f;
        for (const std::string& l : lines) w = std::max(w, (float)freetype::easy_measureWidth(6, l));
        CSurface::GL_DrawRect(x - 10.f, y0 - 8.f, w + 20.f, lineH * lines.size() + 12.f, GL_Color(0.f, 0.f, 0.f, 0.75f));
        float y = y0;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (i == 0) CSurface::GL_SetColor(GL_Color(1.f, 0.85f, 0.3f, 1.f)); else CSurface::GL_SetColor(GL_Color(1.f, 1.f, 1.f, 1.f));
            freetype::easy_print(6, x, y, lines[i]);
            y += lineH;
        }
        return;
    }
    coop::Session* s = CoopGame::GetSession();
    std::string head = "Co-op ";
    std::string net = CoopGame::NetStatus();
    if (!s) head += "(network error, see coop.log)";
    else
    {
        head += s->IsHost() ? "host" : ("player " + std::to_string(s->Slot() + 1));
        head += " | " + std::to_string(s->PlayerCount()) + " players | " + ModeName(CoopGame::CurrentMode());
        if (!s->Started()) head += s->IsHost() ? " | waiting (F9 starts)" : " | waiting for the host";
        else if (!CoopGame::InLockstep()) head += " | loading...";
    }
    CSurface::GL_SetColor(GL_Color(1.f, 1.f, 1.f, 1.f));
    freetype::easy_print(6, 8.f, 4.f, head);
    double now = Now();
    float y = 18.f;
    // Steam lobby state (or the network error) until the run starts.
    if (!net.empty() && (!s || !s->Started())) { freetype::easy_print(6, 8.f, y, net); y += 13.f; }
    for (auto it = g_lines.begin(); it != g_lines.end();)
    {
        if (it->until < now) { it = g_lines.erase(it); continue; }
        freetype::easy_print(6, 8.f, y, it->text);
        y += 13.f;
        ++it;
    }
}

} // namespace CoopOverlay

// Tests: FTL_COOP_SHOT_AT=<n> saves the frame drawn on render call n (and every n after it) as <coop log>.<n>.ppm.
static void MaybeScreenshot()
{
    static int every = getenv("FTL_COOP_SHOT_AT") ? atoi(getenv("FTL_COOP_SHOT_AT")) : 0;
    static int calls = 0;
    if (every <= 0 || ++calls % every != 0) return;
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    std::vector<unsigned char> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(vp[0], vp[1], w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::string path = CoopGame::GetConfig().logPath + "." + std::to_string(calls) + ".ppm";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
    fclose(f);
}

HOOK_METHOD_PRIORITY(CApp, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRender -> Begin (CoopOverlay.cpp)\n")
    super();
    CoopOverlay::Render();
    MaybeScreenshot();
}

// Own-crew mode: show which player commands each crew member (P1 = host). Same on every copy: owners are assigned in
// crew-list order and changed only by lockstep commands.
static const GL_Color kPlayerColors[8] = {
    GL_Color(1.f, 1.f, 1.f, 1.f), GL_Color(0.4f, 0.8f, 1.f, 1.f), GL_Color(1.f, 0.6f, 0.2f, 1.f), GL_Color(0.5f, 1.f, 0.4f, 1.f),
    GL_Color(1.f, 0.4f, 0.8f, 1.f), GL_Color(1.f, 1.f, 0.3f, 1.f), GL_Color(0.7f, 0.5f, 1.f, 1.f), GL_Color(0.3f, 1.f, 0.9f, 1.f) };

HOOK_METHOD_PRIORITY(CrewMember, OnRender, -2000, (bool outlineOnly) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewMember::OnRender -> Begin (CoopOverlay.cpp)\n")
    super(outlineOnly);
    if (outlineOnly || bDead || !CoopGame::InLockstep() || CoopGame::CurrentMode() != CoopGame::Mode::OwnCrew) return;
    if (iShipId != 0) return; // the player ship's crew only (also when they board the enemy)
    int owner = CoopGame::CrewOwner(this);
    if (owner < 0 || owner > 7) return;
    CSurface::GL_SetColor(kPlayerColors[owner]);
    freetype::easy_printCenter(6, x, y - 24.f, "P" + std::to_string(owner + 1));
}

// F7 (host): give the selected crew members to the next player.
HOOK_METHOD_PRIORITY(CApp, OnKeyDown, 8000, (SDLKey key) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnKeyDown -> Begin (CoopOverlay.cpp)\n")
    coop::Session* s = CoopGame::GetSession();
    if (key == SDLK_F7 && CoopGame::InLockstep() && s && s->IsHost() && G_->GetWorld()->commandGui)
    {
        CrewControl& cc = G_->GetWorld()->commandGui->crewControl;
        ShipManager* ship = G_->GetShipManager(0);
        std::vector<int> slots = s->Slots();
        for (CrewMember* c : cc.selectedCrew)
        {
            int idx = -1;
            for (size_t i = 0; ship && i < ship->vCrewList.size(); ++i) if (ship->vCrewList[i] == c) idx = (int)i;
            if (idx < 0 || slots.empty()) continue;
            int cur = CoopGame::CrewOwner(c);
            size_t k = 0;
            while (k < slots.size() && slots[k] != cur) ++k;
            int next = slots[(k + 1) % slots.size()];
            CoopGame::SubmitLocal(CoopCommands::AssignCrew(idx, next));
            CoopOverlay::Post(std::string("Crew member given to player ") + std::to_string(next + 1));
        }
        return;
    }
    super(key);
}
