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

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

// Zaphkiel bridge — resolved at runtime via dlsym, no link-time dependency
extern "C" void zaphkiel_draw_tab();
extern "C" bool zaphkiel_notify_touch(int action, float x, float y);

// ── state ─────────────────────────────────────────────────────────────────────

static bool       g_initialized   = false;
static int        g_width         = 0;
static int        g_height        = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

struct WindowBounds { float x, y, w, h; bool visible; };
static WindowBounds g_menuBounds = {0, 0, 0, 0, false};
static std::mutex   g_boundsMutex;

// ── drawmenu ──────────────────────────────────────────────────────────────────

void drawmenu() {
    static bool show_menu  = false;
    static int  current_tab = 0;

    ImGuiIO& io = ImGui::GetIO();

    // ── floating open button (left-center edge) ───────────────────────────────
    ImGui::SetNextWindowPos(
        ImVec2(0.0f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always,
        ImVec2(0.0f, 0.5f)
    );
    ImGui::Begin("MenuTrigger", nullptr,
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoBackground);

    ImGui::SetWindowFontScale(1.5f);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.1f, 0.1f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button("OPEN MENU", ImVec2(200, 80)))
        show_menu = !show_menu;
    ImGui::PopStyleColor(2);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();

    if (!show_menu) return;

    // ── main window ───────────────────────────────────────────────────────────
    ImGui::SetNextWindowSize(
        ImVec2(1000, 650),
        ImGuiCond_Appearing
    );
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing,
        ImVec2(0.5f, 0.5f)
    );
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 1.0f));

    ImGui::Begin("CustomUI_Main", &show_menu,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

    ImVec2      win_pos  = ImGui::GetWindowPos();
    ImVec2      win_size = ImGui::GetWindowSize();
    ImDrawList* dl       = ImGui::GetWindowDrawList();

    // rainbow top bar
    float t = (float)ImGui::GetTime();
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(fmodf(t * 0.5f, 1.0f), 1.0f, 1.0f, r, g, b);
    dl->AddRectFilled(win_pos,
        ImVec2(win_pos.x + win_size.x, win_pos.y + 5.0f),
        ImColor(r, g, b));

    ImGui::SetCursorPosY(5.0f);

    // ── sidebar tabs ─────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.04f, 0.04f, 1.0f));
    ImGui::BeginChild("Sidebar", ImVec2(120, win_size.y - 5.0f), false);

    // 4 tabs: 0=Home 1=Box 2=X 3=Zaphkiel(sliders icon)
    for (int i = 0; i < 4; ++i) {
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

        ImU32 col  = (current_tab == i) ? ImColor(255,255,255) : ImColor(150,150,150);
        float stk  = 4.0f;

        if (i == 0) {
            // person icon
            dl->AddCircleFilled(ImVec2(center.x, center.y - 8), 12.0f, col);
            dl->PathArcTo(center, 22.0f, 0.0f, 3.14159f);
            dl->PathStroke(col, false, stk);
        } else if (i == 1) {
            // box icon
            dl->AddRectFilled(ImVec2(center.x-12,center.y-12),ImVec2(center.x+12,center.y+12),col);
            dl->AddRect(ImVec2(center.x-20,center.y-20),ImVec2(center.x+20,center.y+20),col,0,0,stk);
        } else if (i == 2) {
            // X icon
            dl->AddLine(ImVec2(center.x-20,center.y-20),ImVec2(center.x+20,center.y+20),col,stk+1);
            dl->AddLine(ImVec2(center.x+20,center.y-20),ImVec2(center.x-20,center.y+20),col,stk+1);
        } else {
            // sliders icon (Zaphkiel)
            dl->AddLine(ImVec2(center.x-20,center.y-10),ImVec2(center.x+20,center.y-10),col,stk);
            dl->AddCircleFilled(ImVec2(center.x-5, center.y-10),6.0f,col);
            dl->AddLine(ImVec2(center.x-20,center.y+10),ImVec2(center.x+20,center.y+10),col,stk);
            dl->AddCircleFilled(ImVec2(center.x+8, center.y+10),6.0f,col);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg

    ImGui::SameLine(0, 0);

    // ── content area ─────────────────────────────────────────────────────────
    ImGui::BeginChild("Content", ImVec2(win_size.x - 120.0f, win_size.y - 5.0f), false);
    ImGui::SetCursorPos(ImVec2(40.0f, 40.0f));

    if (current_tab == 0) {
        // ── Tab 0: placeholder (was motion blur, now empty for your use) ──────
        ImGui::SetWindowFontScale(1.8f);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Tab 0 — add your features here");
        ImGui::SetWindowFontScale(1.0f);

    } else if (current_tab == 1) {
        // ── Tab 1: placeholder ────────────────────────────────────────────────
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Tab 1");
        ImGui::SetWindowFontScale(1.0f);

    } else if (current_tab == 2) {
        // ── Tab 2: placeholder ────────────────────────────────────────────────
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Tab 2");
        ImGui::SetWindowFontScale(1.0f);

    } else if (current_tab == 3) {
        // ── Tab 3: Zaphkiel ───────────────────────────────────────────────────
        // Calls into Rust — renders progress bars, search, dumper controls.
        // Defined in imgui_bridge.rs, exported as zaphkiel_draw_tab().
        // Resolved at runtime via dlsym — zero link-time dependency.
        static void (*zaphkiel_fn)() = nullptr;
        if (!zaphkiel_fn)
            zaphkiel_fn = (void(*)())dlsym(RTLD_DEFAULT, "zaphkiel_draw_tab");
        if (zaphkiel_fn)
            zaphkiel_fn();
        else
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Zaphkiel not loaded");
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(2);  // WindowPadding, WindowBorderSize

    // update bounds for hit-test in touch callback
    if (show_menu) {
        std::lock_guard<std::mutex> lock(g_boundsMutex);
        g_menuBounds = { win_pos.x, win_pos.y, win_size.x, win_size.y, true };
    } else {
        std::lock_guard<std::mutex> lock(g_boundsMutex);
        g_menuBounds.visible = false;
    }
}

// ── setup / render ────────────────────────────────────────────────────────────

static void setup() {
    if (g_initialized || g_width <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io  = ImGui::GetIO();
    io.IniFilename      = nullptr;
    io.FontGlobalScale  = 1.4f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;

    // save GL state
    GLint  last_active_texture; glGetIntegerv(GL_ACTIVE_TEXTURE,             &last_active_texture);
    glActiveTexture(GL_TEXTURE0);
    GLint  last_tex0;           glGetIntegerv(GL_TEXTURE_BINDING_2D,         &last_tex0);
    glActiveTexture(GL_TEXTURE1);
    GLint  last_tex1;           glGetIntegerv(GL_TEXTURE_BINDING_2D,         &last_tex1);
    glActiveTexture(last_active_texture);
    GLint  last_prog;           glGetIntegerv(GL_CURRENT_PROGRAM,            &last_prog);
    GLint  last_vbo;            glGetIntegerv(GL_ARRAY_BUFFER_BINDING,       &last_vbo);
    GLint  last_ebo;            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&last_ebo);
    GLint  last_fbo;            glGetIntegerv(GL_FRAMEBUFFER_BINDING,        &last_fbo);
    GLint  last_vp[4];          glGetIntegerv(GL_VIEWPORT,                   last_vp);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_depth   = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_blend   = glIsEnabled(GL_BLEND);

    // ImGui frame
    ImGuiIO& io     = ImGui::GetIO();
    io.DisplaySize  = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();

    drawmenu();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // restore GL state
    glUseProgram(last_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, last_tex0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, last_tex1);
    glActiveTexture(last_active_texture);
    glBindBuffer(GL_ARRAY_BUFFER,              last_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,      last_ebo);
    glBindFramebuffer(GL_FRAMEBUFFER,          last_fbo);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_depth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (last_blend)   glEnable(GL_BLEND);        else glDisable(GL_BLEND);
}

// ── eglSwapBuffers hook ───────────────────────────────────────────────────────

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

    if (g_targetcontext == EGL_NO_CONTEXT) {
        g_targetcontext = ctx;
        g_targetsurface = surf;
    }
    g_width  = w;
    g_height = h;

    setup();
    render();

    return orig_eglswapbuffers(dpy, surf);
}

// ── touch callback ────────────────────────────────────────────────────────────

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

    // notify Zaphkiel bridge (no-op currently, hook point for future use)
    static bool (*zaphkiel_touch)(int, float, float) = nullptr;
    if (!zaphkiel_touch)
        zaphkiel_touch = (bool(*)(int,float,float))dlsym(RTLD_DEFAULT, "zaphkiel_notify_touch");
    if (zaphkiel_touch)
        zaphkiel_touch(action, x, y);

    bool hitTest = false;
    {
        std::lock_guard<std::mutex> lock(g_boundsMutex);
        if (g_menuBounds.visible &&
            x >= g_menuBounds.x && x <= (g_menuBounds.x + g_menuBounds.w) &&
            y >= g_menuBounds.y && y <= (g_menuBounds.y + g_menuBounds.h))
            hitTest = true;
    }
    return hitTest || io.WantCaptureMouse;
}

// ── main thread + constructor ─────────────────────────────────────────────────

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
