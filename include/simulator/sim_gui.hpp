#ifndef LIBNTR_SIM_GUI_HPP
#define LIBNTR_SIM_GUI_HPP

#include <nitro/types.h>
#include <SDL2/SDL.h>

namespace SIM::GUI {

void Init(SDL_Window * window, SDL_GLContext context);
void Main();
void NewFrame();
void ProcessEvent(SDL_Event * event);
void Render();
void Toggle();
bool IsGameLogicPaused();
void AppButton(const char * label, bool * state, void (*aInitFunc)(void), void (*aAppFunc)(bool *));



}


#endif
