#pragma once

#include <HttpClient.h>

namespace fx::native_downloads
{
// Downloads a file through the native Linux helper (fivem-helper, started by the native launcher,
// CitizenFX_NativeHelper=127.0.0.1:port) instead of HttpClient's sockets under Wine; same
// callback contract as HttpClient::DoFileGetRequest. Native only when launched that way (a helper
// failure fails the download); nullptr only without the variable: the caller then uses HttpClient.
HttpRequestPtr DoFileGetRequest(const std::string& url, const std::string& windowsPath, const HttpRequestOptions& options,
	const std::function<void(bool, const char*, size_t)>& callback);
}
