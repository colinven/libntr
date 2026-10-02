#include "simulator/sim.h"
#include "simulator/imgui/imgui.hpp"
#include "simulator/sim_RomExtractor.h"
#include "simulator/sim_gui.hpp"

#include <SDL2/SDL.h>
#include <string>


void SIM_ShowRomExtractionDialog() {
    bool fileSelected = false;
    SIM::GUI::Enable();
    SIM::GUI::OpenFileDialog([&fileSelected](std::string fileName) {
        fileSelected = true;

        // TODO: Hook up a ROM extraction library here
        char messageBuf[200] = {0};
        snprintf(messageBuf, 199, "Selected %s", fileName.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FS - ROM Extraction Required", messageBuf, NULL);
    });
    while(!fileSelected) {
        SIM_Render(nullptr);
    }


    int foo = 1;
    foo++;
    printf("%d", foo);
}