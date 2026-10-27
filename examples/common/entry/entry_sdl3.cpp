/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include "entry_p.h"

#if ENTRY_CONFIG_USE_SDL3

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#undef main
#if BX_PLATFORM_WIIU
#include <SDL3/SDL_wiiu.h>
#include <bgfx/platform_gx2.h>
#endif
#include "entry_sdl3.h"
#include <unistd.h>

namespace entry
{
	struct Window
	{
		SDL_Window* window = NULL;
#if BX_PLATFORM_WIIU
		bgfx::Gx2PlatformData platform;
		Sint64 generation = -1;
#endif
		int32_t mouseX = 0;
		int32_t mouseY = 0;
		int32_t mouseZ = 0;
	};

	// SDL's single window mirrors both outputs. A second window owns the DRC.
	static Window s_windows[ENTRY_CONFIG_MAX_WINDOWS];
	static EventQueue s_events;
	static SDL_Gamepad* s_gamepads[ENTRY_CONFIG_MAX_GAMEPADS];
	// Remember the key chosen on press, so releasing ZL before X/Y still releases
	// the corresponding keyboard alias. Other GamePad buttons retain their keys.
	static Key::Enum s_faceKeys[ENTRY_CONFIG_MAX_GAMEPADS][2];
	static bool s_quit;

	static WindowHandle findWindow(SDL_WindowID _id)
	{
		for (uint16_t ii = 0; ii < BX_COUNTOF(s_windows); ++ii)
			if (s_windows[ii].window && SDL_GetWindowID(s_windows[ii].window) == _id) return { ii };
		return { UINT16_MAX };
	}

