/*
 * Resource downloads through the native Linux helper (wine-fivem fivem-helper): when FiveM runs
 * under Wine from the native launcher, the files of server resources are downloaded by the system
 * libcurl outside Wine and written straight into the cache folder; the game only receives progress
 * and the result. Protocol: wine-fivem fivem-helper/src/helper.cc.
 *
 * Without CitizenFX_NativeHelper (or if the helper cannot be reached) nothing changes: FiveM's
 * HttpClient downloads as before.
 */

#include "StdInc.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include <json.hpp>

#include "NativeDownloader.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
using json = nlohmann::json;

class NativeRequest;

struct Helper
{
	std::once_flag init;
	std::atomic<bool> active = false;
	SOCKET sock = INVALID_SOCKET;
	std::mutex sendLock;
	std::mutex lock;
	std::map<uint64_t, std::shared_ptr<NativeRequest>> requests;
	std::atomic<uint64_t> nextId = 1;
};

Helper g_helper;

bool SendAll(SOCKET s, const char* data, size_t size)
{
	while (size)
	{
		int n = send(s, data, int(std::min<size_t>(size, 1 << 20)), 0);
		if (n <= 0)
		{
			return false;
		}
		data += n;
		size -= n;
	}
	return true;
}

bool RecvAll(SOCKET s, char* data, size_t size)
{
	while (size)
	{
		int n = recv(s, data, int(std::min<size_t>(size, 1 << 20)), 0);
		if (n <= 0)
		{
			return false;
		}
		data += n;
		size -= n;
	}
	return true;
}

bool Send(const json& message)
{
	std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
	uint32_t sizes[2] = { uint32_t(text.size()), 0 };

	std::unique_lock _(g_helper.sendLock);
	if (!g_helper.active)
	{
		return false;
	}
	if (!SendAll(g_helper.sock, (const char*)sizes, sizeof(sizes)) || !SendAll(g_helper.sock, text.data(), text.size()))
	{
		trace("Native downloads: helper connection lost\n");
		g_helper.active = false;
		return false;
	}
	return true;
}

class NativeRequest : public HttpRequestHandle
{
public:
	NativeRequest(uint64_t id, const HttpRequestOptions& options, const std::function<void(bool, const char*, size_t)>& callback)
		: m_id(id), m_progress(options.progressCallback), m_callback(callback), m_errorBody(options.addErrorBody)
	{
	}

	bool HasCompleted() override
	{
		return m_completed;
	}

	void SetRequestWeight(int weight) override
	{
		Send(json{ { "t", "weight" }, { "id", m_id }, { "weight", weight } });
	}

	void Abort() override
	{
		Send(json{ { "t", "cancel" }, { "id", m_id } });
	}

	std::string GetRawBody() override
	{
		return m_body;
	}

	void OnProgress(uint64_t now, uint64_t total)
	{
		if (m_progress)
		{
			ProgressInfo info;
			info.downloadNow = now;
			info.downloadTotal = total;
			m_progress(info);
		}
	}

	void OnDone(const json& message, std::string body)
	{
		bool ok = message.value("ok", false);
		m_body = body;

		if (ok)
		{
			m_completed = true;
			m_callback(true, "", size_t(message.value("size", uint64_t(0))));
			return;
		}

		// as HttpClient: "HTTP 404" (": <body>" with addErrorBody) or curl's error
		std::string error = message.value("error", std::string("failed"));
		if (m_errorBody && error.rfind("HTTP ", 0) == 0)
		{
			error += ": " + body;
		}
		m_completed = true;
		m_callback(false, error.c_str(), error.size());
	}

private:
	uint64_t m_id;
	std::function<void(const ProgressInfo&)> m_progress;
	std::function<void(bool, const char*, size_t)> m_callback;
	bool m_errorBody;
	std::atomic<bool> m_completed = false;
	std::string m_body;
};

void FailAll(const char* why)
{
	std::map<uint64_t, std::shared_ptr<NativeRequest>> requests;
	{
		std::unique_lock _(g_helper.lock);
		requests.swap(g_helper.requests);
	}
	for (auto& [id, request] : requests)
	{
		request->OnDone(json{ { "ok", false }, { "error", why } }, {});
	}
}

