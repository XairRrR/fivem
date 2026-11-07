/*
 * This file is part of the Cfx project - https://cfx.re/
 *
 * See LICENSE in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "HttpClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include <json.hpp>

#pragma comment(lib, "ws2_32.lib")

#include <chrono>

#include <boost/algorithm/string.hpp>
#include <Error.h>

#include <sstream>

#define CURL_STATICLIB
#include <curl/multi.h>

#include <VFSManager.h>

#include <tbb/concurrent_queue.h>

#include <shared_mutex>

#include <UvLoopManager.h>

class HttpClientImpl
{
public:
	CURLM* multi;
	tbb::concurrent_queue<CURL*> handlesToAdd;
	tbb::concurrent_queue<std::function<void()>> cbsToRun;
	HttpClient* client;
	uv_loop_t* loop;
	uv_timer_t timeout;
	std::string userAgent;

	std::shared_ptr<uvw::AsyncHandle> addHandle;
	std::shared_ptr<uvw::AsyncHandle> runCb;

	std::shared_mutex mutex;

	HttpClientImpl()
		: multi(nullptr), client(nullptr), loop(nullptr)
	{

	}

	void AddCurlHandle(CURL* easy);
};

void HttpClientImpl::AddCurlHandle(CURL* easy)
{
	handlesToAdd.push(easy);

	std::shared_lock<std::shared_mutex> _(mutex);

	if (addHandle)
	{
		addHandle->send();
	}
}

class CurlData final
{
public:
	std::string url;
	std::string postData;
	std::function<void(bool, std::string_view)> callback;
	std::function<size_t(const void*, size_t)> writeFunction;
	std::function<void()> preCallback;
	std::function<void(const ProgressInfo&)> progressCallback;
	std::stringstream ss;
	char errBuffer[CURL_ERROR_SIZE];
	CURL* curlHandle;
	HttpClientImpl* impl;
	curl_slist* headersSList = nullptr;
	int defaultWeight;
	int weight;
	HttpHeaderListPtr responseHeaders;
	std::shared_ptr<int> responseCode;
	std::chrono::milliseconds timeoutNoResponse;
	std::chrono::high_resolution_clock::duration reqStart;
	uint64_t maxFilesize;

	std::stringstream errorBody;
	std::stringstream rawBody;

	bool addErrorBody = false;
	bool addRawBody = false;

	// what the native Linux helper needs to make the same request (the curl handle's options)
	std::map<std::string, std::string> headers;
	std::string method;			// CURLOPT_CUSTOMREQUEST, empty for none
	bool post = false;			// CURLOPT_POSTFIELDS set (postData)
	bool followLocation = true;
	bool ipv4 = false;
	// a request made by the helper (native_http below): its id, its status, whether it completed
	uint64_t nativeId = 0;
	long nativeStatus = 0;
	bool nativeFinished = false;

	CurlData();

	virtual ~CurlData();

	void HandleResult(CURL* handle, CURLcode result);

	size_t HandleWrite(const void* data, size_t size, size_t nmemb);
};

CurlData::CurlData()
{
	timeoutNoResponse = std::chrono::milliseconds(0);
	reqStart = std::chrono::high_resolution_clock::duration(0);

	writeFunction = [this] (const void* data, size_t size)
	{
		ss << std::string((const char*)data, size);
		return size;
	};
}

CurlData::~CurlData()
{
	curl_slist_free_all(headersSList);
	headersSList = nullptr;
}

size_t CurlData::HandleWrite(const void* data, size_t size, size_t nmemb)
{
	if (addErrorBody)
	{
		long code;
		curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &code);

		if (code >= 400)
		{
			errorBody << std::string_view{ (const char*)data, size * nmemb };
		}
	}

	if (addRawBody)
	{
		rawBody << std::string_view{ (const char*)data, size * nmemb };
	}

	return writeFunction(data, size * nmemb);
}

void CurlData::HandleResult(CURL* handle, CURLcode result)
{
	if (preCallback)
	{
		preCallback();
		preCallback = {};
	}

	if (!callback)
	{
		return;
	}

	if (result != CURLE_OK)
	{
		auto failure = fmt::sprintf("%s - CURL error code %d (%s)", errBuffer, (int)result, curl_easy_strerror(result));

		callback(false, failure);
	}
	else
	{
		long code;
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);

		if (this->responseCode)
		{
			*this->responseCode = (int)code;
		}

		if (code >= 400)
		{
			auto failure = fmt::sprintf("HTTP %d%s",
				code,
				addErrorBody
					? fmt::sprintf(": %s", this->errorBody.str())
					: "");

			callback(false, failure);
		}
		else
		{
			auto str = ss.str();

			callback(true, str);
		}
	}

	callback = {};
}

struct curl_context_t
{
	uv_poll_t poll_handle;
	curl_socket_t sockfd;
	HttpClientImpl* impl;
};

static curl_context_t* CreateCurlContext(HttpClientImpl* i, curl_socket_t sockfd)
{
	curl_context_t* context = new curl_context_t;
	context->sockfd = sockfd;

	uv_poll_init_socket(i->loop, &context->poll_handle, sockfd);
	context->poll_handle.data = context;
	context->impl = i;

	return context;
}

static void FinalizeCurlHandle(CURL* curl, CURLM* multi = nullptr, const CURLcode* result = nullptr)
{
	char* dataPtr;
	curl_easy_getinfo(curl, CURLINFO_PRIVATE, &dataPtr);

	auto data = reinterpret_cast<std::shared_ptr<CurlData>*>(dataPtr);

	if (result)
	{
		(*data)->HandleResult(curl, *result);
	}

	if (multi)
	{
		curl_multi_remove_handle(multi, curl);
	}

	curl_easy_cleanup(curl);

	// delete data
	(*data)->curlHandle = nullptr;
	delete data;
}

static void CheckMultiInfo(HttpClientImpl* impl)
{
	// read infos
	CURLMsg* msg;

	do
	{
		int nq;
		msg = curl_multi_info_read(impl->multi, &nq);

		// is this a completed transfer?
		if (msg && msg->msg == CURLMSG_DONE)
		{
			// get the handle and the result
			CURL* curl = msg->easy_handle;
			CURLcode result = msg->data.result;

			FinalizeCurlHandle(curl, impl->multi, &result);
		}
	} while (msg);
}

static void CurlPerform(uv_poll_t* req, int status, int events)
{
	int running_handles;
	int flags = 0;
	curl_context_t* context;

	if (events & UV_READABLE)
		flags |= CURL_CSELECT_IN;
	if (events & UV_WRITABLE)
		flags |= CURL_CSELECT_OUT;

	context = (curl_context_t*)req->data;

	curl_multi_socket_action(context->impl->multi, context->sockfd, flags,
	&running_handles);

	CheckMultiInfo(context->impl);
}

static void CurlCloseCb(uv_handle_t* handle)
{
	curl_context_t* context = (curl_context_t*)handle->data;
	delete context;
}

static void DestroyCurlContext(curl_context_t* context)
{
	uv_close((uv_handle_t*)&context->poll_handle, CurlCloseCb);
}

static int CurlHandleSocket(CURL* easy, curl_socket_t s, int action, void* userp, void* socketp)
{
	auto impl = reinterpret_cast<HttpClientImpl*>(userp);

	curl_context_t* curl_context;
	int events = 0;

	switch (action)
	{
		case CURL_POLL_IN:
		case CURL_POLL_OUT:
		case CURL_POLL_INOUT:
			curl_context = socketp ? (curl_context_t*)socketp : CreateCurlContext(impl, s);

			curl_multi_assign(impl->multi, s, (void*)curl_context);

			if (action != CURL_POLL_IN)
				events |= UV_WRITABLE;
			if (action != CURL_POLL_OUT)
				events |= UV_READABLE;

			uv_poll_start(&curl_context->poll_handle, events, CurlPerform);
			break;
		case CURL_POLL_REMOVE:
			if (socketp)
			{
				uv_poll_stop(&((curl_context_t*)socketp)->poll_handle);
				DestroyCurlContext((curl_context_t*)socketp);
				curl_multi_assign(impl->multi, s, NULL);
			}
			break;
	}

	return 0;
}

static void OnTimeout(uv_timer_t* req)
{
	auto impl = reinterpret_cast<HttpClientImpl*>(req->data);

	int running_handles;
	curl_multi_socket_action(impl->multi, CURL_SOCKET_TIMEOUT, 0,
	&running_handles);
	CheckMultiInfo(impl);
}

static int CurlStartTimeout(CURLM* multi, long timeout_ms, void* userp)
{
	auto impl = reinterpret_cast<HttpClientImpl*>(userp);

	if (timeout_ms < 0)
	{
		uv_timer_stop(&impl->timeout);
	}
	else
	{
		if (timeout_ms == 0)
			timeout_ms = 1; /* 0 means directly call socket_action, but we'll do it
                         in a bit */
		uv_timer_start(&impl->timeout, OnTimeout, timeout_ms, 0);
	}
	return 0;
}

