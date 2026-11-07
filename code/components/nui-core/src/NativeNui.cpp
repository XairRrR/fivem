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
#include "NativeNuiBridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>

#include <mutex>
#include <vector>

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
	// the texture's D3D11 usage allows updating a part of it (UpdateSubresource): only the
	// rectangles the host changed are uploaded, else each new frame is copied whole (Map)
	int inPlace = -1;	// -1 not checked yet
	// the texture the game draws, when shared with it (default usage: updatable in part)
	Microsoft::WRL::ComPtr<ID3D11Texture2D> shared;
	HANDLE sharedHandle = nullptr;
	uint64_t uploaded = 0, frames = 0;
	DWORD statsTime = 0;
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

// The part of the view frames (lastSeq, seq] changed (header dirty ring), or all of it when an
// entry is missing (first frame, overwritten, host without dirty rectangles)
RECT ChangedSince(fnui_header* header, uint64_t lastSeq, uint64_t seq, uint32_t width, uint32_t height)
{
	RECT all = { 0, 0, LONG(width), LONG(height) };

	if (!lastSeq || header->dirty_ring != FNUI_DIRTY_RING || seq - lastSeq >= FNUI_DIRTY_RING)
	{
		return all;
	}

	RECT changed = { 0, 0, 0, 0 };
	for (uint64_t s = lastSeq + 1; s <= seq; s++)
	{
		const volatile fnui_dirty& entry = header->dirty[s % FNUI_DIRTY_RING];
		uint64_t entrySeq = InterlockedCompareExchange64((volatile LONG64*)&entry.seq, 0, 0);

		if (entrySeq != s)
		{
			return all;
		}

		const int32_t x = entry.x, y = entry.y, w = entry.w, h = entry.h;
		if (w <= 0 || h <= 0)
		{
			continue;
		}

		RECT r = { x, y, x + w, y + h };
		if (changed.right <= changed.left)
		{
			changed = r;
		}
		else
		{
			UnionRect(&changed, &changed, &r);
		}
	}

	IntersectRect(&changed, &changed, &all);
	return changed;
}

// The rectangles frames (lastSeq, seq] changed (header dirty_rects ring), merged down to a few:
// false when the host does not give them or an entry is missing (then ChangedSince decides)
bool ChangedRects(fnui_header* header, uint64_t lastSeq, uint64_t seq, uint32_t width, uint32_t height, std::vector<RECT>& out)
{
	out.clear();
	if (!lastSeq || header->rects_ring != FNUI_RECTS_RING || seq - lastSeq >= FNUI_RECTS_RING)
	{
		return false;
	}

	RECT bounds = { 0, 0, LONG(width), LONG(height) };
	for (uint64_t s = lastSeq + 1; s <= seq; s++)
	{
		const volatile fnui_rects& entry = header->dirty_rects[s % FNUI_RECTS_RING];
		if (uint64_t(InterlockedCompareExchange64((volatile LONG64*)&entry.seq, 0, 0)) != s)
		{
			return false;
		}

		uint32_t count = std::min<uint32_t>(entry.count, FNUI_RECTS_MAX);
		for (uint32_t i = 0; i < count; i++)
		{
			const int32_t x = entry.rects[i].x, y = entry.rects[i].y, w = entry.rects[i].w, h = entry.rects[i].h;
			RECT r = { x, y, x + w, y + h };
			if (w > 0 && h > 0 && IntersectRect(&r, &r, &bounds))
			{
				out.push_back(r);
			}
		}
	}

	// the frames' rectangles, merged while there are more than a few uploads: the pair whose union
	// adds the least area first
	auto area = [](const RECT& r)
	{
		return int64_t(r.right - r.left) * (r.bottom - r.top);
	};
	while (out.size() > 8)
	{
		size_t bi = 0, bj = 1;
		int64_t best = INT64_MAX;
		for (size_t i = 0; i < out.size(); i++)
		{
			for (size_t j = i + 1; j < out.size(); j++)
			{
				RECT u;
				UnionRect(&u, &out[i], &out[j]);
				int64_t cost = area(u) - area(out[i]) - area(out[j]);
				if (cost < best)
				{
					best = cost;
					bi = i;
					bj = j;
				}
			}
		}
		UnionRect(&out[bi], &out[bi], &out[bj]);
		out.erase(out.begin() + bj);
	}
	return true;
}