void Reader(SOCKET s)
{
	SetThreadName(-1, "NativeDownloads");

	for (;;)
	{
		uint32_t sizes[2];
		if (!RecvAll(s, (char*)sizes, sizeof(sizes)))
		{
			break;
		}
		std::string text(sizes[0], '\0'), body(sizes[1], '\0');
		if (!RecvAll(s, text.data(), text.size()) || !RecvAll(s, body.data(), body.size()))
		{
			break;
		}

		json message = json::parse(text, nullptr, false);
		if (message.is_discarded() || !message.is_object())
		{
			continue;
		}

		std::string type = message.value("t", "");
		uint64_t id = message.value("id", uint64_t(0));
		std::shared_ptr<NativeRequest> request;
		{
			std::unique_lock _(g_helper.lock);
			auto it = g_helper.requests.find(id);
			if (it != g_helper.requests.end())
			{
				request = it->second;
				if (type == "done")
				{
					g_helper.requests.erase(it);
				}
			}
		}

		if (type == "hello")
		{
			trace("Native downloads: helper ready\n");
		}
		else if (request && type == "progress")
		{
			request->OnProgress(message.value("now", uint64_t(0)), message.value("total", uint64_t(0)));
		}
		else if (request && type == "done")
		{
			request->OnDone(message, std::move(body));
		}
	}

	trace("Native downloads: helper connection closed\n");
	g_helper.active = false;
	FailAll("native download helper disconnected");
}

void Connect()
{
	char value[64];
	DWORD length = GetEnvironmentVariableA("CitizenFX_NativeHelper", value, sizeof(value));
	if (!length || length >= sizeof(value))
	{
		return;
	}

	std::string address = value;
	auto colon = address.rfind(':');
	if (colon == std::string::npos)
	{
		return;
	}

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(uint16_t(atoi(address.c_str() + colon + 1)));
	inet_pton(AF_INET, address.substr(0, colon).c_str(), &addr.sin_addr);
	if (s == INVALID_SOCKET || connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
	{
		trace("Native downloads: cannot connect to the helper at %s (%d), using HttpClient\n", address, WSAGetLastError());
		if (s != INVALID_SOCKET)
		{
			closesocket(s);
		}
		return;
	}
	BOOL noDelay = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

	g_helper.sock = s;
	g_helper.active = true;
	std::thread(Reader, s).detach();
	trace("Native downloads: helper connected at %s\n", address);
}

// the Linux path of a Windows path, by Wine (kernel32's wine_get_unix_file_name)
std::string UnixPath(const std::string& windowsPath)
{
	using Fn = char*(CDECL*)(const wchar_t*);
	static auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
	if (!fn)
	{
		return {};
	}

	std::wstring wide = ToWide(windowsPath);
	for (auto& c : wide)
	{
		if (c == L'/')
		{
			c = L'\\';
		}
	}

	char* unixPath = fn(wide.c_str());
	if (!unixPath)
	{
		return {};
	}
	std::string result = unixPath;
	HeapFree(GetProcessHeap(), 0, unixPath);
	return result;
}
}

namespace fx::native_downloads
{
HttpRequestPtr DoFileGetRequest(const std::string& url, const std::string& windowsPath, const HttpRequestOptions& options,
	const std::function<void(bool, const char*, size_t)>& callback)
{
	std::call_once(g_helper.init, Connect);
	if (!g_helper.active)
	{
		return nullptr;
	}

	std::string unixPath = UnixPath(windowsPath);
	if (unixPath.empty())
	{
		return nullptr;
	}

	uint64_t id = g_helper.nextId++;
	auto request = std::make_shared<NativeRequest>(id, options, callback);
	{
		std::unique_lock _(g_helper.lock);
		g_helper.requests[id] = request;
	}

	json headers = json::object();
	for (const auto& [key, value] : options.headers)
	{
		headers[key] = value;
	}

	if (!Send(json{ { "t", "get" }, { "id", id }, { "url", url }, { "path", unixPath }, { "headers", headers },
			{ "weight", options.weight }, { "max_size", options.maxFilesize }, { "error_body", options.addErrorBody },
			{ "follow", options.followLocation } }))
	{
		std::unique_lock _(g_helper.lock);
		g_helper.requests.erase(id);
		return nullptr;
	}

	return request;
}
}