HttpClient::HttpClient(const wchar_t* userAgent /* = L"CitizenFX/1" */, const std::string& loopId)
	: m_impl(new HttpClientImpl())
{
	m_impl->client = this;
	m_impl->userAgent = ToNarrow(userAgent);
	m_impl->multi = curl_multi_init();
	curl_multi_setopt(m_impl->multi, CURLMOPT_PIPELINING, CURLPIPE_HTTP1 | CURLPIPE_MULTIPLEX);
	curl_multi_setopt(m_impl->multi, CURLMOPT_MAX_HOST_CONNECTIONS, 8);
	curl_multi_setopt(m_impl->multi, CURLMOPT_SOCKETFUNCTION, CurlHandleSocket);
	curl_multi_setopt(m_impl->multi, CURLMOPT_SOCKETDATA, m_impl.get());
	curl_multi_setopt(m_impl->multi, CURLMOPT_TIMERFUNCTION, CurlStartTimeout);
	curl_multi_setopt(m_impl->multi, CURLMOPT_TIMERDATA, m_impl.get());
	
	auto loop = Instance<net::UvLoopManager>::Get()->GetOrCreate(loopId.empty() ? "httpClient" : loopId);
	m_impl->loop = loop->GetLoop();

	loop->EnqueueCallback([this, loop]()
	{
		auto impl = m_impl.get();
		uv_timer_init(loop->GetLoop(), &impl->timeout);
		impl->timeout.data = impl;

		auto runCbs = [impl]()
		{
			std::function<void()> runCb;

			while (impl->cbsToRun.try_pop(runCb))
			{
				runCb();
			}
		};

		auto runAdds = [impl]()
		{
			CURL* addHandle;

			while (impl->handlesToAdd.try_pop(addHandle))
			{
				curl_multi_add_handle(impl->multi, addHandle);
			}
		};

		auto addHandle = loop->Get()->resource<uvw::AsyncHandle>();
		addHandle->on<uvw::AsyncEvent>([runAdds](const uvw::AsyncEvent& ev, uvw::AsyncHandle& handle)
		{
			runAdds();
		});

		auto runCb = loop->Get()->resource<uvw::AsyncHandle>();
		runCb->on<uvw::AsyncEvent>([runCbs](const uvw::AsyncEvent& ev, uvw::AsyncHandle& handle)
		{
			runCbs();
		});

		std::lock_guard<std::shared_mutex> _(impl->mutex);

		runCbs();
		runAdds();

		impl->runCb = runCb;
		impl->addHandle = addHandle;
	});
}

