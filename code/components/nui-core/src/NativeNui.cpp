/*
 * Native Linux NUI (proof of concept).
 *
 * When FiveM runs under Wine from the native Linux launcher, a Linux Chromium (fivem-nui-host,
 * outside Wine) can render web pages and publish its frames in a shared-memory file. The
 * launcher passes that file in CitizenFX_NativeNUIFrames (a Z:\dev\shm\... path, which Wine maps
 * shared with the Linux side); its frames are drawn over the game, after FiveM's own NUI windows.
 *
 * Without the variable, nothing changes.
 */

#include "StdInc.h"
#include "CefOverlay.h"

extern "C"
{
#include "FiveMNuiShm.h"
}

extern nui::GameInterface* g_nuiGi;

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

	if (!header || !header->host_alive || header->front >= FNUI_BUFFERS)
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
});
