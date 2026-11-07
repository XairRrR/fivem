/*
 * Native Linux NUI: control channel with fivem-nui-host, the Linux Chromium that renders the NUI
 * outside Wine (protocol: wine-fivem nui-host/src/bridge.h).
 *
 * - Messages FiveM sends to its root page (pushEvent: SendNUIMessage, rootCall, handover data;
 *   createFrame / destroyFrame) go to the host instead (CefOverlay.cpp).
 * - The host's requests (nui://, https://nui-game-internal, https://cfx-nui-<resource>, resource
 *   callbacks https://<resource>/...) are answered by FiveM's own scheme handler factories,
 *   driven here the way CEF drives them, so files and callbacks behave as with FiveM's CEF.
 * - invokeNative from the host's pages goes to nui::OnInvokeNative, as NUICallbacks_Native.cpp.
 *
 * Inactive without CitizenFX_NativeNUIFrames: nothing changes.
 */

#include "StdInc.h"
#include "CefOverlay.h"
#include "NativeNuiBridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include <include/cef_parser.h>
#include <include/cef_request.h>
#include <include/cef_response.h>
#include <include/cef_resource_handler.h>

#include <json.hpp>

#include "SharedLegitimacyAPI.h"

#pragma comment(lib, "ws2_32.lib")

extern "C"
{
#include "FiveMNuiShm.h"
}

extern nui::GameInterface* g_nuiGi;
extern HCURSOR g_defaultCursor;

namespace
{
using json = nlohmann::json;

struct Bridge
{
	std::once_flag init;
	std::atomic<bool> active = false;
	SOCKET sock = INVALID_SOCKET;
	std::mutex sendLock;
	std::mutex factoryLock;
	std::condition_variable factoryAdded;
	std::vector<std::tuple<std::string, std::string, CefRefPtr<CefSchemeHandlerFactory>>> factories;
	// the root page is loaded once the connection is up and FiveM's nui:// factory exists (it is
	// registered right after CefInitialize, while the game may already talk to the host)
	std::atomic<bool> nuiFactory = false;
	std::once_flag rootLoaded;
};

Bridge g_bridge;

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

void Send(const json& message, const std::string& body = {})
{
	if (!g_bridge.active)
	{
		return;
	}

	std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
	uint32_t sizes[2] = { uint32_t(text.size()), uint32_t(body.size()) };

	std::unique_lock _(g_bridge.sendLock);
	if (!SendAll(g_bridge.sock, (const char*)sizes, sizeof(sizes)) || !SendAll(g_bridge.sock, text.data(), text.size()) ||
		(!body.empty() && !SendAll(g_bridge.sock, body.data(), body.size())))
	{
		trace("Native NUI: control channel lost\n");
		g_bridge.active = false;
	}
}

// CEF's callbacks, waited for synchronously on the request's worker thread
class WaitCallback : public CefCallback
{
public:
	void Continue() override
	{
		std::unique_lock _(m_lock);
		m_done = true;
		m_cv.notify_all();
	}

	void Cancel() override
	{
		std::unique_lock _(m_lock);
		m_done = m_cancelled = true;
		m_cv.notify_all();
	}

	// false when cancelled or timed out
	bool Wait()
	{
		std::unique_lock _(m_lock);
		bool ok = m_cv.wait_for(_, std::chrono::seconds(60), [this] { return m_done; });
		bool result = ok && !m_cancelled;
		m_done = m_cancelled = false;
		return result;
	}

private:
	std::mutex m_lock;
	std::condition_variable m_cv;
	bool m_done = false;
	bool m_cancelled = false;

	IMPLEMENT_REFCOUNTING(WaitCallback);
};

class WaitReadCallback : public CefResourceReadCallback
{
public:
	void Continue(int bytesRead) override
	{
		std::unique_lock _(m_lock);
		m_bytes = bytesRead;
		m_done = true;
		m_cv.notify_all();
	}

	int Wait()
	{
		std::unique_lock _(m_lock);
		if (!m_cv.wait_for(_, std::chrono::seconds(60), [this] { return m_done; }))
		{
			return -1;
		}
		m_done = false;
		return m_bytes;
	}

private:
	std::mutex m_lock;
	std::condition_variable m_cv;
	bool m_done = false;
	int m_bytes = 0;