HttpClient::~HttpClient()
{

}

std::string HttpClient::BuildPostString(const std::map<std::string, std::string>& fields)
{
	std::stringstream retval;

	for (auto& field : fields)
	{
		retval << field.first << "=" << url_encode(field.second) << "&";
	}

	fwString str = fwString(retval.str().c_str());
	return str.substr(0, str.length() - 1);
}

static std::string MakeURL(const std::wstring& host, uint16_t port, const std::wstring& url)
{
	return fmt::sprintf("http://%s:%d%s", ToNarrow(host), port, ToNarrow(url));
}

static auto CurlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t
{
	auto cd = reinterpret_cast<std::shared_ptr<CurlData>*>(userdata);

	return (*cd)->HandleWrite(ptr, size, nmemb);
}

static int CurlXferInfo(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	auto cdPtr = reinterpret_cast<std::shared_ptr<CurlData>*>(userdata);
	CurlData* cd = cdPtr->get();

	if (cd->progressCallback)
	{
		ProgressInfo info;
		info.downloadNow = dlnow;
		info.downloadTotal = dltotal;

		cd->progressCallback(info);
	}

	using namespace std::chrono_literals;

	if (cd->timeoutNoResponse != 0ms)
	{
		// first progress callback is the start of the timeout
		// if we do this any earlier, we run the risk of having concurrent connections that
		// are throttled due to max-connection limit time out instantly
		if (cd->reqStart.count() == 0)
		{
			cd->reqStart = std::chrono::high_resolution_clock::now().time_since_epoch();
		}

		if (dlnow == 0 && dltotal == 0)
		{
			if (std::chrono::high_resolution_clock::now().time_since_epoch() - cd->reqStart > cd->timeoutNoResponse)
			{
				// abort due to timeout
				return 1;
			}
		}
	}

	return 0;
}

static size_t CurlHeaderInfo(char* buffer, size_t size, size_t nitems, void* userdata)
{
	auto cdPtr = reinterpret_cast<std::shared_ptr<CurlData>*>(userdata);
	CurlData* cd = cdPtr->get();

	if (cd->responseHeaders)
	{
		std::string str(buffer, size * nitems);

		// reset HTTP headers if we followed a Location and got a new HTTP response
		if (str.find("HTTP/") == 0)
		{
			(*cd->responseHeaders).clear();
		}

		auto colonPos = str.find(": ");
		
		if (colonPos != std::string::npos)
		{
			(*cd->responseHeaders).emplace(str.substr(0, colonPos), str.substr(colonPos + 2, str.length() - 2 - colonPos - 2));
		}
	}

	return size * nitems;
}

