// FTL co-op: on-screen status (top left) and the host's hotkeys.
//  F9  host: start the run (when not starting automatically)
//  F10 host: resync everyone from the host's save
//  F11 host: switch ship sharing (free-for-all <-> own crew)
#include "CoopCommands.h"
#include "Global.h"
#include <ctime>
#include <deque>

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
    coop::Session* s = CoopGame::GetSession();
    std::string head = "Co-op ";
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
    for (auto it = g_lines.begin(); it != g_lines.end();)
    {
        if (it->until < now) { it = g_lines.erase(it); continue; }
        freetype::easy_print(6, 8.f, y, it->text);
        y += 13.f;
        ++it;
    }
}

} // namespace CoopOverlay

HOOK_METHOD_PRIORITY(CApp, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRender -> Begin (CoopOverlay.cpp)\n")
    super();
    CoopOverlay::Render();
}