	static void updateWindowSize(WindowHandle _handle)
	{
		Window& win = s_windows[_handle.idx];
		int width, height; SDL_GetWindowSizeInPixels(win.window, &width, &height);
#if BX_PLATFORM_WIIU
		win.generation = SDL_GetNumberProperty(SDL_GetWindowProperties(win.window), SDL_PROP_WINDOW_WIIU_FOREGROUND_GENERATION_NUMBER, 0);
#endif
		s_events.postSizeEvent(_handle, width, height);
	}

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
		do
		{
			SDL_Event event;
			while (SDL_PollEvent(&event))
			{
				switch (event.type)
				{
				case SDL_EVENT_QUIT:
					s_quit = true;
					s_events.postExitEvent();
					break;
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
					{
						WindowHandle handle = findWindow(event.window.windowID);
						if (handle.idx == 0)
						{
							s_quit = true;
							s_events.postExitEvent();
						}
						else if (isValid(handle)) destroyWindow(handle);
					}
					break;
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					{
						WindowHandle handle = findWindow(event.window.windowID);
						if (isValid(handle)) updateWindowSize(handle);
					}
					break;
				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
					s_events.postKeyEvent(findWindow(event.key.windowID), translateKey(event.key.scancode), translateModifiers(event.key.mod), event.key.down);
					break;
				case SDL_EVENT_TEXT_INPUT:
					for (const char* str = event.text.text; *str;)
					{
						uint8_t ch[4] = {};
						uint8_t len = (*str & 0x80) == 0 ? 1 : (*str & 0xe0) == 0xc0 ? 2 : (*str & 0xf0) == 0xe0 ? 3 : 4;
						bx::memCopy(ch, str, len);
						s_events.postCharEvent(findWindow(event.text.windowID), len, ch);
						str += len;
					}
					break;
				case SDL_EVENT_MOUSE_MOTION:
					{
						WindowHandle handle = findWindow(event.motion.windowID);
						if (!isValid(handle)) break;
						Window& win = s_windows[handle.idx];
						win.mouseX = int32_t(event.motion.x);
						win.mouseY = int32_t(event.motion.y);
						s_events.postMouseEvent(handle, win.mouseX, win.mouseY, win.mouseZ);
					}
					break;
				case SDL_EVENT_MOUSE_WHEEL:
					{
						WindowHandle handle = findWindow(event.wheel.windowID);
						if (!isValid(handle)) break;
						Window& win = s_windows[handle.idx];
						win.mouseZ += int32_t(event.wheel.y);
						s_events.postMouseEvent(handle, win.mouseX, win.mouseY, win.mouseZ);
					}
					break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
					{
						WindowHandle handle = findWindow(event.button.windowID);
						if (!isValid(handle)) break;
						Window& win = s_windows[handle.idx];
						MouseButton::Enum button = event.button.button == SDL_BUTTON_LEFT ? MouseButton::Left
							: event.button.button == SDL_BUTTON_RIGHT ? MouseButton::Right
							: event.button.button == SDL_BUTTON_MIDDLE ? MouseButton::Middle : MouseButton::None;
						win.mouseX = int32_t(event.button.x);
						win.mouseY = int32_t(event.button.y);
						s_events.postMouseEvent(handle, win.mouseX, win.mouseY, win.mouseZ, button, event.button.down);
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
							for (Key::Enum& key : s_faceKeys[handle.idx])
							{
								s_events.postKeyEvent(kDefaultWindowHandle, key, 0, false);
								key = Key::None;
							}
							SDL_CloseGamepad(s_gamepads[handle.idx]);
							s_gamepads[handle.idx] = NULL;
							s_events.postGamepadEvent(kDefaultWindowHandle, handle, false);
						}
					}
					break;
				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				case SDL_EVENT_GAMEPAD_BUTTON_UP:
					{
						Key::Enum key = translateButton(event.gbutton.button);
						if (event.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH || event.gbutton.button == SDL_GAMEPAD_BUTTON_WEST)
						{
							const GamepadHandle handle = findGamepad(event.gbutton.which);
							if (!isValid(handle)) break;
							const uint32_t face = event.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH ? 0 : 1;
							Key::Enum& pressed = s_faceKeys[handle.idx][face];
							if (event.gbutton.down)
							{
								if (SDL_GetGamepadAxis(s_gamepads[handle.idx], SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384)
									key = face == 0 ? Key::KeyC : Key::KeyD;
								pressed = key;
							}
							else { key = pressed; pressed = Key::None; }
						}
						s_events.postKeyEvent(kDefaultWindowHandle, key, 0, event.gbutton.down);
					}
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
#if BX_PLATFORM_WIIU
			if (SDL_GetNumberProperty(SDL_GetWindowProperties(s_windows[0].window), SDL_PROP_WINDOW_WIIU_PRESENT_OUTPUT_MASK_NUMBER, 0) != 0) break;
			// Background entry waits until lifecycle handling permits GPU submission.
			// Keep pumping ProcUI without returning to an example's rendering loop.
			SDL_Delay(10);
#else
			break;
#endif
		} while (true);

#if BX_PLATFORM_WIIU
		for (uint16_t ii = 0; ii < BX_COUNTOF(s_windows); ++ii)
		{
			Window& win = s_windows[ii];
			if (NULL == win.window) continue;
			Sint64 generation = SDL_GetNumberProperty(SDL_GetWindowProperties(win.window), SDL_PROP_WINDOW_WIIU_FOREGROUND_GENERATION_NUMBER, 0);
			if (generation != win.generation) updateWindowSize({ ii });
		}
#endif
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
		BX_UNUSED(_flags);
		uint16_t idx;
		for (idx = 0; idx < BX_COUNTOF(s_windows); ++idx)
		{
			if (NULL == s_windows[idx].window) break;
		}
		if (idx == BX_COUNTOF(s_windows)) return { UINT16_MAX };
#if BX_PLATFORM_WIIU
		if (idx > 1) return { UINT16_MAX };
#endif

#if BX_PLATFORM_WIIU
		SDL_DisplayID display = SDL_GetPrimaryDisplay();
		if (idx != 0)
		{
			int count;
			SDL_DisplayID* displays = SDL_GetDisplays(&count);
			for (int ii = 0; ii < count; ++ii)
			{
				if (displays[ii] != SDL_GetDisplayForWindow(s_windows[0].window))
				{
					display = displays[ii];
					break;
				}
			}
			SDL_free(displays);
		}

#endif
		SDL_PropertiesID props = SDL_CreateProperties();
		SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, _title);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, _width);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, _height);
#if BX_PLATFORM_WIIU
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
		Window& win = s_windows[idx];