static std::shared_ptr<CurlData> SetupCURLHandle(const std::unique_ptr<HttpClientImpl>& impl, const std::string& url, const HttpRequestOptions& options)
{
	if (boost::algorithm::to_lower_copy(url).find("file://") == 0)
	{
		FatalError("Invalid URL in HttpClient\nHit a file:// URL in HttpClient (%s). Please report this somewhere.", url);
	}

	auto curlHandle = curl_easy_init();

	auto curlData = std::make_shared<CurlData>();
	curlData->url = url;
	curlData->progressCallback = options.progressCallback;
	curlData->curlHandle = curlHandle;
	curlData->impl = impl.get();
	curlData->defaultWeight = curlData->weight = options.weight;
	curlData->responseHeaders = options.responseHeaders;
	curlData->responseCode = options.responseCode;
	curlData->timeoutNoResponse = options.timeoutNoResponse;
	curlData->addErrorBody = options.addErrorBody;
	curlData->addRawBody = options.addRawBody;
	curlData->maxFilesize = options.maxFilesize;
	curlData->headers = options.headers;
	curlData->followLocation = options.followLocation;
	curlData->ipv4 = options.ipv4;

	auto scb = options.streamingCallback;

	if (scb)
	{
		curlData->writeFunction = [scb](const void* data, size_t len)
		{
			bool success = scb(std::string(reinterpret_cast<const char*>(data), len));

			return (success) ? len : 0;
		};
	}

	auto curlDataPtr = new std::shared_ptr<CurlData>(curlData);

	curl_easy_setopt(curlHandle, CURLOPT_USERAGENT, impl->userAgent.c_str());
	curl_easy_setopt(curlHandle, CURLOPT_URL, curlData->url.c_str());
	curl_easy_setopt(curlHandle, CURLOPT_PRIVATE, curlDataPtr);
	curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, curlDataPtr);
	curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, CurlWrite);
	curl_easy_setopt(curlHandle, CURLOPT_XFERINFODATA, curlDataPtr);
	curl_easy_setopt(curlHandle, CURLOPT_XFERINFOFUNCTION, CurlXferInfo);
	curl_easy_setopt(curlHandle, CURLOPT_NOPROGRESS, 0);
	curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, options.followLocation);
	curl_easy_setopt(curlHandle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(curlHandle, CURLOPT_ERRORBUFFER, &curlData->errBuffer);
	curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYPEER, 0);
	curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYHOST, 0);
	curl_easy_setopt(curlHandle, CURLOPT_STREAM_WEIGHT, long(options.weight));
	
	if (options.responseHeaders)
	{
		curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, CurlHeaderInfo);
		curl_easy_setopt(curlHandle, CURLOPT_HEADERDATA, curlDataPtr);
	}

	curl_slist* headers = nullptr;
	for (const auto& header : options.headers)
	{
		headers = curl_slist_append(headers, va("%s: %s", header.first, header.second));
	}

	curlData->headersSList = headers;

	curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, headers);

	if (options.ipv4)
	{
		curl_easy_setopt(curlHandle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	}

	if (options.maxFilesize > 0)
	{
		curl_easy_setopt(curlHandle, CURLOPT_MAXFILESIZE_LARGE, options.maxFilesize);
	}

	impl->client->OnSetupCurlHandle(curlHandle, url);

	return curlData;
}

