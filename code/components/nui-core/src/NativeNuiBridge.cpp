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

#include <algorithm>
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
};

Bridge g_bridge;

// audio streams of the host's browsers (sid -> the sink's stream)
std::mutex g_audioLock;
std::map<int, std::shared_ptr<nui::IAudioStream>> g_audioStreams;
std::atomic<int> g_audioMuted = -1;	// -1: not sent yet

void OnAudioStart(const json& message)
{
	auto sink = nui::GetAudioSink();
	if (!sink)
	{
		return;
	}

	nui::AudioStreamParams params;
	params.channels = message.value("channels", 2);
	params.channelLayout = nui::CefChannelLayout(message.value("layout", 3));
	params.sampleRate = message.value("rate", 48000);
	params.framesPerBuffer = message.value("frames", 1024);
	params.frameName = message.value("frame", "");

	auto stream = sink->CreateAudioStream(params);
	if (stream)
	{
		std::unique_lock _(g_audioLock);
		g_audioStreams[message.value("sid", 0)] = stream;
	}
}

void OnAudioData(const json& message, const std::string& body)
{
	std::shared_ptr<nui::IAudioStream> stream;
	{
		std::unique_lock _(g_audioLock);
		auto it = g_audioStreams.find(message.value("sid", 0));
		if (it == g_audioStreams.end())
		{
			return;
		}
		stream = it->second;
	}

	int frames = message.value("frames", 0);
	if (frames <= 0)
	{
		return;
	}
	size_t channels = body.size() / (sizeof(float) * frames);
	if (!channels || channels > 8 || channels * sizeof(float) * frames != body.size())
	{
		return;
	}

	// planar: channel c's frames follow channel c - 1's
	const float* planes[8];
	for (size_t c = 0; c < channels; c++)
	{
		planes[c] = reinterpret_cast<const float*>(body.data()) + c * frames;
	}
	stream->ProcessPacket(planes, frames, int64_t(message.value("pts", 0.0)));
}

void OnAudioStop(const json& message)
{
	std::unique_lock _(g_audioLock);
	g_audioStreams.erase(message.value("sid", 0));
}

// DUI browsers of the host (nui::native::CreateDui)
struct NativeDui
{
	int width = 0, height = 0;
	fwRefContainer<nui::GITexture> texture;
	fnui_header* header = nullptr;		// the host's frame file, once it created the browser
	uint64_t lastSeq = 0;
	bool logged = false;
};

std::mutex g_duiLock;
std::map<std::string, std::shared_ptr<NativeDui>> g_duis;

void UnmapDui(NativeDui& dui)
{
	if (dui.header)
	{
		UnmapViewOfFile(dui.header);
		dui.header = nullptr;
	}
}

// {"t":"dui-created","id","shm":"/dev/shm/...","width","height"}: the frame file, through Wine's Z:
void OnDuiCreated(const json& message)
{
	std::string id = message.value("id", "");
	std::string shm = message.value("shm", "");
	std::wstring path = L"Z:" + ToWide(shm);
	std::replace(path.begin(), path.end(), L'/', L'\\');

	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		trace("Native NUI: DUI %s: cannot open %s (%d)\n", id, shm, GetLastError());
		return;
	}
	HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
	CloseHandle(file);
	auto header = mapping ? static_cast<fnui_header*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0)) : nullptr;
	if (mapping)
	{
		CloseHandle(mapping);
	}
	if (!header || header->magic != FNUI_MAGIC || header->version != FNUI_VERSION)
	{
		trace("Native NUI: DUI %s: %s is not a frame file\n", id, shm);
		if (header)
		{
			UnmapViewOfFile(header);
		}
		return;
	}

	std::unique_lock _(g_duiLock);
	auto it = g_duis.find(id);
	if (it == g_duis.end())
	{
		UnmapViewOfFile(header);		// destroyed meanwhile
		return;
	}
	UnmapDui(*it->second);
	it->second->header = header;
	it->second->lastSeq = 0;
	trace("Native NUI: DUI %s rendered by the host (%dx%d)\n", id, header->max_width, header->max_height);
}