	IMPLEMENT_REFCOUNTING(WaitReadCallback);
};

CefRefPtr<CefSchemeHandlerFactory> FindFactoryLocked(const std::string& scheme, const std::string& host)
{
	CefRefPtr<CefSchemeHandlerFactory> any;

	for (auto& [s, domain, factory] : g_bridge.factories)
	{
		if (s != scheme)
		{
			continue;
		}
		if (domain == host)
		{
			return factory;
		}
		if (domain.empty() && !any)
		{
			any = factory;
		}
	}
	return any;
}

// a request that arrives before FiveM registered its factory (startup) waits for it
CefRefPtr<CefSchemeHandlerFactory> FindFactory(const std::string& scheme, const std::string& host)
{
	std::unique_lock lock(g_bridge.factoryLock);
	CefRefPtr<CefSchemeHandlerFactory> factory;
	g_bridge.factoryAdded.wait_for(lock, std::chrono::seconds(15), [&]
	{
		return (factory = FindFactoryLocked(scheme, host)) != nullptr;
	});
	return factory;
}

void LoadRootPage()
{
	if (g_bridge.active && g_bridge.nuiFactory)
	{
		std::call_once(g_bridge.rootLoaded, []
		{
			// the root page hosting the resource frames, as FiveM's CEF loads it (ui_rootUrl)
			Send(json{ { "t", "navigate" }, { "url", "nui://game/ui/root.html" } });
		});
	}
}

// Answers one request of the host with FiveM's handlers, driven like CEF does
void AnswerRequest(json request, std::string body)
{
	uint32_t id = request.value("id", 0u);
	std::string url = request.value("url", "");
	auto reply = [id](int status, const std::string& mime, const json& headers, const std::string& data)
	{
		Send(json{ { "t", "res" }, { "id", id }, { "status", status }, { "mime", mime }, { "headers", headers } }, data);
	};

	CefURLParts parts;
	if (!CefParseURL(url, parts))
	{
		return reply(400, "text/plain", json::object(), "bad url");
	}
	std::string scheme = CefString(&parts.scheme).ToString();
	std::string host = CefString(&parts.host).ToString();

	CefRefPtr<CefRequest> cefRequest = CefRequest::Create();
	CefRequest::HeaderMap headerMap;
	for (auto& [key, value] : request.value("headers", json::object()).items())
	{
		if (value.is_string())
		{
			headerMap.emplace(key, value.get<std::string>());
		}
	}
	CefRefPtr<CefPostData> postData;
	if (!body.empty())
	{
		postData = CefPostData::Create();
		CefRefPtr<CefPostDataElement> element = CefPostDataElement::Create();
		element->SetToBytes(body.size(), body.data());
		postData->AddElement(element);
	}
	cefRequest->Set(url, request.value("method", "GET"), postData, headerMap);

	CefRefPtr<CefSchemeHandlerFactory> factory = FindFactory(scheme, host);
	CefRefPtr<CefResourceHandler> handler = factory ? factory->Create(nullptr, nullptr, scheme, cefRequest) : nullptr;
	if (!handler)
	{
		return reply(404, "text/plain", json::object(), "no handler");
	}

	// Open, or the legacy ProcessRequest when Open is not implemented
	CefRefPtr<WaitCallback> callback = new WaitCallback();
	bool handleRequest = false;
	if (handler->Open(cefRequest, handleRequest, callback))
	{
		if (!handleRequest && !callback->Wait())
		{
			return reply(502, "text/plain", json::object(), "cancelled");
		}
	}
	else if (handleRequest)
	{
		return reply(502, "text/plain", json::object(), "cancelled");
	}
	else if (!handler->ProcessRequest(cefRequest, callback) || !callback->Wait())
	{
		handler->Cancel();
		return reply(404, "text/plain", json::object(), "not handled");
	}

	CefRefPtr<CefResponse> response = CefResponse::Create();
	int64_t length = -1;
	CefString redirect;
	handler->GetResponseHeaders(response, length, redirect);

	json headers = json::object();
	CefResponse::HeaderMap responseHeaders;
	response->GetHeaderMap(responseHeaders);
	for (auto& [key, value] : responseHeaders)
	{
		headers[key.ToString()] = value.ToString();
	}
	if (!redirect.empty())
	{
		headers["Location"] = redirect.ToString();
	}

	// Read, or the legacy ReadResponse when Read is not implemented
	std::string data;
	std::vector<char> buffer(64 * 1024);
	CefRefPtr<WaitReadCallback> readCallback = new WaitReadCallback();
	bool legacy = false;
	for (bool first = true;; first = false)
	{
		if (length >= 0 && int64_t(data.size()) >= length)
		{
			break;
		}

		int bytesRead = 0;
		if (!legacy)
		{
			bool more = handler->Read(buffer.data(), int(buffer.size()), bytesRead, readCallback);
			if (!more && bytesRead == -1 && first)
			{
				legacy = true;
				continue;
			}
			if (!more)
			{
				break;
			}
			if (bytesRead == 0)
			{
				bytesRead = readCallback->Wait();
				if (bytesRead <= 0)
				{
					break;
				}
			}
		}
		else
		{
			bool more = handler->ReadResponse(buffer.data(), int(buffer.size()), bytesRead, callback);
			if (!more)
			{
				break;
			}
			if (bytesRead == 0)
			{
				if (!callback->Wait())
				{
					break;
				}
				continue;
			}
		}
		data.append(buffer.data(), bytesRead);
	}

	int status = response->GetStatus();
	reply(status ? status : 200, response->GetMimeType().ToString(), headers, data);
}

// the host page's cursor by name, as FiveM's CEF gives it a native cursor (NUIClient::OnCursorChange)
void SetCursorByName(const std::string& name)
{
	// resource ids: IDC_* are char or wchar_t pointers depending on UNICODE
	static const std::map<std::string, ULONG_PTR> cursors = {
		{ "hand", ULONG_PTR(IDC_HAND) }, { "ibeam", ULONG_PTR(IDC_IBEAM) }, { "wait", ULONG_PTR(IDC_WAIT) }, { "progress", ULONG_PTR(IDC_APPSTARTING) },
		{ "cross", ULONG_PTR(IDC_CROSS) }, { "help", ULONG_PTR(IDC_HELP) }, { "move", ULONG_PTR(IDC_SIZEALL) }, { "notallowed", ULONG_PTR(IDC_NO) },
		{ "ns", ULONG_PTR(IDC_SIZENS) }, { "ew", ULONG_PTR(IDC_SIZEWE) }, { "nesw", ULONG_PTR(IDC_SIZENESW) }, { "nwse", ULONG_PTR(IDC_SIZENWSE) },
	};

	if (!g_nuiGi || !nui::HasFocus())
	{
		return;
	}

	auto it = cursors.find(name);
	if (it != cursors.end())
	{
		g_nuiGi->SetHostCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(WORD(it->second))));
	}
	else if (name == "none")
	{
		g_nuiGi->SetHostCursor(nullptr);
	}
	else
	{
		g_nuiGi->SetHostCursor(g_defaultCursor);
	}
}

