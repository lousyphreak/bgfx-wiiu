/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include "entry_p.h"

#if BX_PLATFORM_WIIU

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#undef main
#include <SDL3/SDL_wiiu.h>
#include <bgfx/platform_gx2.h>
#include <gx2/surface.h>
#include <unistd.h>

namespace entry
{
	static SDL_Window* s_window;
	static EventQueue s_events;
	static SDL_Gamepad* s_gamepads[ENTRY_CONFIG_MAX_GAMEPADS];
	static Sint64 s_generation = -1;
	static int32_t s_mouseX;
	static int32_t s_mouseY;
	static int32_t s_mouseZ;
	static bool s_quit;

	static bool queryTarget(void* _userData, bgfx::Gx2Target* _target)
	{
		SDL_PropertiesID props = SDL_GetWindowProperties(static_cast<SDL_Window*>(_userData));
		Uint32 outputs = Uint32(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_WIIU_PRESENT_OUTPUT_MASK_NUMBER, 0));
		*_target = {};
		if (outputs & SDL_WIIU_PRESENT_OUTPUT_TV)
		{
			_target->color = static_cast<GX2ColorBuffer*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIIU_TV_COLOR_BUFFER_POINTER, NULL));
			_target->context = static_cast<GX2ContextState*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIIU_TV_CONTEXT_STATE_POINTER, NULL));
		}
		else if (outputs & SDL_WIIU_PRESENT_OUTPUT_DRC)
		{
			_target->color = static_cast<GX2ColorBuffer*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIIU_DRC_COLOR_BUFFER_POINTER, NULL));
			_target->context = static_cast<GX2ContextState*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIIU_DRC_CONTEXT_STATE_POINTER, NULL));
		}
		return NULL != _target->color && NULL != _target->context;
	}

	static void present(void* _userData)
	{
		SDL_WIIU_SwapWindow(static_cast<SDL_Window*>(_userData));
	}

	static bgfx::Gx2PlatformData s_platform = { NULL, queryTarget, present };

	static Key::Enum translateKey(SDL_Scancode _key)
	{
		if (_key >= SDL_SCANCODE_A && _key <= SDL_SCANCODE_Z)
		{
			return Key::Enum(int(Key::KeyA) + int(_key) - int(SDL_SCANCODE_A));
		}
		if (_key >= SDL_SCANCODE_1 && _key <= SDL_SCANCODE_9)
		{
			return Key::Enum(int(Key::Key1) + int(_key) - int(SDL_SCANCODE_1));
		}
		if (_key >= SDL_SCANCODE_F1 && _key <= SDL_SCANCODE_F12)
		{
			return Key::Enum(int(Key::F1) + int(_key) - int(SDL_SCANCODE_F1));
		}
		switch (_key)
		{
		case SDL_SCANCODE_0: return Key::Key0;
		case SDL_SCANCODE_ESCAPE: return Key::Esc;
		case SDL_SCANCODE_RETURN: return Key::Return;
		case SDL_SCANCODE_TAB: return Key::Tab;
		case SDL_SCANCODE_SPACE: return Key::Space;
		case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
		case SDL_SCANCODE_UP: return Key::Up;
		case SDL_SCANCODE_DOWN: return Key::Down;
		case SDL_SCANCODE_LEFT: return Key::Left;
		case SDL_SCANCODE_RIGHT: return Key::Right;
		case SDL_SCANCODE_INSERT: return Key::Insert;
		case SDL_SCANCODE_DELETE: return Key::Delete;
		case SDL_SCANCODE_HOME: return Key::Home;
		case SDL_SCANCODE_END: return Key::End;
		case SDL_SCANCODE_PAGEUP: return Key::PageUp;
		case SDL_SCANCODE_PAGEDOWN: return Key::PageDown;
		case SDL_SCANCODE_PRINTSCREEN: return Key::Print;
		case SDL_SCANCODE_EQUALS: return Key::Plus;
		case SDL_SCANCODE_MINUS: return Key::Minus;
		case SDL_SCANCODE_LEFTBRACKET: return Key::LeftBracket;
		case SDL_SCANCODE_RIGHTBRACKET: return Key::RightBracket;
		case SDL_SCANCODE_SEMICOLON: return Key::Semicolon;
		case SDL_SCANCODE_APOSTROPHE: return Key::Quote;
		case SDL_SCANCODE_COMMA: return Key::Comma;
		case SDL_SCANCODE_PERIOD: return Key::Period;
		case SDL_SCANCODE_SLASH: return Key::Slash;
		case SDL_SCANCODE_BACKSLASH: return Key::Backslash;
		case SDL_SCANCODE_GRAVE: return Key::Tilde;
		default: return Key::None;
		}
	}

	static uint8_t translateModifiers(SDL_Keymod _mod)
	{
		return (_mod & SDL_KMOD_LALT   ? Modifier::LeftAlt    : 0)
			|  (_mod & SDL_KMOD_RALT   ? Modifier::RightAlt   : 0)
			|  (_mod & SDL_KMOD_LCTRL  ? Modifier::LeftCtrl   : 0)
			|  (_mod & SDL_KMOD_RCTRL  ? Modifier::RightCtrl  : 0)
			|  (_mod & SDL_KMOD_LSHIFT ? Modifier::LeftShift : 0)
			|  (_mod & SDL_KMOD_RSHIFT ? Modifier::RightShift : 0)
			|  (_mod & SDL_KMOD_LGUI   ? Modifier::LeftMeta   : 0)
			|  (_mod & SDL_KMOD_RGUI   ? Modifier::RightMeta  : 0);
	}

	static Key::Enum translateButton(Uint8 _button)
	{
		switch (_button)
		{
		case SDL_GAMEPAD_BUTTON_SOUTH: return Key::GamepadA;
		case SDL_GAMEPAD_BUTTON_EAST: return Key::GamepadB;
		case SDL_GAMEPAD_BUTTON_WEST: return Key::GamepadX;
		case SDL_GAMEPAD_BUTTON_NORTH: return Key::GamepadY;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return Key::GamepadThumbL;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return Key::GamepadThumbR;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Key::GamepadShoulderL;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Key::GamepadShoulderR;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return Key::GamepadUp;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Key::GamepadDown;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Key::GamepadLeft;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Key::GamepadRight;
		case SDL_GAMEPAD_BUTTON_BACK: return Key::GamepadBack;
		case SDL_GAMEPAD_BUTTON_START: return Key::GamepadStart;
		case SDL_GAMEPAD_BUTTON_GUIDE: return Key::GamepadGuide;
		default: return Key::None;
		}
	}

	static GamepadHandle findGamepad(SDL_JoystickID _id)
	{
		for (uint16_t ii = 0; ii < ENTRY_CONFIG_MAX_GAMEPADS; ++ii)
		{
			if (NULL != s_gamepads[ii] && SDL_GetGamepadID(s_gamepads[ii]) == _id)
			{
				return { ii };
			}
		}
		return { UINT16_MAX };
	}

	static void pumpEvents()
	{
		bgfx::Gx2Target target;
		do
		{
			SDL_Event event;
			while (SDL_PollEvent(&event))
			{
				switch (event.type)
				{
				case SDL_EVENT_QUIT:
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
					s_quit = true;
					s_events.postExitEvent();
					break;
				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
					s_events.postKeyEvent(kDefaultWindowHandle, translateKey(event.key.scancode), translateModifiers(event.key.mod), event.key.down);
					break;
				case SDL_EVENT_TEXT_INPUT:
					for (const char* str = event.text.text; *str;)
					{
						uint8_t ch[4] = {};
						uint8_t len = (*str & 0x80) == 0 ? 1 : (*str & 0xe0) == 0xc0 ? 2 : (*str & 0xf0) == 0xe0 ? 3 : 4;
						bx::memCopy(ch, str, len);
						s_events.postCharEvent(kDefaultWindowHandle, len, ch);
						str += len;
					}
					break;
				case SDL_EVENT_MOUSE_MOTION:
					s_mouseX = int32_t(event.motion.x);
					s_mouseY = int32_t(event.motion.y);
					s_events.postMouseEvent(kDefaultWindowHandle, s_mouseX, s_mouseY, s_mouseZ);
					break;
				case SDL_EVENT_MOUSE_WHEEL:
					s_mouseZ += int32_t(event.wheel.y);
					s_events.postMouseEvent(kDefaultWindowHandle, s_mouseX, s_mouseY, s_mouseZ);
					break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
					{
						MouseButton::Enum button = event.button.button == SDL_BUTTON_LEFT ? MouseButton::Left
							: event.button.button == SDL_BUTTON_RIGHT ? MouseButton::Right
							: event.button.button == SDL_BUTTON_MIDDLE ? MouseButton::Middle : MouseButton::None;
						s_events.postMouseEvent(kDefaultWindowHandle, int32_t(event.button.x), int32_t(event.button.y), s_mouseZ, button, event.button.down);
					}
					break;
				case SDL_EVENT_GAMEPAD_ADDED:
					for (uint16_t ii = 0; ii < ENTRY_CONFIG_MAX_GAMEPADS; ++ii)
					{
						if (NULL == s_gamepads[ii])
						{
							s_gamepads[ii] = SDL_OpenGamepad(event.gdevice.which);
							if (NULL != s_gamepads[ii]) s_events.postGamepadEvent(kDefaultWindowHandle, { ii }, true);
							break;
						}
					}
					break;
				case SDL_EVENT_GAMEPAD_REMOVED:
					{
						GamepadHandle handle = findGamepad(event.gdevice.which);
						if (isValid(handle))
						{
							SDL_CloseGamepad(s_gamepads[handle.idx]);
							s_gamepads[handle.idx] = NULL;
							s_events.postGamepadEvent(kDefaultWindowHandle, handle, false);
						}
					}
					break;
				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				case SDL_EVENT_GAMEPAD_BUTTON_UP:
					s_events.postKeyEvent(kDefaultWindowHandle, translateButton(event.gbutton.button), 0, event.gbutton.down);
					break;
				case SDL_EVENT_GAMEPAD_AXIS_MOTION:
					{
						static const GamepadAxis::Enum axes[] = { GamepadAxis::LeftX, GamepadAxis::LeftY, GamepadAxis::RightX, GamepadAxis::RightY, GamepadAxis::LeftZ, GamepadAxis::RightZ };
						GamepadHandle handle = findGamepad(event.gaxis.which);
						if (isValid(handle)) s_events.postAxisEvent(kDefaultWindowHandle, handle, axes[event.gaxis.axis], event.gaxis.value);
					}
					break;
				default: break;
				}
			}

			if (s_quit) return;
			if (queryTarget(s_window, &target)) break;
			// SDL releases its borrowed GX2 targets while in the background.
			// Keep pumping ProcUI without returning to an example's rendering loop.
			SDL_Delay(10);
		} while (true);

		Sint64 generation = SDL_GetNumberProperty(SDL_GetWindowProperties(s_window), SDL_PROP_WINDOW_WIIU_FOREGROUND_GENERATION_NUMBER, 0);
		if (generation != s_generation)
		{
			s_generation = generation;
			s_events.postSizeEvent(kDefaultWindowHandle, target.color->surface.width, target.color->surface.height);
		}
	}

	const Event* poll()
	{
		pumpEvents();
		return s_events.poll();
	}

	const Event* poll(WindowHandle _handle)
	{
		pumpEvents();
		return s_events.poll(_handle);
	}

	void release(const Event* _event) { s_events.release(_event); }

	WindowHandle createWindow(int32_t _x, int32_t _y, uint32_t _width, uint32_t _height, uint32_t _flags, const char* _title)
	{
		BX_UNUSED(_x, _y, _width, _height, _flags, _title);
		return { UINT16_MAX };
	}
	void destroyWindow(WindowHandle _handle) { BX_UNUSED(_handle); }
	void setWindowPos(WindowHandle _handle, int32_t _x, int32_t _y) { BX_UNUSED(_handle, _x, _y); }
	void setWindowSize(WindowHandle _handle, uint32_t _width, uint32_t _height)
	{
		BX_UNUSED(_handle);
		SDL_SetWindowSize(s_window, int(_width), int(_height));
	}
	void setWindowTitle(WindowHandle _handle, const char* _title)
	{
		BX_UNUSED(_handle);
		SDL_SetWindowTitle(s_window, _title);
	}
	void setWindowFlags(WindowHandle _handle, uint32_t _flags, bool _enabled) { BX_UNUSED(_handle, _flags, _enabled); }
	void toggleFullscreen(WindowHandle _handle) { BX_UNUSED(_handle); }
	void setMouseLock(WindowHandle _handle, bool _lock)
	{
		BX_UNUSED(_handle);
		SDL_SetWindowRelativeMouseMode(s_window, _lock);
	}
	void* getNativeWindowHandle(WindowHandle _handle)
	{
		return _handle.idx == 0 ? &s_platform : NULL;
	}
	void* getNativeDisplayHandle() { return NULL; }
	bgfx::NativeWindowHandleType::Enum getNativeWindowHandleType() { return bgfx::NativeWindowHandleType::Default; }

} // namespace entry

int SDL_main(int _argc, char** _argv)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
	{
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return 1;
	}
	entry::s_window = SDL_CreateWindow("bgfx example", ENTRY_DEFAULT_WIDTH, ENTRY_DEFAULT_HEIGHT, 0);
	if (NULL == entry::s_window)
	{
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	entry::s_platform.userData = entry::s_window;
	chdir("fs:/vol/content");
	entry::s_events.postWindowEvent(entry::kDefaultWindowHandle, &entry::s_platform);
	int result = entry::main(_argc, _argv);
	for (SDL_Gamepad* gamepad : entry::s_gamepads) SDL_CloseGamepad(gamepad);
	SDL_DestroyWindow(entry::s_window);
	SDL_Quit();
	return result;
}

#endif // BX_PLATFORM_WIIU