// Uploads the part of the latest complete frame changed since lastSeq into the texture, in place
bool UpdateLatestFrame(fnui_header* header, uint64_t lastSeq, uint64_t seq, ID3D11Texture2D* texture, uint32_t texWidth, uint32_t texHeight)
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

	const uint32_t frameWidth = header->width[front], frameHeight = header->height[front];
	const uint32_t width = std::min(frameWidth, texWidth), height = std::min(frameHeight, texHeight);
	static std::vector<RECT> rects;
	if (!ChangedRects(header, lastSeq, seq, width, height, rects))
	{
		rects = { ChangedSince(header, lastSeq, seq, width, height) };
	}

	for (const RECT& changed : rects)
	{
		if (changed.right <= changed.left || changed.bottom <= changed.top)
		{
			continue;
		}

		D3D11_BOX box = { UINT(changed.left), UINT(changed.top), 0, UINT(changed.right), UINT(changed.bottom), 1 };
		const uint8_t* src = fnui_buffer(header, front) + size_t(changed.top) * header->stride + size_t(changed.left) * 4;

		// copies the data before returning: the buffer may be released right after
		g_nuiGi->GetD3D11DeviceContext()->UpdateSubresource(texture, 0, &box, src, header->stride, 0);
		g_native.uploaded += uint64_t(changed.right - changed.left) * (changed.bottom - changed.top) * 4;
	}

	InterlockedExchange((volatile LONG*)&header->reading, FNUI_NONE);
	return true;
}

// A default-usage texture of ours, given to the game through a share handle (as FiveM's CEF frames
// are): the game draws it, and only the changed rectangles are uploaded into it. The textures
// CreateTextureBacking makes are dynamic: they can only be rewritten whole.
fwRefContainer<nui::GITexture> CreateSharedTexture(uint32_t width, uint32_t height)
{
	ID3D11Device* device = g_nuiGi->GetD3D11Device();
	if (!device)
	{
		return nullptr;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
	if (FAILED(hr))
	{
		trace("Native NUI: shared texture not created (0x%08x)\n", hr);
		return nullptr;
	}

	Microsoft::WRL::ComPtr<IDXGIResource1> resource;
	HANDLE handle = nullptr;
	if (FAILED(texture.As(&resource)) ||
		FAILED(hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)))
	{
		trace("Native NUI: no share handle for the texture (0x%08x)\n", hr);
		return nullptr;
	}

	g_native.shared = texture;
	g_native.sharedHandle = handle;
	return g_nuiGi->CreateTextureFromShareHandle(handle, int(width), int(height), []()
	{
		trace("Native NUI: the game could not open the shared texture\n");
		return true;
	});
}

void RenderNativeFrames()
{
	// DUI textures are used by the game's world: updated even when no NUI is drawn
	if (nui::native::IsMenuActive())
	{
		nui::native::RenderDuis();
	}

	// the host's pages are muted when FiveM would mute its own (NUIWindow.cpp: QueryShouldMute)
	if (nui::native::IsActive())
	{
		bool shouldMute = false;
		g_nuiGi->QueryShouldMute(shouldMute);
		nui::native::SetAudioMuted(shouldMute);
	}

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
		g_native.texture = CreateSharedTexture(header->max_width, header->max_height);
		if (!g_native.texture.GetRef())
		{
			g_native.texture = g_nuiGi->CreateTextureBacking(header->max_width, header->max_height, nui::GITextureFormat::ARGB);
		}
		return;
	}

	if (!g_native.texture->GetNativeTexture())
	{
		return;
	}

	uint64_t seq = header->seq;
	auto nativeTexture = static_cast<ID3D11Texture2D*>(g_native.texture->GetNativeTexture());

	if (g_native.inPlace < 0)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		nativeTexture->GetDesc(&desc);
		g_native.inPlace = desc.Usage == D3D11_USAGE_DEFAULT && g_nuiGi->GetD3D11DeviceContext() ? 1 : 0;
		trace("Native NUI: texture usage %d%s, %s\n", int(desc.Usage), g_native.shared.Get() ? " (shared)" : "",
			g_native.inPlace ? "changed rectangles uploaded in place" : "whole frames copied");
	}

	if (seq != g_native.lastSeq && g_native.inPlace == 1)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		nativeTexture->GetDesc(&desc);

		if (UpdateLatestFrame(header, g_native.lastSeq, seq, nativeTexture, desc.Width, desc.Height))
		{
			g_native.lastSeq = seq;
			g_native.frames++;
		}

		if (!g_native.logged)
		{
			g_native.logged = true;
			trace("Native NUI: first frame drawn (in place)\n");
		}

		// once a minute while frames come: how much was uploaded
		DWORD now = GetTickCount();
		if (!g_native.statsTime)
		{
			g_native.statsTime = now;
		}
		else if (now - g_native.statsTime >= 60000 && g_native.frames)
		{
			trace("Native NUI: %llu frames in %u s, %.1f MB uploaded (%.0f KB per frame)\n", g_native.frames, (now - g_native.statsTime) / 1000,
				g_native.uploaded / 1048576.0, g_native.uploaded / 1024.0 / g_native.frames);
			g_native.frames = g_native.uploaded = 0;
			g_native.statsTime = now;
		}
	}
	else if (seq != g_native.lastSeq)
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

// NativeNuiBridge.cpp: DUI frames, same file format
bool CopyNativeFrame(fnui_header* header, const nui::GILockedTexture& locked)
{
	return CopyLatestFrame(header, locked);
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