void HandleProcessMessage(const json& message)
{
	std::string name = message.value("name", "");
	auto args = message.value("args", json::array());

	if (name == "invokeNative" && args.size() >= 2 && args[0].is_string() && args[1].is_string())
	{
		std::string type = args[0].get<std::string>();
		std::string arg = args[1].get<std::string>();
		nui::OnInvokeNative(ToWide(type).c_str(), ToWide(arg).c_str());

		if (type == "quit")
		{
			ExitProcess(0);
		}
	}
	else
	{
		trace("Native NUI: unhandled message %s from frame %s\n", name, message.value("frame", ""));
	}
}

// FiveM adds the Cfx.re entitlement header to requests of its trusted frames (the main menu) for
// the hosts the legitimacy component handles (NUIClient::OnBeforeResourceLoad); the host asks for
// it for each such request of its own
void AnswerHeaders(const json& message)
{
	json set = json::object();
	std::string host = message.value("host", "");

	if (!host.empty() && cfx::legitimacy::ShouldProcessHeaders(host.c_str()))
	{
		char key[256] = { 0 };
		char value[2048] = { 0 };
		cfx::legitimacy::ProcessHeaders(key, value);
		if (key[0])
		{
			set[key] = value;
		}
	}

	Send(json{ { "t", "headers-res" }, { "id", message.value("id", 0u) }, { "set", set } });
}

