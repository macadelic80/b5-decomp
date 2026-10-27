// FLAG PC divergence: Win32 window messages -> brn_input_event (see ModInput.h).

#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModInput.h"

#include <Windows.h>
#include <windowsx.h>

namespace CgsPC { namespace Mods
{
    namespace
    {
        int32_t KeyCode(WPARAM luVirtualKey, LPARAM liLParam)
        {
            const bool lbExtended = (liLParam & (1 << 24)) != 0;
            if (luVirtualKey >= 'A' && luVirtualKey <= 'Z') return static_cast<int32_t>(luVirtualKey);
            if (luVirtualKey >= '0' && luVirtualKey <= '9') return static_cast<int32_t>(luVirtualKey);
            if (luVirtualKey >= VK_F1 && luVirtualKey <= VK_F12) return BRN_KEY_F1 + static_cast<int32_t>(luVirtualKey - VK_F1);
            if (luVirtualKey >= VK_NUMPAD0 && luVirtualKey <= VK_NUMPAD9) return BRN_KEY_KP_0 + static_cast<int32_t>(luVirtualKey - VK_NUMPAD0);
            switch (luVirtualKey)
            {
            case VK_SPACE:      return BRN_KEY_SPACE;
            case VK_OEM_7:      return BRN_KEY_APOSTROPHE;
            case VK_OEM_COMMA:  return BRN_KEY_COMMA;
            case VK_OEM_MINUS:  return BRN_KEY_MINUS;
            case VK_OEM_PERIOD: return BRN_KEY_PERIOD;
            case VK_OEM_2:      return BRN_KEY_SLASH;
            case VK_OEM_1:      return BRN_KEY_SEMICOLON;
            case VK_OEM_PLUS:   return BRN_KEY_EQUAL;
            case VK_OEM_4:      return BRN_KEY_LEFT_BRACKET;
            case VK_OEM_5:      return BRN_KEY_BACKSLASH;
            case VK_OEM_6:      return BRN_KEY_RIGHT_BRACKET;
            case VK_OEM_3:      return BRN_KEY_GRAVE;
            case VK_ESCAPE:     return BRN_KEY_ESCAPE;
            case VK_RETURN:     return lbExtended ? BRN_KEY_KP_ENTER : BRN_KEY_ENTER;
            case VK_TAB:        return BRN_KEY_TAB;
            case VK_BACK:       return BRN_KEY_BACKSPACE;
            case VK_INSERT:     return BRN_KEY_INSERT;
            case VK_DELETE:     return BRN_KEY_DELETE;
            case VK_RIGHT:      return BRN_KEY_RIGHT;
            case VK_LEFT:       return BRN_KEY_LEFT;
            case VK_DOWN:       return BRN_KEY_DOWN;
            case VK_UP:         return BRN_KEY_UP;
            case VK_PRIOR:      return BRN_KEY_PAGE_UP;
            case VK_NEXT:       return BRN_KEY_PAGE_DOWN;
            case VK_HOME:       return BRN_KEY_HOME;
            case VK_END:        return BRN_KEY_END;
            case VK_CAPITAL:    return BRN_KEY_CAPS_LOCK;
            case VK_SCROLL:     return BRN_KEY_SCROLL_LOCK;
            case VK_NUMLOCK:    return BRN_KEY_NUM_LOCK;
            case VK_SNAPSHOT:   return BRN_KEY_PRINT_SCREEN;
            case VK_PAUSE:      return BRN_KEY_PAUSE;
            case VK_DECIMAL:    return BRN_KEY_KP_DECIMAL;
            case VK_DIVIDE:     return BRN_KEY_KP_DIVIDE;
            case VK_MULTIPLY:   return BRN_KEY_KP_MULTIPLY;
            case VK_SUBTRACT:   return BRN_KEY_KP_SUBTRACT;
            case VK_ADD:        return BRN_KEY_KP_ADD;
            case VK_SHIFT:      // left/right by scan code: 0x36 is the right shift
                return ((liLParam >> 16) & 0xFF) == 0x36 ? BRN_KEY_RIGHT_SHIFT : BRN_KEY_LEFT_SHIFT;
            case VK_CONTROL:    return lbExtended ? BRN_KEY_RIGHT_CTRL : BRN_KEY_LEFT_CTRL;
            case VK_MENU:       return lbExtended ? BRN_KEY_RIGHT_ALT : BRN_KEY_LEFT_ALT;
            case VK_LWIN:       return BRN_KEY_LEFT_SUPER;
            case VK_RWIN:       return BRN_KEY_RIGHT_SUPER;
            case VK_APPS:       return BRN_KEY_MENU;
            default:            return BRN_KEY_UNKNOWN;
            }
        }

        brn_input_event Event(uint32_t luType, uint32_t luModifiers)
        {
            brn_input_event lEvent = {};
            lEvent.type = luType;
            lEvent.modifiers = luModifiers;
            return lEvent;
        }
    }