// FiveM's HTTP through the native Linux helper (wine-fivem fivem-helper, "req" transfers) when FiveM
// runs under Wine from the native launcher (CitizenFX_NativeHelper=127.0.0.1:port, and the
// launcher's token in CitizenFX_NativeHelperToken): the system libcurl outside Wine makes the
// request with the options of the curl handle HttpClient prepared, and streams the response back;
// its headers, body and result reach the request's callbacks on HttpClient's loop, as they would
// from curl. Without the variable, or when the helper cannot be reached, requests use curl here.
namespace native_http
{
using json = nlohmann::json;

struct Helper
{
	std::once_flag init;
	std::atomic<bool> active = false;
	SOCKET sock = INVALID_SOCKET;
	std::mutex sendLock;
	std::mutex lock;
	std::map<uint64_t, std::shared_ptr<CurlData>> requests;
	std::atomic<uint64_t> nextId = 1;
};

static Helper g_helper;

static bool SendAll(SOCKET s, const char* data, size_t size)
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

static bool RecvAll(SOCKET s, char* data, size_t size)
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

static bool Send(const json& message, const std::string& body = {})
{
	std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
	uint32_t sizes[2] = { uint32_t(text.size()), uint32_t(body.size()) };

	std::unique_lock _(g_helper.sendLock);
	if (!g_helper.active)
	{
		return false;
	}
	if (!SendAll(g_helper.sock, (const char*)sizes, sizeof(sizes)) || !SendAll(g_helper.sock, text.data(), text.size()) ||
		(!body.empty() && !SendAll(g_helper.sock, body.data(), body.size())))
	{
		trace("Native HTTP: helper connection lost\n");
		g_helper.active = false;
		return false;
	}
	return true;
}

// runs on the request's HttpClient loop, after what was queued before (keeps the helper's order)
static void PostToLoop(HttpClientImpl* impl, std::function<void()>&& fn)
{
	impl->cbsToRun.push(std::move(fn));

	std::shared_lock<std::shared_mutex> _(impl->mutex);
	if (impl->runCb)
	{
		impl->runCb->send();
	}
}

// as CurlData::HandleResult, with the helper's result; then the unused curl handle is released
static void Finish(const std::shared_ptr<CurlData>& cd, int curlCode, long status, const std::string& error)
{
	if (cd->nativeFinished)
	{
		return;
	}
	cd->nativeFinished = true;

	if (cd->preCallback)
	{
		cd->preCallback();
		cd->preCallback = {};
	}

	if (cd->callback)
	{
		if (curlCode != 0)
		{
			cd->callback(false, error);
		}
		else
		{
			if (cd->responseCode)
			{
				*cd->responseCode = int(status);
			}

			if (status >= 400)
			{
				auto failure = fmt::sprintf("HTTP %d%s", status, cd->addErrorBody ? fmt::sprintf(": %s", cd->errorBody.str()) : "");
				cd->callback(false, failure);
			}
			else
			{
				auto str = cd->ss.str();
				cd->callback(true, str);
			}
		}

		cd->callback = {};
	}

	if (cd->curlHandle)
	{
		FinalizeCurlHandle(cd->curlHandle);
	}
}

// as CurlData::HandleWrite
static bool Write(const std::shared_ptr<CurlData>& cd, const std::string& data)
{
	if (cd->addErrorBody && cd->nativeStatus >= 400)
	{
		cd->errorBody << data;
	}

	if (cd->addRawBody)
	{
		cd->rawBody << data;
	}

	return cd->writeFunction(data.data(), data.size()) == data.size();
}

static std::shared_ptr<CurlData> Take(uint64_t id, bool remove)
{
	std::unique_lock _(g_helper.lock);
	auto it = g_helper.requests.find(id);
	if (it == g_helper.requests.end())
	{
		return {};
	}
	auto cd = it->second;
	if (remove)
	{
		g_helper.requests.erase(it);
	}
	return cd;
}

static void FailAll(const std::string& why)
{
	std::map<uint64_t, std::shared_ptr<CurlData>> requests;
	{
		std::unique_lock _(g_helper.lock);
		requests.swap(g_helper.requests);
	}
	for (auto& [id, cd] : requests)
	{
		PostToLoop(cd->impl, [cd, why]()
		{
			Finish(cd, CURLE_COULDNT_CONNECT, 0, why);
		});
	}
}

static void Reader(SOCKET s)
{
	SetThreadName(-1, "NativeHttp");

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

		if (type == "hello")
		{
			continue;
		}

		auto cd = Take(id, type == "done");
		if (!cd)
		{
			continue;		// cancelled, or finished here (a write failure)
		}

		if (type == "head")
		{
			PostToLoop(cd->impl, [cd, message]()
			{
				cd->nativeStatus = message.value("status", 0L);

				// as CurlHeaderInfo: the final response's headers
				if (cd->responseHeaders)
				{
					cd->responseHeaders->clear();
					for (const auto& header : message.value("headers", json::array()))
					{
						if (header.is_array() && header.size() == 2)
						{
							cd->responseHeaders->emplace(header[0].get<std::string>(), header[1].get<std::string>());
						}
					}
				}
			});
		}
		else if (type == "data")
		{
			PostToLoop(cd->impl, [cd, data = std::move(body)]()
			{
				if (cd->nativeFinished)
				{
					return;
				}
				if (!Write(cd, data))
				{
					// as curl when the write function takes less than it is given
					{
						std::unique_lock _(g_helper.lock);
						g_helper.requests.erase(cd->nativeId);
					}
					Send(json{ { "t", "cancel" }, { "id", cd->nativeId } });
					Finish(cd, CURLE_WRITE_ERROR, cd->nativeStatus,
						"Failure writing output to destination - CURL error code 23 (Failure writing output to destination)");
				}
			});
		}
		else if (type == "progress")
		{
			PostToLoop(cd->impl, [cd, now = message.value("now", uint64_t(0)), total = message.value("total", uint64_t(0))]()
			{
				if (cd->progressCallback && !cd->nativeFinished)
				{
					ProgressInfo info;
					info.downloadNow = now;
					info.downloadTotal = total;
					cd->progressCallback(info);
				}
			});
		}
		else if (type == "done")
		{
			PostToLoop(cd->impl, [cd, curlCode = message.value("curl", 0), status = message.value("status", 0L),
								  error = message.value("error", std::string())]()
			{
				Finish(cd, curlCode, status, error);
			});
		}
	}

