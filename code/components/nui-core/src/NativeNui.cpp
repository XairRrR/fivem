/*
 * Native Linux NUI (proof of concept).
 *
 * When FiveM runs under Wine from the native Linux launcher, a Linux Chromium (fivem-nui-host,
 * outside Wine) can render web pages and publish its frames in a shared-memory file. The
 * launcher passes that file in CitizenFX_NativeNUIFrames (a Z:\dev\shm\... path, which Wine maps
 * shared with the Linux side); its frames are drawn over the game, after FiveM's own NUI windows.
 *
 * Input: while FiveM gives the NUI focus, mouse and keyboard events are also sent to the host,
 * over TCP 127.0.0.1 on the port it publishes in the frame file (fnui_input messages), from the
 * same two sources FiveM's CEF uses (CefInput.cpp: QueryInputTarget and OnWndProc). The game
 * thread never waits: events are dropped when the host is not connected.
 *
 * Without the variable, nothing changes.
 */

#include "StdInc.h"
#include "CefOverlay.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windowsx.h>

#include <mutex>

#pragma comment(lib, "ws2_32.lib")

extern "C"
{
#include "FiveMNuiShm.h"
}

extern nui::GameInterface* g_nuiGi;

// CefInput.cpp
int GetCefKeyboardModifiers(WPARAM wparam, LPARAM lparam);
int GetCefMouseModifiers(WPARAM wparam);
int GetCefMouseModifiers();

namespace
{
struct NativeFrames
{
	fnui_header* header = nullptr;
	uint64_t lastSeq = 0;
	fwRefContainer<nui::GITexture> texture;
	bool tried = false;
	bool logged = false;
};

NativeFrames g_native;

struct NativeInput
{
	std::mutex lock;
	SOCKET sock = INVALID_SOCKET;
	DWORD lastAttempt = 0;
	bool focus = false;
	int lastX = 0, lastY = 0;

