#include "simulator/sim.h"
#include "simulator/sim_gui.hpp"

#include "simulator/imgui/imgui.hpp"

#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "gui_internal.hpp"

namespace SIM::GUI {
static SDL_Window * sWindow;
static SDL_GLContext sContext;
static ImGuiContext * sImguiContext;
static bool sEnabled = false;
static bool sPauseGameLogic = false;
static constexpr ImVec2 sButtonSize = {100, 20};

static bool sShowAppConfig = false;

void Init(SDL_Window * window, SDL_GLContext context) {
    sWindow = window;
    sContext = context;

    sImguiContext = ImGui::CreateContext(NULL);

    ImGuiIO igIO = ImGui::GetIO();
	igIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
	igIO.DisplaySize.x = (float)(SIM_NDS_SCREEN_WIDTH*SIM_NDS_WINDOW_SCALE_FACTOR);
	igIO.DisplaySize.y = (float)(SIM_NDS_SCREEN_HEIGHT*2*SIM_NDS_WINDOW_SCALE_FACTOR);



	ImGui_ImplSDL2_InitForOpenGL(sWindow, sContext);
	const char * glsl_version = "#version 420";
  	ImGui_ImplOpenGL3_Init(glsl_version);

    ImGui::StyleColorsDark(NULL);

    //ImFontAtlas::ImFontAtlas_AddFontDefault
    //ioptr->Fonts.AddFontDefault(NULL);
}

void Main() {
    if(sEnabled) {
        ImGui::Begin("libntr", &sEnabled);

        AppButton("Config", &sShowAppConfig, nullptr, AppConfigMain);

        ImGui::End();
    }
}

void NewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void ProcessEvent(SDL_Event * event) {
    if(sEnabled) {
        ImGui_ImplSDL2_ProcessEvent(event);
    }
}

void Render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Toggle() {
    if(sEnabled) {
        sEnabled = false;
    } else {
        sEnabled = true;
    }
}

bool IsGameLogicPaused() {
    return sPauseGameLogic;
}

// Generic button to open another GUI applet
void AppButton(const char * label, bool * state, void (*initFunc)(void), void (*appFunc)(bool *)) {
    if(ImGui::Button(label, sButtonSize)) {
        if(*state) {
            *state = false;
        } else {
            if(initFunc) {
                initFunc();
            }
            *state = true;
        }
    }

    if(*state) {
        appFunc(state);
    }
}

}