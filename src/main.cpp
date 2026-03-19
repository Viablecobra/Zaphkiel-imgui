#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "zaphkiel_bridge.h"

static bool       g_initialized   = false;
static int        g_width         = 0;
static int        g_height        = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

struct WindowBounds { float x, y, w, h; bool visible; };
static WindowBounds g_menuBounds = {0, 0, 0, 0, false};
static std::mutex   g_boundsMutex;

struct SearchResult { std::string file; int line; std::string text; };
static std::vector<SearchResult> g_search_results;
static char   g_search_buf[256] = {};
static std::mutex g_search_mutex;

static void do_search(const char* query) {
    std::vector<SearchResult> results;
    const char* files[] = {
        "functions.txt","imports.txt","exports.txt","vtables.txt",
        "rtti.txt","names.txt","strings.txt","xrefs.txt",
        "pseudo_c.txt","structs.txt","patterns.txt","relocs.txt",
    };
    std::string q = query;
    for (auto& c : q) c = tolower(c);
    for (auto& fname : files) {
        std::string path = std::string("/storage/emulated/0/games/kurumi/") + fname;
        FILE* f = fopen(path.c_str(), "r");
        if (!f) continue;
        char line[512];
        int lineno = 0;
        while (fgets(line, sizeof(line), f)) {
            lineno++;
            std::string l = line;
            std::string ll = l;
            for (auto& c : ll) c = tolower(c);
            if (ll.find(q) != std::string::npos) {
                if (l.size() > 100) l.resize(100);
                results.push_back({fname, lineno, l});
                if (results.size() >= 200) { fclose(f); goto done; }
            }
        }
        fclose(f);
    }
    done:
    std::lock_guard<std::mutex> lock(g_search_mutex);
    g_search_results = results;
}

void drawmenu() {
    static bool show_menu   = false;
    static int  current_tab = 0;

    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(
        ImVec2(0.0f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.0f, 0.5f));
    ImGui::Begin("MenuTrigger", nullptr,
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f,0.1f,0.1f,0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f,0.2f,0.2f,1.0f));
    if (ImGui::Button("OPEN MENU", ImVec2(200, 80)))
        show_menu = !show_menu;
    ImGui::PopStyleColor(2);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();

    if (!show_menu) return;

    ImGui::SetNextWindowSize(ImVec2(1000, 650), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f,0.06f,0.06f,1.0f));

    ImGui::Begin("Zaphkiel", &show_menu,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

    ImVec2      win_pos  = ImGui::GetWindowPos();
    ImVec2      win_size = ImGui::GetWindowSize();
    ImDrawList* dl       = ImGui::GetWindowDrawList();

    float t = (float)ImGui::GetTime();
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(fmodf(t * 0.5f, 1.0f), 1.0f, 1.0f, r, g, b);
    dl->AddRectFilled(win_pos,
        ImVec2(win_pos.x + win_size.x, win_pos.y + 5.0f),
        ImColor(r, g, b));

    ImGui::SetCursorPosY(5.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f,0.04f,0.04f,1.0f));
    ImGui::BeginChild("Sidebar", ImVec2(120, win_size.y - 5.0f), false);

    for (int i = 0; i < 3; ++i) {
        ImVec2 cp     = ImGui::GetCursorScreenPos();
        ImVec2 center = ImVec2(cp.x + 60.0f, cp.y + 60.0f);

        if (current_tab == i)
            dl->AddRectFilled(cp,
                ImVec2(cp.x + 120.0f, cp.y + 120.0f),
                ImColor(0.12f, 0.12f, 0.12f, 1.0f));

        if (ImGui::InvisibleButton(
                (std::string("tab_") + std::to_string(i)).c_str(),
                ImVec2(120.0f, 120.0f)))
            current_tab = i;

        ImU32 col = (current_tab == i) ? ImColor(255,255,255) : ImColor(150,150,150);
        float stk = 4.0f;

        if (i == 0) {
            dl->AddCircle(ImVec2(center.x - 4, center.y - 4), 14.0f, col, 0, stk);
            dl->AddLine(ImVec2(center.x + 6,  center.y + 6),
                        ImVec2(center.x + 18, center.y + 18), col, stk);
        } else if (i == 1) {
            dl->AddRectFilled(ImVec2(center.x-18,center.y-14),ImVec2(center.x+18,center.y-6), col);
            dl->AddRectFilled(ImVec2(center.x-18,center.y-2), ImVec2(center.x+8, center.y+6), col);
            dl->AddRectFilled(ImVec2(center.x-18,center.y+10),ImVec2(center.x+14,center.y+18),col);
        } else {
            dl->AddCircle(center, 14.0f, col, 0, stk);
            dl->AddCircleFilled(center, 5.0f, col);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);

    ImGui::BeginChild("Content", ImVec2(win_size.x - 120.0f, win_size.y - 5.0f), false);
    ImGui::SetCursorPos(ImVec2(20.0f, 20.0f));

    if (current_tab == 0) {
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(ImVec4(0.4f,0.9f,0.6f,1.0f), "Search Dumps");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(500.0f);
        bool enter = ImGui::InputText("##q", g_search_buf, sizeof(g_search_buf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool btn = ImGui::Button("Search", ImVec2(120, 0));

        if ((enter || btn) && strlen(g_search_buf) >= 2) {
            std::string q = g_search_buf;
            pthread_t pt;
            pthread_create(&pt, nullptr, [](void* arg) -> void* {
                std::string* q = (std::string*)arg;
                do_search(q->c_str());
                delete q;
                return nullptr;
            }, new std::string(q));
            pthread_detach(pt);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        {
            std::lock_guard<std::mutex> lock(g_search_mutex);
            if (g_search_results.empty()) {
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f),
                    strlen(g_search_buf) < 2
                        ? "Type at least 2 characters and press Search"
                        : "No results");
            } else {
                ImGui::Text("%zu results:", g_search_results.size());
                ImGui::BeginChild("##results", ImVec2(0, 0), false);
                for (auto& res : g_search_results) {
                    ImGui::TextColored(ImVec4(0.4f,0.7f,1.0f,1.0f),
                        "[%s:%d]", res.file.c_str(), res.line);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(res.text.c_str());
                }
                ImGui::EndChild();
            }
        }

    } else if (current_tab == 1) {
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(ImVec4(0.4f,0.9f,0.6f,1.0f), "Zaphkiel Dumpers");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        ImGui::Spacing();

        if (dlsym(RTLD_DEFAULT, "zaphkiel_draw_tab")) {
            Zaphkiel::DrawTab();
        } else {
            ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f),
                "Waiting for Zaphkiel to initialise...");
            ImGui::Spacing();
            FILE* f = fopen("/storage/emulated/0/games/kurumi/.progress", "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f))
                    ImGui::TextUnformatted(line);
                fclose(f);
            } else {
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f), "No progress data yet");
            }
        }

    } else {
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f), "Settings");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f), "Add your settings here");
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    {
        std::lock_guard<std::mutex> lock(g_boundsMutex);
        if (show_menu)
            g_menuBounds = {win_pos.x, win_pos.y, win_size.x, win_size.y, true};
        else
            g_menuBounds.visible = false;
    }
}