	// double-click detection, as CefInput.cpp
	int clickCount = 0, clickX = 0, clickY = 0, clickButton = -1;
	LONG clickTime = 0;
};

NativeInput g_input;

bool NativeActive()
{
	return g_native.header && g_native.header->host_alive;
}

// on the render thread, retried every 2 s: a loopback connect answers at once
void ConnectInput()
{
	auto header = g_native.header;

	if (!header || g_input.sock != INVALID_SOCKET || !header->input_port || GetTickCount() - g_input.lastAttempt < 2000)
	{
		return;
	}

	g_input.lastAttempt = GetTickCount();

	static bool wsaStarted = false;

	if (!wsaStarted)
	{
		WSADATA wsa;
		wsaStarted = !WSAStartup(MAKEWORD(2, 2), &wsa);
	}

	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (sock == INVALID_SOCKET)
	{
		return;
	}

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(u_short(header->input_port));
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	BOOL one = TRUE;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

	if (connect(sock, (sockaddr*)&addr, sizeof(addr)))
	{
		closesocket(sock);
		return;
	}

	u_long nonBlocking = 1;
	ioctlsocket(sock, FIONBIO, &nonBlocking);

	std::unique_lock _(g_input.lock);
	g_input.sock = sock;
	g_input.focus = false;
	trace("Native NUI: input connected to port %d\n", header->input_port);
}

void SendInput(uint8_t type, uint8_t button, int modifiers, int x, int y, int a, int b)
{
	fnui_input in = { type, button, uint16_t(modifiers), x, y, a, b };

	std::unique_lock _(g_input.lock);

	if (g_input.sock == INVALID_SOCKET)
	{
		return;
	}

	int sent = send(g_input.sock, (const char*)&in, sizeof(in), 0);

	if (sent == sizeof(in) || (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK))
	{
		return;
	}

	// a partial message would shift the stream: reconnect
	closesocket(g_input.sock);
	g_input.sock = INVALID_SOCKET;
}

// game client pixels to host view pixels
void ScaleToHost(int* x, int* y)
{
	int resX, resY;
	g_nuiGi->GetGameResolution(&resX, &resY);

	if (resX > 0 && resY > 0)
	{
		*x = int(int64_t(*x) * g_native.header->max_width / resX);
		*y = int(int64_t(*y) * g_native.header->max_height / resY);
	}
}

void SendMouse(int button, int x, int y, bool down, int modifiers)
{
	ScaleToHost(&x, &y);
	g_input.lastX = x;
	g_input.lastY = y;

	if (button < 0)
	{
		SendInput(FNUI_INPUT_MOUSE_MOVE, 0, modifiers, x, y, 0, 0);
		return;
	}

	if (down)
	{
		LONG now = GetMessageTime();
		bool sameClick = button == g_input.clickButton && now - g_input.clickTime <= LONG(GetDoubleClickTime()) &&
			abs(g_input.clickX - x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
			abs(g_input.clickY - y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;

		g_input.clickCount = sameClick ? std::min(3, g_input.clickCount + 1) : 1;
		g_input.clickX = x;
		g_input.clickY = y;
		g_input.clickTime = now;
		g_input.clickButton = button;
	}

	SendInput(down ? FNUI_INPUT_MOUSE_DOWN : FNUI_INPUT_MOUSE_UP, uint8_t(button), modifiers, x, y, std::max(1, g_input.clickCount), 0);
}

// lParam of a key message, or a bare scan code, to scan code | 0x100 when extended
int KeyScanCode(UINT vKey, UINT scanOrLParam)
{
	if (scanOrLParam > 0xffff)
	{
		return ((scanOrLParam >> 16) & 0xff) | (((scanOrLParam >> 24) & 1) << 8);
	}

	if (scanOrLParam)
	{
		return scanOrLParam & 0x1ff;
	}

	UINT scan = MapVirtualKeyW(vKey, MAPVK_VK_TO_VSC_EX);
	return (scan & 0xff) | ((scan & 0xff00) ? 0x100 : 0);
}

void SendKey(int type, UINT vKey, UINT scanOrLParam)
{
	SendInput(FNUI_INPUT_KEY, uint8_t(type), GetCefKeyboardModifiers(vKey, scanOrLParam), 0, 0, int(vKey), KeyScanCode(vKey, scanOrLParam));
}

struct NativeInputTarget : InputTarget
{
	void KeyDown(UINT vKey, UINT scanCode) override
	{
		SendKey(KEYEVENT_RAWKEYDOWN, vKey, scanCode);
	}

	void KeyUp(UINT vKey, UINT scanCode) override
	{
		SendKey(KEYEVENT_KEYUP, vKey, scanCode);
	}

	void MouseDown(int buttonIdx, int x, int y) override
	{
		SendMouse(buttonIdx, x, y, true, GetCefMouseModifiers());
	}

	void MouseUp(int buttonIdx, int x, int y) override
	{
		SendMouse(buttonIdx, x, y, false, GetCefMouseModifiers());
	}

	void MouseMove(int x, int y) override
	{
		SendMouse(-1, x, y, true, GetCefMouseModifiers());
	}

	void MouseWheel(int delta) override
	{
		bool horizontal = GetKeyState(VK_SHIFT) & 0x8000;
		SendInput(FNUI_INPUT_MOUSE_WHEEL, 0, GetCefMouseModifiers(), g_input.lastX, g_input.lastY, horizontal ? delta * 120 : 0, horizontal ? 0 : delta * 120);
	}
} g_inputTarget;

void OnNativeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONDBLCLK:
		SendMouse((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? 0 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? 1 : 2,
			GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true, GetCefMouseModifiers(wParam));
		break;
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
		SendMouse(msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, GetCefMouseModifiers(wParam));
		break;
	case WM_MOUSEMOVE:
		SendMouse(-1, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true, GetCefMouseModifiers(wParam));
		break;
	case WM_MOUSELEAVE:
		SendInput(FNUI_INPUT_MOUSE_LEAVE, 0, GetCefMouseModifiers(), g_input.lastX, g_input.lastY, 0, 0);
		break;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
	{
		POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hWnd, &p);

		int x = p.x, y = p.y;
		ScaleToHost(&x, &y);

		int delta = GET_WHEEL_DELTA_WPARAM(wParam);
		bool horizontal = msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT);
		SendInput(FNUI_INPUT_MOUSE_WHEEL, 0, GetCefMouseModifiers(wParam), x, y, horizontal ? delta : 0, horizontal ? 0 : delta);
		break;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		SendKey(KEYEVENT_RAWKEYDOWN, UINT(wParam), UINT(lParam));
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		SendKey(KEYEVENT_KEYUP, UINT(wParam), UINT(lParam));
		break;
	case WM_CHAR:
	case WM_SYSCHAR:
	{
		int modifiers = GetCefKeyboardModifiers(wParam, lParam);

		// AltGr characters are not Ctrl+Alt shortcuts (CefInput.cpp: AlterKeyEventForAltGr)
		if ((GetKeyState(VK_RMENU) & 0x8000) && ((VkKeyScanExW(WCHAR(wParam), GetKeyboardLayout(0)) >> 8) & 0xff) == (2 | 4))
		{
			modifiers &= ~(EVENTFLAG_CONTROL_DOWN | EVENTFLAG_ALT_DOWN);
			modifiers |= EVENTFLAG_ALTGR_DOWN;
		}

		SendInput(FNUI_INPUT_KEY, KEYEVENT_CHAR, modifiers, int(wParam), int(wParam), int(wParam), KeyScanCode(0, UINT(lParam)));
		break;
	}
	}
}

// focus follows FiveM's (SET_NUI_FOCUS and the main menu)
void UpdateInputFocus()
{
	bool focus = nui::HasFocus();

	if (focus != g_input.focus)
	{
		g_input.focus = focus;
		SendInput(FNUI_INPUT_FOCUS, 0, 0, 0, 0, focus ? 1 : 0, 0);
	}
}

fnui_header* OpenNativeFrames()
{
	wchar_t path[MAX_PATH];
	DWORD length = GetEnvironmentVariableW(L"CitizenFX_NativeNUIFrames", path, std::size(path));

	if (!length || length >= std::size(path))
	{
		return nullptr;
	}

	HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

	if (file == INVALID_HANDLE_VALUE)
	{
		trace("Native NUI: cannot open %s (%d)\n", ToNarrow(path), GetLastError());
		return nullptr;
	}

	HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
	CloseHandle(file);

	if (!mapping)
	{
		trace("Native NUI: CreateFileMapping failed (%d)\n", GetLastError());
		return nullptr;
	}

	auto header = static_cast<fnui_header*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
	CloseHandle(mapping);

	if (!header || header->magic != FNUI_MAGIC || header->version != FNUI_VERSION)
	{
		trace("Native NUI: %s is not a fivem-nui-host frame file\n", ToNarrow(path));
		return nullptr;
	}

	trace("Native NUI: frames from host %d, %dx%d\n", header->host_pid, header->max_width, header->max_height);
	return header;
}

// Copies the latest complete frame; the host never writes the buffer marked as being read.
bool CopyLatestFrame(fnui_header* header, const nui::GILockedTexture& locked)
{
	uint32_t front;

	do
	{
		front = InterlockedCompareExchange((volatile LONG*)&header->front, 0, 0);

		if (front >= FNUI_BUFFERS)
		{
			return false;
		}

		InterlockedExchange((volatile LONG*)&header->reading, front);
	} while (InterlockedCompareExchange((volatile LONG*)&header->front, 0, 0) != front);

	// the header fields are volatile: copy them before std::min, which takes references
	const uint32_t frameWidth = header->width[front], frameHeight = header->height[front];
	const uint32_t maxWidth = locked.width ? uint32_t(locked.width) : uint32_t(header->max_width);
	const uint32_t maxHeight = locked.height ? uint32_t(locked.height) : uint32_t(header->max_height);
	uint32_t width = std::min(frameWidth, maxWidth);
	uint32_t height = std::min(frameHeight, maxHeight);
	const uint8_t* src = fnui_buffer(header, front);

	for (uint32_t y = 0; y < height; y++)
	{
		memcpy(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.pitch, src + size_t(y) * header->stride, size_t(width) * 4);
	}

	InterlockedExchange((volatile LONG*)&header->reading, FNUI_NONE);
	return true;
}

void RenderNativeFrames()
{
	if (!g_native.tried)
	{
		g_native.tried = true;
		g_native.header = OpenNativeFrames();
	}

	auto header = g_native.header;

	if (!header || !header->host_alive)
	{
		return;
	}

	ConnectInput();
	UpdateInputFocus();

	if (header->front >= FNUI_BUFFERS)
	{
		return;
	}

	if (!g_native.texture.GetRef())
	{
		// materialized on the render queue: drawn from the next frames on
		g_native.texture = g_nuiGi->CreateTextureBacking(header->max_width, header->max_height, nui::GITextureFormat::ARGB);
		return;
	}

	if (!g_native.texture->GetNativeTexture())
	{
		return;
	}

	uint64_t seq = header->seq;

	if (seq != g_native.lastSeq)
	{
		nui::GILockedTexture locked = {};

		if (g_native.texture->Map(0, 0, &locked, nui::GILockFlags::WriteDiscard))
		{
			if (CopyLatestFrame(header, locked))
			{
				g_native.lastSeq = seq;
			}

			g_native.texture->Unmap(&locked);
		}

		if (!g_native.logged)
		{
			g_native.logged = true;
			trace("Native NUI: first frame drawn (pitch %d)\n", locked.pitch);
		}
	}

	int resX, resY;
	g_nuiGi->GetGameResolution(&resX, &resY);

	nui::ResultingRectangle rr;
	rr.color = CRGBA(0xff, 0xff, 0xff, 0xff);
	rr.rectangle = CRect(0, 0, resX, resY);

	g_nuiGi->SetTexture(g_native.texture, true);
	g_nuiGi->DrawRectangles(1, &rr);
	g_nuiGi->UnsetTexture();
}
}

static HookFunction initFunction([]()
{
	// after FiveM's own windows (NUIRenderCallbacks.cpp, order 0)
	g_nuiGi->OnRender.Connect([]()
	{
		RenderNativeFrames();
	},
	100);

	// before CefInput.cpp's handlers (order 0), which stop the chain or the message when focused
	g_nuiGi->QueryInputTarget.Connect([](std::vector<InputTarget*>& targets)
	{
		if (NativeActive() && nui::HasFocus())
		{
			targets.push_back(&g_inputTarget);
		}

		return true;
	},
	-100);

	g_nuiGi->OnWndProc.Connect([](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, bool& pass, LRESULT& lresult)
	{
		if (pass && NativeActive() && nui::HasFocus())
		{
			OnNativeWndProc(hWnd, msg, wParam, lParam);
		}
	},
	-100);
});