// the host's browser is gone (its renderer crashed): the texture keeps its last frame
void OnDuiClosed(const json& message)
{
	std::unique_lock _(g_duiLock);
	auto it = g_duis.find(message.value("id", ""));
	if (it != g_duis.end())
	{
		UnmapDui(*it->second);
	}
}

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
		else if (type == "audio-data")
		{
			OnAudioData(message, body);
		}
		else if (type == "audio-start")
		{
			OnAudioStart(message);
		}
		else if (type == "audio-stop")
		{
			OnAudioStop(message);
		}
		else if (type == "dui-created")
		{
			OnDuiCreated(message);
		}
		else if (type == "dui-closed")
		{
			OnDuiClosed(message);
		}
		else if (type == "hello")
		{
			trace("Native NUI: host control channel ready (%dx%d)\n", message.value("width", 0), message.value("height", 0));
		}
	}

	trace("Native NUI: control channel closed\n");
	g_bridge.active = false;

	{
		std::unique_lock _(g_audioLock);
		g_audioStreams.clear();
	}

	// the host closes the DUI browsers with the connection
	std::unique_lock _(g_duiLock);
	for (auto& [id, dui] : g_duis)
	{
		UnmapDui(*dui);
	}
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

	// the pages' audio is played by the game when it has its NUI audio sink
	if (nui::GetAudioSink())
	{
		Send(json{ { "t", "audio-sink" } });
	}

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

}

void Navigate(const std::string& url)
{
	Send(json{ { "t", "navigate" }, { "url", url } });
}

void OnAudioSink()
{
	// set after the connection: tell the host now (before, Connect does)
	if (g_bridge.active)
	{
		Send(json{ { "t", "audio-sink" } });
	}
}

void SetAudioMuted(bool muted)
{
	if (g_audioMuted.exchange(muted ? 1 : 0) != (muted ? 1 : 0))
	{
		Send(json{ { "t", "audio-mute" }, { "muted", muted } });
	}
}

bool CreateDui(const std::string& handle, int width, int height, const std::string& url)
{
	if (!IsMenuActive())
	{
		return false;
	}

	auto dui = std::make_shared<NativeDui>();
	dui->width = width;
	dui->height = height;
	// as NUIWindow::Initialize: the texture exists at once (scripts make a runtime texture of it
	// right after CREATE_DUI), materialized on the render queue
	dui->texture = g_nuiGi->CreateTextureBacking(width, height, nui::GITextureFormat::ARGB);

	{
		std::unique_lock _(g_duiLock);
		if (auto it = g_duis.find(handle); it != g_duis.end())
		{
			UnmapDui(*it->second);
		}
		g_duis[handle] = dui;
	}
	Send(json{ { "t", "dui-create" }, { "id", handle }, { "url", url }, { "width", width }, { "height", height } });
	return true;
}

bool IsDui(const std::string& handle)
{
	std::unique_lock _(g_duiLock);
	return g_duis.find(handle) != g_duis.end();
}

void DestroyDui(const std::string& handle)
{
	{
		std::unique_lock _(g_duiLock);
		auto it = g_duis.find(handle);
		if (it == g_duis.end())
		{
			return;
		}
		UnmapDui(*it->second);
		g_duis.erase(it);
	}
	Send(json{ { "t", "dui-destroy" }, { "id", handle } });
}

void SetDuiUrl(const std::string& handle, const std::string& url)
{
	Send(json{ { "t", "dui-url" }, { "id", handle }, { "url", url } });
}

void RunDuiScript(const std::string& handle, const std::string& code)
{
	Send(json{ { "t", "dui-js" }, { "id", handle }, { "code", code } });
}

void SendDuiInput(const std::string& handle, int type, int button, int x, int y, int a, int b)
{
	Send(json{ { "t", "dui-input" }, { "id", handle }, { "type", type }, { "button", button }, { "x", x }, { "y", y },
		{ "a", a }, { "b", b }, { "modifiers", 0 } });
}

bool IsDuiAvailable(const std::string& handle)
{
	std::unique_lock _(g_duiLock);
	auto it = g_duis.find(handle);
	return it != g_duis.end() && it->second->header;
}

fwRefContainer<nui::GITexture> GetDuiTexture(const std::string& handle)
{
	std::unique_lock _(g_duiLock);
	auto it = g_duis.find(handle);
	return it != g_duis.end() ? it->second->texture : nullptr;
}
}

// NativeNui.cpp
bool CopyNativeFrame(fnui_header* header, const nui::GILockedTexture& locked);

namespace nui::native
{
void RenderDuis()
{
	std::unique_lock _(g_duiLock);

	for (auto& [id, dui] : g_duis)
	{
		auto header = dui->header;
		if (!header || !header->host_alive || header->front >= FNUI_BUFFERS || !dui->texture.GetRef() ||
			!dui->texture->GetNativeTexture())
		{
			continue;
		}

		uint64_t seq = header->seq;
		if (seq == dui->lastSeq)
		{
			continue;
		}

		nui::GILockedTexture locked = {};
		if (dui->texture->Map(0, 0, &locked, nui::GILockFlags::WriteDiscard))
		{
			if (CopyNativeFrame(header, locked))
			{
				dui->lastSeq = seq;
			}
			dui->texture->Unmap(&locked);

			if (!dui->logged)
			{
				dui->logged = true;
				trace("Native NUI: DUI %s: first frame\n", id);
			}
		}
	}
}
}