static void setup() {
    if (g_initialized || g_width <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io        = ImGui::GetIO();
    io.IniFilename     = nullptr;
    io.FontGlobalScale = 1.4f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;

    GLint last_active_texture; glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
    glActiveTexture(GL_TEXTURE0);
    GLint last_tex0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex0);
    glActiveTexture(GL_TEXTURE1);
    GLint last_tex1; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex1);
    glActiveTexture(last_active_texture);
    GLint last_prog; glGetIntegerv(GL_CURRENT_PROGRAM,              &last_prog);
    GLint last_vbo;  glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &last_vbo);
    GLint last_ebo;  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_ebo);
    GLint last_fbo;  glGetIntegerv(GL_FRAMEBUFFER_BINDING,          &last_fbo);
    GLint last_vp[4];glGetIntegerv(GL_VIEWPORT,                     last_vp);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_depth   = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_blend   = glIsEnabled(GL_BLEND);

    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();

    drawmenu();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glUseProgram(last_prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, last_tex0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, last_tex1);
    glActiveTexture(last_active_texture);
    glBindBuffer(GL_ARRAY_BUFFER,          last_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,  last_ebo);
    glBindFramebuffer(GL_FRAMEBUFFER,      last_fbo);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_depth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (last_blend)   glEnable(GL_BLEND);        else glDisable(GL_BLEND);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT ||
        (g_targetcontext != EGL_NO_CONTEXT &&
         (ctx != g_targetcontext || surf != g_targetsurface)))
        return orig_eglswapbuffers(dpy, surf);

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) { g_targetcontext = ctx; g_targetsurface = surf; }
    g_width = w; g_height = h;

    setup();
    render();

    return orig_eglswapbuffers(dpy, surf);
}

typedef bool (*PreloaderInput_OnTouch_Fn)(int action, int pointerId, float x, float y);
struct PreloaderInput_Interface {
    void (*RegisterTouchCallback)(PreloaderInput_OnTouch_Fn callback);
};
typedef PreloaderInput_Interface* (*GetPreloaderInput_Fn)();

bool OnTouchCallback(int action, int pointerId, float x, float y) {
    if (!g_initialized) return false;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    if (action == AMOTION_EVENT_ACTION_DOWN)
        io.AddMouseButtonEvent(0, true);
    else if (action == AMOTION_EVENT_ACTION_UP)
        io.AddMouseButtonEvent(0, false);

    Zaphkiel::NotifyTouch(action, x, y);

    bool hitTest = false;
    {
        std::lock_guard<std::mutex> lock(g_boundsMutex);
        if (g_menuBounds.visible &&
            x >= g_menuBounds.x && x <= g_menuBounds.x + g_menuBounds.w &&
            y >= g_menuBounds.y && y <= g_menuBounds.y + g_menuBounds.h)
            hitTest = true;
    }
    return hitTest || io.WantCaptureMouse;
}

static void* mainthread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hegl = GlossOpen("libEGL.so");
    if (hegl) {
        void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
        if (swap)
            GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
    }

    void* preloaderLib = dlopen("libpreloader.so", RTLD_NOW);
    if (preloaderLib) {
        auto GetInput = (GetPreloaderInput_Fn)dlsym(preloaderLib, "GetPreloaderInput");
        if (GetInput) {
            PreloaderInput_Interface* input = GetInput();
            if (input && input->RegisterTouchCallback)
                input->RegisterTouchCallback(OnTouchCallback);
        }
    }
    return nullptr;
}

__attribute__((constructor))
void display_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
