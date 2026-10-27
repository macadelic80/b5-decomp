#pragma once

// FLAG PC divergence: the window's messages as the portable brn_input_event the WebAssembly mods
// read (pc/mods/api/brn_mod_api.h). The only Win32-aware piece of the wasm input path: a port
// to another windowing layer replaces this file and nothing in the mods.

#include "pc/mods/ModInternal.h"

namespace CgsPC { namespace Mods
{
    class InputTranslator
    {
    public:
        // Render thread. lfScaleX/Y: window client pixels -> back buffer pixels.
        void Translate(const BrnWindowMessage& lrMessage, float lfScaleX, float lfScaleY,
                       std::vector<brn_input_event>& lrOut);

    private:
        uint32_t muHighSurrogate = 0;   // WM_CHAR delivers UTF-16: the first half of a pair
    };
} }