#else
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, _x);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, _y);
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
		Window& win = s_windows[idx];
#endif
#if BX_PLATFORM_WIIU
		win.platform.display = idx;
#endif
		win.window = SDL_CreateWindowWithProperties(props);
		SDL_DestroyProperties(props);
		if (NULL == win.window) return { UINT16_MAX };

#if BX_PLATFORM_WIIU
		win.generation = -1;
#endif
		win.mouseX = win.mouseY = win.mouseZ = 0;
		WindowHandle handle = { idx };
		updateWindowSize(handle);
		s_events.postWindowEvent(handle, getNativeWindowHandle(handle));
		return handle;
	}

	void destroyWindow(WindowHandle _handle)
	{
		Window& win = s_windows[_handle.idx];
		s_events.postWindowEvent(_handle);
		SDL_DestroyWindow(win.window);
		win.window = NULL;
	}
	void setWindowPos(WindowHandle _handle, int32_t _x, int32_t _y) { SDL_SetWindowPosition(s_windows[_handle.idx].window, _x, _y); }
	void setWindowSize(WindowHandle _handle, uint32_t _width, uint32_t _height)
	{
		SDL_SetWindowSize(s_windows[_handle.idx].window, int(_width), int(_height));
	}
	void setWindowTitle(WindowHandle _handle, const char* _title)
	{
		SDL_SetWindowTitle(s_windows[_handle.idx].window, _title);
	}
	void setWindowFlags(WindowHandle _handle, uint32_t _flags, bool _enabled) { BX_UNUSED(_handle, _flags, _enabled); }
	void toggleFullscreen(WindowHandle _handle)
	{
		SDL_Window* window = s_windows[_handle.idx].window;
		SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
	}
	void setMouseLock(WindowHandle _handle, bool _lock)
	{
		SDL_SetWindowRelativeMouseMode(s_windows[_handle.idx].window, _lock);
	}
	void* getNativeWindowHandle(WindowHandle _handle)
	{
		if (!isValid(_handle) || !s_windows[_handle.idx].window) return NULL;
#if BX_PLATFORM_WIIU
		return &s_windows[_handle.idx].platform;
#else
		SDL_PropertiesID props = SDL_GetWindowProperties(s_windows[_handle.idx].window);
		if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0)
			return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
		return (void*)(uintptr_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
#endif
	}
	void* getNativeDisplayHandle()
	{
#if BX_PLATFORM_WIIU
		return NULL;
#else
		SDL_PropertiesID props = SDL_GetWindowProperties(s_windows[0].window);
		return SDL_GetPointerProperty(props, SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0
			? SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER : SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
#endif
	}
	bgfx::NativeWindowHandleType::Enum getNativeWindowHandleType()
	{
		return SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0
			? bgfx::NativeWindowHandleType::Wayland : bgfx::NativeWindowHandleType::Default;
	}

} // namespace entry

#if BX_PLATFORM_WIIU
int SDL_main(int _argc, char** _argv)
#else
int main(int _argc, char** _argv)
#endif
{
	SDL_SetHint("SDL_VIDEO_EXTERNAL_GRAPHICS", "1");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
	{
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return 1;
	}
	SDL_AddEventWatch(entry::sdl3::lifecycle, NULL);
	entry::WindowHandle handle = entry::createWindow(0, 0, ENTRY_DEFAULT_WIDTH, ENTRY_DEFAULT_HEIGHT, ENTRY_WINDOW_FLAG_NONE, "bgfx example");
	if (!entry::isValid(handle))
	{
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		SDL_Quit();
		return 1;
	}
#if BX_PLATFORM_WIIU
	chdir("fs:/vol/external01/bgfx");
#endif
	int result = entry::main(_argc, _argv);
	for (SDL_Gamepad* gamepad : entry::s_gamepads) SDL_CloseGamepad(gamepad);
	for (entry::Window& win : entry::s_windows) SDL_DestroyWindow(win.window);
	SDL_RemoveEventWatch(entry::sdl3::lifecycle, NULL);
	SDL_Quit();
	return result;
}

#endif // ENTRY_CONFIG_USE_SDL3