	trace("Native HTTP: helper connection closed\n");
	g_helper.active = false;
	FailAll("native HTTP helper disconnected - CURL error code 7 (Couldn't connect to server)");
}

static void Connect()
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
		trace("Native HTTP: cannot connect to the helper at %s (%d), using curl here\n", address, WSAGetLastError());
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

	char token[128];
	length = GetEnvironmentVariableA("CitizenFX_NativeHelperToken", token, sizeof(token));
	if (length && length < sizeof(token))
	{
		Send(json{ { "t", "auth" }, { "token", std::string(token, length) } });
	}

	std::thread(Reader, s).detach();
	trace("Native HTTP: helper connected at %s\n", address);
}

// true: the helper makes the request (its answers drive cd's callbacks)
static bool Submit(const std::shared_ptr<CurlData>& cd, const std::string& userAgent)
{
	if (!GetEnvironmentVariableA("CitizenFX_NativeHelper", nullptr, 0))
	{
		return false;
	}

	std::call_once(g_helper.init, Connect);
	if (!g_helper.active)
	{
		return false;
	}

	uint64_t id = g_helper.nextId++;
	cd->nativeId = id;
	{
		std::unique_lock _(g_helper.lock);
		g_helper.requests[id] = cd;
	}

	json headers = json::object();
	for (const auto& [key, value] : cd->headers)
	{
		headers[key] = value;
	}

	json request = {
		{ "t", "req" }, { "id", id }, { "url", cd->url }, { "method", cd->method }, { "post", cd->post },
		{ "headers", headers }, { "user_agent", userAgent }, { "follow", cd->followLocation }, { "ipv4", cd->ipv4 },
		{ "max_size", cd->maxFilesize }, { "timeout_ms", int64_t(cd->timeoutNoResponse.count()) },
		{ "weight", cd->weight }, { "progress", bool(cd->progressCallback) }
	};

	if (!Send(request, cd->post ? cd->postData : std::string()))
	{
		std::unique_lock _(g_helper.lock);
		g_helper.requests.erase(id);
		cd->nativeId = 0;
		return false;
	}
	return true;
}

static void Cancel(const std::shared_ptr<CurlData>& cd)
{
	if (!Take(cd->nativeId, true))
	{
		return;
	}
	Send(json{ { "t", "cancel" }, { "id", cd->nativeId } });

	// as Abort with curl: no callback, the handle is released
	PostToLoop(cd->impl, [cd]()
	{
		if (!cd->nativeFinished)
		{
			cd->nativeFinished = true;
			if (cd->curlHandle)
			{
				FinalizeCurlHandle(cd->curlHandle);
			}
		}
	});
}

static void SetWeight(const std::shared_ptr<CurlData>& cd, int weight)
{
	Send(json{ { "t", "weight" }, { "id", cd->nativeId }, { "weight", weight } });
}
}

class HttpRequestHandleImpl final : public ManualHttpRequestHandle
{
public:
	HttpRequestHandleImpl(const std::shared_ptr<CurlData>& reqData)
		: m_request(reqData)
	{

	}

	virtual ~HttpRequestHandleImpl()
	{
		if (!m_started)
		{
			FinalizeCurlHandle(m_request->curlHandle);
		}
	}

	virtual bool HasCompleted() override
	{
		return m_request->curlHandle == nullptr;
	}

