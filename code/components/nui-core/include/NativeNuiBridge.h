/*
 * Native Linux NUI: control channel with fivem-nui-host (see NativeNuiBridge.cpp).
 */

#pragma once

#include <string>
#include <vector>

#include <include/cef_scheme.h>

#include "CefOverlay.h"

namespace nui
{
// FiveM's NUI audio sink (NUIClient.cpp), null before extra-natives sets it
IAudioSink* GetAudioSink();
}

namespace nui::native
{
// true when a native Linux host renders the NUI (CitizenFX_NativeNUIFrames set and the host's
// control channel connected, on first use)
bool IsActive();

// the main menu (mpMenu) is also rendered by the host, as a frame of its root page
// (CitizenFX_NativeNUIMenu=1, opt-in)
bool IsMenuActive();

// a process message for a frame of the host ("" = the root page), as FiveM sends to its CEF
void PostProcessMessage(const std::string& frame, const std::string& name, const std::vector<std::string>& args);

// every scheme handler factory FiveM registers, to answer the host's requests with them
void RegisterFactory(const std::string& scheme, const std::string& domain, CefRefPtr<CefSchemeHandlerFactory> factory);

// load a page in the host's main frame
void Navigate(const std::string& url);

// DUI browsers (CREATE_DUI) rendered by the host, when it renders the main menu too: each has its
// own frame file, copied into the DUI's texture on the render thread (wine-fivem nui-host/src/dui.h)
// false: not handled natively (the caller creates a CEF browser)
bool CreateDui(const std::string& handle, int width, int height, const std::string& url);
bool IsDui(const std::string& handle);
void DestroyDui(const std::string& handle);
void SetDuiUrl(const std::string& handle, const std::string& url);
void RunDuiScript(const std::string& handle, const std::string& code);
// fnui_input type, button (0 left, 1 right, 2 middle), DUI pixels, a/b as fnui_input
void SendDuiInput(const std::string& handle, int type, int button, int x, int y, int a, int b);
// the host created its browser (IS_DUI_AVAILABLE)
bool IsDuiAvailable(const std::string& handle);
fwRefContainer<nui::GITexture> GetDuiTexture(const std::string& handle);
// render thread, each frame: new DUI frames into their textures
void RenderDuis();

// Audio of the host's pages, played by the game through FiveM's NUI audio sink (as NUIClient.cpp
// does with its own Chromium; wine-fivem nui-host/src/audio.h): the sink was set (tells the host)
void OnAudioSink();
// the game's mute state (QueryShouldMute), sent to the host when it changes
void SetAudioMuted(bool muted);

// The game view for the host's pages (wine-fivem nui-host/src/gameview.h): while on, the game's image
// (FiveM's game view texture, rage-graphics-five GameViewCapture.cpp) is copied into the host's frame
// file each frame (NativeNui.cpp)
void SetGameView(bool on, const std::string& shm);
}