void Reader(SOCKET s)
{
	SetThreadName(-1, "NativeNuiBridge");

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
		if (type == "req")
		{
			// handlers may wait for the game: one worker per request, the channel keeps flowing
			std::thread(AnswerRequest, std::move(message), std::move(body)).detach();
		}
		else if (type == "process")
		{
			HandleProcessMessage(message);
		}
		else if (type == "headers")
		{
			AnswerHeaders(message);
		}
		else if (type == "cursor")
		{
			SetCursorByName(message.value("name", ""));
		}
		else if (type == "hello")
		{
			trace("Native NUI: host control channel ready (%dx%d)\n", message.value("width", 0), message.value("height", 0));
		}
	}

	trace("Native NUI: control channel closed\n");
	g_bridge.active = false;
}

void Connect()
{
	wchar_t path[MAX_PATH];
	DWORD length = GetEnvironmentVariableW(L"CitizenFX_NativeNUIFrames", path, std::size(path));
	if (!length || length >= std::size(path))
	{
		return;
	}

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return;
	}
	fnui_header header = {};
	DWORD read = 0;
	ReadFile(file, &header, sizeof(header), &read, nullptr);
	CloseHandle(file);
	if (read < sizeof(header) || header.magic != FNUI_MAGIC || !header.control_port)
	{
		trace("Native NUI: no control channel in the frame file\n");
		return;
	}

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(uint16_t(header.control_port));
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s == INVALID_SOCKET || connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
	{
		trace("Native NUI: cannot connect to the control channel on port %d (%d)\n", header.control_port, WSAGetLastError());
		if (s != INVALID_SOCKET)
		{
			closesocket(s);
		}
		return;
	}
	BOOL noDelay = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

	g_bridge.sock = s;
	g_bridge.active = true;
	std::thread(Reader, s).detach();
	trace("Native NUI: control channel connected on port %d\n", header.control_port);

	// the host needs to know which http(s) hosts the game serves
	{
		std::unique_lock _(g_bridge.factoryLock);
		for (auto& [scheme, domain, factory] : g_bridge.factories)
		{
			if ((scheme == "http" || scheme == "https") && !domain.empty())
			{
				Send(json{ { "t", "domain" }, { "name", domain } });
			}
		}
	}

	LoadRootPage();
}
}

namespace nui::native
{
bool IsActive()
{
	std::call_once(g_bridge.init, Connect);
	return g_bridge.active;
}

bool IsMenuActive()
{
	static bool wanted = []
	{
		wchar_t value[8];
		DWORD length = GetEnvironmentVariableW(L"CitizenFX_NativeNUIMenu", value, std::size(value));
		return length == 1 && value[0] == L'1';
	}();

	return wanted && IsActive();
}

void PostProcessMessage(const std::string& frame, const std::string& name, const std::vector<std::string>& args)
{
	Send(json{ { "t", "process" }, { "frame", frame }, { "name", name }, { "args", args } });
}

void RegisterFactory(const std::string& scheme, const std::string& domain, CefRefPtr<CefSchemeHandlerFactory> factory)
{
	{
		std::unique_lock _(g_bridge.factoryLock);
		g_bridge.factories.emplace_back(scheme, domain, factory);
	}
	g_bridge.factoryAdded.notify_all();
	if ((scheme == "http" || scheme == "https") && !domain.empty())
	{
		Send(json{ { "t", "domain" }, { "name", domain } });
	}
	if (scheme == "nui")
	{
		g_bridge.nuiFactory = true;
		LoadRootPage();
	}
}

void Navigate(const std::string& url)
{
	Send(json{ { "t", "navigate" }, { "url", url } });
}
}