	virtual void SetRequestWeight(int weight) override
	{
		int newWeight = weight;

		if (newWeight == -1)
		{
			newWeight = m_request->defaultWeight;
		}

		if (m_request->weight == newWeight)
		{
			return;
		}

		auto request = m_request;
		request->weight = newWeight;

		if (request->nativeId)
		{
			native_http::SetWeight(request, newWeight);
			return;
		}

		request->impl->cbsToRun.push([request, newWeight]()
		{
			if (request->curlHandle)
			{
				curl_easy_setopt(request->curlHandle, CURLOPT_STREAM_WEIGHT, long(newWeight));
			}
		});

		std::shared_lock<std::shared_mutex> _(request->impl->mutex);
		if (request->impl->runCb)
		{
			request->impl->runCb->send();
		}
	}

	virtual void Abort() override
	{
		auto request = m_request;

		if (request->nativeId)
		{
			native_http::Cancel(request);
			return;
		}

		request->impl->cbsToRun.push([request]()
		{
			auto curl = request->curlHandle;
			auto impl = request->impl;

			//Request has completed and curl has been cleaned up.
			if (curl == nullptr)
				return;

			FinalizeCurlHandle(curl, impl->multi);
		});

		std::shared_lock<std::shared_mutex> _(request->impl->mutex);
		if (request->impl->runCb)
		{
			request->impl->runCb->send();
		}
	}

	virtual std::string GetRawBody() override
	{
		return m_request->rawBody.str();
	}

	virtual void Start() override
	{
		m_started = true;

		if (native_http::Submit(m_request, m_request->impl->userAgent))
		{
			return;
		}

		m_request->impl->AddCurlHandle(m_request->curlHandle);
	}

	virtual void OnCompletion(std::function<void(bool, std::string_view)>&& callback) override
	{
		m_request->callback = std::move(callback);
	}

private:
	std::shared_ptr<CurlData> m_request;

	bool m_started = false;
};

static ManualHttpRequestPtr SetupRequestHandle(const std::shared_ptr<CurlData>& data)
{
	return std::make_shared<HttpRequestHandleImpl>(data);
}

HttpRequestPtr HttpClient::DoGetRequest(const std::wstring& host, uint16_t port, const std::wstring& url, const std::function<void(bool, const char*, size_t)>& callback)
{
	auto urlStr = MakeURL(host, port, url);

	return DoGetRequest(urlStr, callback);
}

HttpRequestPtr HttpClient::DoGetRequest(const std::string& url, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoGetRequest(url, {}, callback);
}

ManualHttpRequestPtr HttpClient::Get(const std::string& url, const HttpRequestOptions& options /* = */)
{
	auto curlData = SetupCURLHandle(m_impl, url, options);
	return SetupRequestHandle(curlData);
}

HttpRequestPtr HttpClient::DoGetRequest(const std::string& url, const HttpRequestOptions& options, const std::function<void(bool, const char *, size_t)>& callback)
{
	auto requestHandle = Get(url, options);

	requestHandle->OnCompletion([callback](bool success, std::string_view data)
	{
		callback(success, data.data(), data.size());
	});

	requestHandle->Start();

	return requestHandle;
}

HttpRequestPtr HttpClient::DoPostRequest(const std::wstring& host, uint16_t port, const std::wstring& url, const std::map<std::string, std::string>& fields, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoPostRequest(host, port, url, BuildPostString(fields), callback);
}

HttpRequestPtr HttpClient::DoPostRequest(const std::wstring& host, uint16_t port, const std::wstring& url, const std::string& postData, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoPostRequest(host, port, url, postData, {}, callback);
}

HttpRequestPtr HttpClient::DoPostRequest(const std::wstring& host, uint16_t port, const std::wstring& url, const std::string& postData, const fwMap<fwString, fwString>& headersMap, const std::function<void(bool, const char*, size_t)>& callback, std::function<void(const std::map<std::string, std::string>&)> headerCallback /*= std::function<void(const std::map<std::string, std::string>&)>()*/)
{
	auto urlStr = MakeURL(host, port, url);

	return DoPostRequest(urlStr, postData, headersMap, callback, headerCallback);
}

HttpRequestPtr HttpClient::DoPostRequest(const std::string& url, const std::map<std::string, std::string>& fields, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoPostRequest(url, BuildPostString(fields), callback);
}

HttpRequestPtr HttpClient::DoPostRequest(const std::string& url, const std::map<std::string, std::string>& fields, const HttpRequestOptions& options, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoPostRequest(url, BuildPostString(fields), options, callback);
}

HttpRequestPtr HttpClient::DoPostRequest(const std::string& url, const std::string& postData, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoPostRequest(url, postData, HttpRequestOptions{}, callback);
}

HttpRequestPtr HttpClient::DoPostRequest(const std::string& url, const std::string& postData, const fwMap<fwString, fwString>& headersMap, const std::function<void(bool, const char*, size_t)>& callback, std::function<void(const std::map<std::string, std::string>&)> headerCallback /*= std::function<void(const std::map<std::string, std::string>&)>()*/)
{
	HttpRequestOptions options;
	options.headers = headersMap;

	return DoPostRequest(url, postData, options, callback, headerCallback);
}