    void InputTranslator::Translate(const BrnWindowMessage& lrMessage, float lfScaleX, float lfScaleY,
                                    std::vector<brn_input_event>& lrOut)
    {
        const WPARAM luWParam = static_cast<WPARAM>(lrMessage.wparam);
        const LPARAM liLParam = static_cast<LPARAM>(lrMessage.lparam);
        const uint32_t luModifiers = lrMessage.modifiers;
        auto lMouse = [&](uint32_t luType) {
            brn_input_event lEvent = Event(luType, luModifiers);
            lEvent.x = static_cast<float>(GET_X_LPARAM(liLParam)) * lfScaleX;
            lEvent.y = static_cast<float>(GET_Y_LPARAM(liLParam)) * lfScaleY;
            return lEvent;
        };
        auto lButton = [&](int32_t liButton, bool lbPressed) {
            brn_input_event lEvent = lMouse(BRN_INPUT_MOUSE_BUTTON);
            lEvent.code = liButton;
            lEvent.pressed = lbPressed ? 1 : 0;
            lrOut.push_back(lEvent);
        };
        switch (lrMessage.msg)
        {
        case WM_MOUSEMOVE:   lrOut.push_back(lMouse(BRN_INPUT_MOUSE_MOVE)); break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: lButton(BRN_MOUSE_LEFT, true); break;
        case WM_LBUTTONUP:   lButton(BRN_MOUSE_LEFT, false); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: lButton(BRN_MOUSE_RIGHT, true); break;
        case WM_RBUTTONUP:   lButton(BRN_MOUSE_RIGHT, false); break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: lButton(BRN_MOUSE_MIDDLE, true); break;
        case WM_MBUTTONUP:   lButton(BRN_MOUSE_MIDDLE, false); break;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        case WM_XBUTTONUP:
            lButton(GET_XBUTTON_WPARAM(luWParam) == XBUTTON1 ? BRN_MOUSE_X1 : BRN_MOUSE_X2, lrMessage.msg != WM_XBUTTONUP);
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            const float lfNotches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(luWParam)) / WHEEL_DELTA;
            brn_input_event lEvent = Event(BRN_INPUT_MOUSE_WHEEL, luModifiers);
            (lrMessage.msg == WM_MOUSEWHEEL ? lEvent.y : lEvent.x) = lfNotches;
            lrOut.push_back(lEvent);
            break;
        }
        case WM_MOUSELEAVE:  lrOut.push_back(Event(BRN_INPUT_MOUSE_LEAVE, luModifiers)); break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
        case WM_KEYUP:   case WM_SYSKEYUP:
        {
            brn_input_event lEvent = Event(BRN_INPUT_KEY, luModifiers);
            lEvent.code = KeyCode(luWParam, liLParam);
            lEvent.pressed = (lrMessage.msg == WM_KEYDOWN || lrMessage.msg == WM_SYSKEYDOWN) ? 1 : 0;
            lEvent.repeat = (lEvent.pressed && (liLParam & (1 << 30)) != 0) ? 1 : 0;   // previous key state: down
            if (lEvent.code != BRN_KEY_UNKNOWN) lrOut.push_back(lEvent);
            break;
        }
        case WM_CHAR: case WM_SYSCHAR:
        {
            const uint32_t luUnit = static_cast<uint32_t>(luWParam) & 0xFFFFu;
            uint32_t luCodePoint = luUnit;
            if (luUnit >= 0xD800 && luUnit <= 0xDBFF) { muHighSurrogate = luUnit; break; }
            if (luUnit >= 0xDC00 && luUnit <= 0xDFFF)
            {
                if (muHighSurrogate == 0) break;
                luCodePoint = 0x10000u + ((muHighSurrogate - 0xD800u) << 10) + (luUnit - 0xDC00u);
            }
            muHighSurrogate = 0;
            if (luCodePoint < 0x20 || luCodePoint == 0x7F) break;   // control keys arrive as BRN_INPUT_KEY
            brn_input_event lEvent = Event(BRN_INPUT_TEXT, luModifiers);
            lEvent.code = static_cast<int32_t>(luCodePoint);
            lrOut.push_back(lEvent);
            break;
        }
        case WM_SETFOCUS: case WM_KILLFOCUS:
        {
            brn_input_event lEvent = Event(BRN_INPUT_FOCUS, luModifiers);
            lEvent.pressed = lrMessage.msg == WM_SETFOCUS ? 1 : 0;
            lrOut.push_back(lEvent);
            break;
        }
        case WM_ACTIVATEAPP:
        {
            brn_input_event lEvent = Event(BRN_INPUT_FOCUS, luModifiers);
            lEvent.pressed = luWParam != 0 ? 1 : 0;
            lrOut.push_back(lEvent);
            break;
        }
        default: break;   // WM_SIZE: the host sends BRN_INPUT_RESIZE from the back buffer size
        }
    }
} }

#endif // BRN_PC_MODS
