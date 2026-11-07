/*
 * Native Linux NUI: control channel with fivem-nui-host (see NativeNuiBridge.cpp).
 */

#pragma once

#include <string>
#include <vector>

#include <include/cef_scheme.h>

namespace nui::native
{
// true when a native Linux host renders the NUI (CitizenFX_NativeNUIFrames set and the host's
// control channel connected, on first use)
bool IsActive();

// a process message for a frame of the host ("" = the root page), as FiveM sends to its CEF
void PostProcessMessage(const std::string& frame, const std::string& name, const std::vector<std::string>& args);

// every scheme handler factory FiveM registers, to answer the host's requests with them
void RegisterFactory(const std::string& scheme, const std::string& domain, CefRefPtr<CefSchemeHandlerFactory> factory);

// load a page in the host's main frame
void Navigate(const std::string& url);
}