HttpRequestPtr HttpClient::DoPostRequest(const std::string& url, const std::string& postData, const HttpRequestOptions& options, const std::function<void(bool, const char*, size_t)>& callback, std::function<void(const std::map<std::string, std::string>&)> headerCallback /*= std::function<void(const std::map<std::string, std::string>&)>()*/)
{
	// make handle
	auto curlData = SetupCURLHandle(m_impl, url, options);

	// assign post data
	curlData->postData = postData;
	curlData->post = true;

	curl_easy_setopt(curlData->curlHandle, CURLOPT_POSTFIELDS, curlData->postData.c_str());

	// write out
	auto requestHandle = SetupRequestHandle(curlData);

	requestHandle->OnCompletion([callback](bool success, std::string_view data)
	{
		callback(success, data.data(), data.size());
	});

	requestHandle->Start();

	return requestHandle;
}

HttpRequestPtr HttpClient::DoMethodRequest(const std::string& method, const std::string& url, const std::string& postData, const HttpRequestOptions& options, const std::function<void(bool, const char*, size_t)>& callback, std::function<void(const std::map<std::string, std::string>&)> headerCallback /*= std::function<void(const std::map<std::string, std::string>&)>()*/)
{
	// make handle
	auto curlData = SetupCURLHandle(m_impl, url, options);

	if (!postData.empty())
	{
		// assign post data
		curlData->postData = postData;
		curlData->post = true;

		curl_easy_setopt(curlData->curlHandle, CURLOPT_POSTFIELDS, curlData->postData.c_str());
	}

	curl_easy_setopt(curlData->curlHandle, CURLOPT_CUSTOMREQUEST, method.c_str());
	curlData->method = method;

	// write out
	auto requestHandle = SetupRequestHandle(curlData);

	requestHandle->OnCompletion([callback](bool success, std::string_view data)
	{
		callback(success, data.data(), data.size());
	});

	requestHandle->Start();

	return requestHandle;
}

HttpRequestPtr HttpClient::DoFileGetRequest(const std::wstring& host, uint16_t port, const std::wstring& url, const char* outDeviceBase, const std::string& outFilename, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoFileGetRequest(host, port, url, vfs::GetDevice(outDeviceBase), outFilename, callback);
}

HttpRequestPtr HttpClient::DoFileGetRequest(const std::string& url, const char* outDeviceBase, const std::string& outFilename, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoFileGetRequest(url, vfs::GetDevice(outDeviceBase), outFilename, callback);
}

HttpRequestPtr HttpClient::DoFileGetRequest(const std::wstring& host, uint16_t port, const std::wstring& url, fwRefContainer<vfs::Device> outDevice, const std::string& outFilename, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoFileGetRequest(MakeURL(host, port, url), outDevice, outFilename, callback);
}

HttpRequestPtr HttpClient::DoFileGetRequest(const std::string& urlStr, fwRefContainer<vfs::Device> outDevice, const std::string& outFilename, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoFileGetRequest(urlStr, outDevice, outFilename, {}, callback);
}

HttpRequestPtr HttpClient::DoFileGetRequest(const std::string& urlStr, fwRefContainer<vfs::Device> outDevice, const std::string& outFilename, const HttpRequestOptions& options, const std::function<void(bool, const char*, size_t)>& callback)
{
	auto curlData = SetupCURLHandle(m_impl, urlStr, options);

	auto handle = outDevice->Create(outFilename);

	curlData->writeFunction = [=] (const void* data, size_t length)
	{
		return outDevice->Write(handle, data, length);
	};

	curlData->preCallback = [=] ()
	{
		outDevice->Close(handle);
	};

	auto requestHandle = SetupRequestHandle(curlData);

	requestHandle->OnCompletion([callback](bool success, std::string_view data)
	{
		callback(success, data.data(), data.size());
	});

	requestHandle->Start();

	return requestHandle;
}

HttpRequestPtr HttpClient::DoFileGetRequest(const std::wstring& host, uint16_t port, const std::wstring& url, rage::fiDevice* outDevice, const std::string& outFilename, const std::function<void(bool, const char*, size_t)>& callback)
{
	return DoFileGetRequest(host, port, url, vfs::GetNativeDevice(outDevice), outFilename, callback);
}

#if defined(COMPILING_HTTP_CLIENT)
static InitFunction initFunction([]()
{
	Instance<HttpClient>::Set(new HttpClient());
});
#endif
