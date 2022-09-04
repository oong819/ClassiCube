#include "Core.h"
#if defined CC_BUILD_WININET && !defined CC_BUILD_CURL
#include "_HttpWorker.h"

#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include "Errors.h"

/* === BEGIN wininet.h === */
#define INETAPI DECLSPEC_IMPORT
typedef PVOID HINTERNET;
typedef WORD INTERNET_PORT;

#define INTERNET_OPEN_TYPE_PRECONFIG  0   // use registry configuration
#define INTERNET_OPEN_TYPE_DIRECT     1   // direct to net
#define INTERNET_SERVICE_HTTP         3

#define HTTP_ADDREQ_FLAG_ADD         0x20000000
#define HTTP_ADDREQ_FLAG_REPLACE     0x80000000

#define INTERNET_FLAG_RELOAD         0x80000000
#define INTERNET_FLAG_NO_CACHE_WRITE 0x04000000
#define INTERNET_FLAG_SECURE         0x00800000
#define INTERNET_FLAG_NO_COOKIES     0x00080000
#define INTERNET_FLAG_NO_UI          0x00000200

#define SECURITY_FLAG_IGNORE_REVOCATION      0x00000080
#define SECURITY_FLAG_IGNORE_UNKNOWN_CA      0x00000100
#define SECURITY_FLAG_IGNORE_CERT_CN_INVALID 0x00001000
#define HTTP_QUERY_RAW_HEADERS          21
#define INTERNET_OPTION_SECURITY_FLAGS  31

static BOOL      (WINAPI *_InternetCloseHandle)(HINTERNET hInternet);
static HINTERNET (WINAPI *_InternetConnectA)(HINTERNET hInternet, PCSTR serverName, INTERNET_PORT serverPort, PCSTR userName, PCSTR password, DWORD service, DWORD flags, DWORD_PTR context);
static HINTERNET (WINAPI *_InternetOpenA)(PCSTR agent, DWORD accessType, PCSTR lpszProxy, PCSTR proxyBypass, DWORD flags);
static BOOL      (WINAPI *_InternetQueryOptionA)(HINTERNET hInternet, DWORD option, PVOID buffer, DWORD* bufferLength);
static BOOL      (WINAPI *_InternetSetOptionA)(HINTERNET hInternet, DWORD option, PVOID buffer, DWORD bufferLength);
static BOOL      (WINAPI *_InternetQueryDataAvailable)(HINTERNET hFile, DWORD* numBytesAvailable, DWORD flags, DWORD_PTR context);
static BOOL      (WINAPI *_InternetReadFile)(HINTERNET hFile, PVOID buffer, DWORD numBytesToRead, DWORD* numBytesRead);
static BOOL      (WINAPI *_HttpQueryInfoA)(HINTERNET hRequest, DWORD infoLevel, PVOID buffer, DWORD* bufferLength, DWORD* index);
static BOOL      (WINAPI *_HttpAddRequestHeadersA)(HINTERNET hRequest, PCSTR headers, DWORD headersLength, DWORD modifiers);
static HINTERNET (WINAPI *_HttpOpenRequestA)(HINTERNET hConnect, PCSTR verb, PCSTR objectName, PCSTR version, PCSTR referrer, PCSTR* acceptTypes, DWORD flags, DWORD_PTR context);
static BOOL      (WINAPI *_HttpSendRequestA)(HINTERNET hRequest, PCSTR headers, DWORD headersLength, PVOID optional, DWORD optionalLength);
/* === END wininet.h === */

static void* wininet_lib;
cc_bool Http_DescribeError(cc_result res, cc_string* dst) {
	return Platform_DescribeErrorExt(res, dst, wininet_lib);
}


/* caches connections to web servers */
struct HttpCacheEntry {
	HINTERNET Handle;  /* Native connection handle. */
	cc_string Address; /* Address of server. (e.g. "classicube.net") */
	cc_uint16 Port;    /* Port server is listening on. (e.g 80) */
	cc_bool Https;     /* Whether HTTPS or just HTTP protocol. */
	char _addressBuffer[STRING_SIZE + 1];
};
#define HTTP_CACHE_ENTRIES 10
static struct HttpCacheEntry http_cache[HTTP_CACHE_ENTRIES];
static HINTERNET hInternet;

/* Converts characters to UTF8, then calls Http_URlEncode on them. */
static void HttpCache_UrlEncodeUrl(cc_string* dst, const cc_string* src) {
	cc_uint8 data[4];
	int i, len;
	char c;

	for (i = 0; i < src->length; i++) {
		c   = src->buffer[i];
		len = Convert_CP437ToUtf8(c, data);

		/* URL path/query must not be URL encoded (it normally would be) */
		if (c == '/' || c == '?' || c == '=') {
			String_Append(dst, c);
		} else {
			Http_UrlEncode(dst, data, len);
		}
	}
}

/* Splits up the components of a URL */
static void HttpCache_MakeEntry(const cc_string* url, struct HttpCacheEntry* entry, cc_string* resource) {
	cc_string scheme, path, addr, name, port, _resource;
	/* URL is of form [scheme]://[server name]:[server port]/[resource] */
	int idx = String_IndexOfConst(url, "://");

	scheme = idx == -1 ? String_Empty : String_UNSAFE_Substring(url,   0, idx);
	path   = idx == -1 ? *url         : String_UNSAFE_SubstringAt(url, idx + 3);
	entry->Https = String_CaselessEqualsConst(&scheme, "https");

	String_UNSAFE_Separate(&path, '/', &addr, &_resource);
	String_UNSAFE_Separate(&addr, ':', &name, &port);

	String_Append(resource, '/');
	/* Address may have unicode characters - need to percent encode them */
	HttpCache_UrlEncodeUrl(resource, &_resource);

	String_InitArray_NT(entry->Address, entry->_addressBuffer);
	String_Copy(&entry->Address, &name);
	entry->Address.buffer[entry->Address.length] = '\0';

	if (!Convert_ParseUInt16(&port, &entry->Port)) {
		entry->Port = entry->Https ? 443 : 80;
	}
}

/* Inserts entry into the cache at the given index */
static cc_result HttpCache_Insert(int i, struct HttpCacheEntry* e) {
	HINTERNET conn;
	conn = _InternetConnectA(hInternet, e->Address.buffer, e->Port, NULL, NULL, 
				INTERNET_SERVICE_HTTP, e->Https ? INTERNET_FLAG_SECURE : 0, 0);
	if (!conn) return GetLastError();

	e->Handle     = conn;
	http_cache[i] = *e;

	/* otherwise address buffer points to stack buffer */
	http_cache[i].Address.buffer = http_cache[i]._addressBuffer;
	return 0;
}

/* Finds or inserts the given entry into the cache */
static cc_result HttpCache_Lookup(struct HttpCacheEntry* e) {
	struct HttpCacheEntry* c;
	int i;

	for (i = 0; i < HTTP_CACHE_ENTRIES; i++) {
		c = &http_cache[i];
		if (c->Https == e->Https && String_Equals(&c->Address, &e->Address) && c->Port == e->Port) {
			e->Handle = c->Handle;
			return 0;
		}
	}

	for (i = 0; i < HTTP_CACHE_ENTRIES; i++) {
		if (http_cache[i].Handle) continue;
		return HttpCache_Insert(i, e);
	}

	/* TODO: Should we be consistent in which entry gets evicted? */
	i = (cc_uint8)Stopwatch_Measure() % HTTP_CACHE_ENTRIES;
	_InternetCloseHandle(http_cache[i].Handle);
	return HttpCache_Insert(i, e);
}


static void Http_AddHeader(struct HttpRequest* req, const char* key, const cc_string* value) {
	cc_string tmp; char tmpBuffer[1024];
	String_InitArray(tmp, tmpBuffer);

	String_Format2(&tmp, "%c: %s\r\n", key, value);
	_HttpAddRequestHeadersA((HINTERNET)req->meta, tmp.buffer, tmp.length, 
							HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
}

/* Creates and sends a HTTP request */
static cc_result Http_StartRequest(struct HttpRequest* req, cc_string* url) {
	static const char* verbs[3] = { "GET", "HEAD", "POST" };
	struct HttpCacheEntry entry;
	cc_string path; char pathBuffer[URL_MAX_SIZE + 1];
	DWORD flags, bufferLen;
	HINTERNET handle;
	cc_result res;

	String_InitArray_NT(path, pathBuffer);
	HttpCache_MakeEntry(url, &entry, &path);
	pathBuffer[path.length] = '\0';

	if (!wininet_lib) return ERR_NOT_SUPPORTED;
	if ((res = HttpCache_Lookup(&entry))) return res;

	flags = INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_COOKIES;
	if (entry.Https) flags |= INTERNET_FLAG_SECURE;

	handle = _HttpOpenRequestA(entry.Handle, verbs[req->requestType], pathBuffer, NULL, NULL, NULL, flags, 0);
	/* Fallback for Windows 95 which returns ERROR_INVALID_PARAMETER */
	if (!handle) {
		flags &= ~INTERNET_FLAG_NO_UI; /* INTERNET_FLAG_NO_UI unsupported on Windows 95 */
		handle = _HttpOpenRequestA(entry.Handle, verbs[req->requestType], pathBuffer, NULL, NULL, NULL, flags, 0);
		if (!handle) return GetLastError();
	}
	req->meta = handle;

	bufferLen = sizeof(flags);
	_InternetQueryOptionA(handle, INTERNET_OPTION_SECURITY_FLAGS, (void*)&bufferLen, &flags);
	/* Users have had issues in the past with revocation servers randomly being offline, */
	/*  which caused all https:// requests to fail. So just skip revocation check. */
	flags |= SECURITY_FLAG_IGNORE_REVOCATION;

	if (!httpsVerify) flags |= SECURITY_FLAG_IGNORE_UNKNOWN_CA;
	_InternetSetOptionA(handle,   INTERNET_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

	Http_SetRequestHeaders(req);
	return _HttpSendRequestA(handle, NULL, 0, req->data, req->size) ? 0 : GetLastError();
}

/* Gets headers from a HTTP response */
static cc_result Http_ProcessHeaders(struct HttpRequest* req, HINTERNET handle) {
	cc_string left, line;
	char buffer[8192];
	DWORD len = 8192;
	if (!_HttpQueryInfoA(handle, HTTP_QUERY_RAW_HEADERS, buffer, &len, NULL)) return GetLastError();

	left = String_Init(buffer, len, len);
	while (left.length) {
		String_UNSAFE_SplitBy(&left, '\0', &line);
		Http_ParseHeader(req, &line);
	}
	return 0;
}

/* Downloads the data/contents of a HTTP response */
static cc_result Http_DownloadData(struct HttpRequest* req, HINTERNET handle) {
	DWORD read, avail;
	Http_BufferInit(req);

	for (;;) {
		if (!_InternetQueryDataAvailable(handle, &avail, 0, 0)) return GetLastError();
		if (!avail) break;
		Http_BufferEnsure(req, avail);

		if (!_InternetReadFile(handle, &req->data[req->size], avail, &read)) return GetLastError();
		if (!read) break;
		Http_BufferExpanded(req, read);
	}

 	http_curProgress = 100;
	return 0;
}

static cc_result HttpBackend_Do(struct HttpRequest* req, cc_string* url) {
	HINTERNET handle;
	cc_result res = Http_StartRequest(req, url);
	HttpRequest_Free(req);
	if (res) return res;

	handle = req->meta;
	http_curProgress = HTTP_PROGRESS_FETCHING_DATA;
	res = Http_ProcessHeaders(req, handle);
	if (res) { _InternetCloseHandle(handle); return res; }

	if (req->requestType != REQUEST_TYPE_HEAD) {
		res = Http_DownloadData(req, handle);
		if (res) { _InternetCloseHandle(handle); return res; }
	}

	return _InternetCloseHandle(handle) ? 0 : GetLastError();
}


static void HttpBackend_Init(void) {
	static const struct DynamicLibSym funcs[] = {
		DynamicLib_Sym(InternetCloseHandle), 
		DynamicLib_Sym(InternetConnectA),   DynamicLib_Sym(InternetOpenA),       
		DynamicLib_Sym(InternetSetOptionA), DynamicLib_Sym(InternetQueryOptionA),
		DynamicLib_Sym(InternetReadFile),   DynamicLib_Sym(InternetQueryDataAvailable),
		DynamicLib_Sym(HttpOpenRequestA),   DynamicLib_Sym(HttpSendRequestA),   
		DynamicLib_Sym(HttpQueryInfoA),     DynamicLib_Sym(HttpAddRequestHeadersA)

	};
	static const cc_string wininet = String_FromConst("wininet.dll");
	DynamicLib_LoadAll(&wininet, funcs, Array_Elems(funcs), &wininet_lib);
	if (!wininet_lib) return;

	/* TODO: Should we use INTERNET_OPEN_TYPE_PRECONFIG instead? */
	hInternet = _InternetOpenA(GAME_APP_NAME, INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
	if (!hInternet) Logger_Abort2(GetLastError(), "Failed to init WinINet");
}
#elif defined CC_BUILD_WINHTTP
#include "_HttpWorker.h"

#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include "Errors.h"
#include <winhttp.h>
#pragma comment(lib, "WinHttp")

static void* winhttp_lib;
cc_bool Http_DescribeError(cc_result res, cc_string* dst) {
	return Platform_DescribeErrorExt(res, dst, winhttp_lib);
}


/* caches connections to web servers */
struct HttpCacheEntry {
	HINTERNET Handle;  /* Native connection handle. */
	cc_string Address; /* Address of server. (e.g. "classicube.net") */
	cc_uint16 Port;    /* Port server is listening on. (e.g 80) */
	cc_bool Https;     /* Whether HTTPS or just HTTP protocol. */
	char _addressBuffer[STRING_SIZE + 1];
};
#define HTTP_CACHE_ENTRIES 10
static struct HttpCacheEntry http_cache[HTTP_CACHE_ENTRIES];
static HINTERNET hInternet;

/* Splits up the components of a URL */
static void HttpCache_MakeEntry(const cc_string* url, struct HttpCacheEntry* entry, cc_string* resource) {
	cc_string scheme, path, addr, name, port, _resource;
	/* URL is of form [scheme]://[server name]:[server port]/[resource] */
	int idx = String_IndexOfConst(url, "://");

	scheme = idx == -1 ? String_Empty : String_UNSAFE_Substring(url,   0, idx);
	path   = idx == -1 ? *url         : String_UNSAFE_SubstringAt(url, idx + 3);
	entry->Https = String_CaselessEqualsConst(&scheme, "https");

	String_UNSAFE_Separate(&path, '/', &addr, &_resource);
	String_UNSAFE_Separate(&addr, ':', &name, &port);

	String_Append(resource, '/');
	String_Copy(resource, &_resource);

	String_InitArray_NT(entry->Address, entry->_addressBuffer);
	String_Copy(&entry->Address, &name);
	entry->Address.buffer[entry->Address.length] = '\0';

	if (!Convert_ParseUInt16(&port, &entry->Port)) {
		entry->Port = entry->Https ? 443 : 80;
	}
}

/* Inserts entry into the cache at the given index */
static cc_result HttpCache_Insert(int i, struct HttpCacheEntry* e) {
	WCHAR address[NATIVE_STR_LEN];
	HINTERNET conn;

	Platform_EncodeUtf16(address, &e->Address);
	conn = WinHttpConnect(hInternet, address, e->Port, 0);
	if (!conn) return GetLastError();

	e->Handle     = conn;
	http_cache[i] = *e;

	/* otherwise address buffer points to stack buffer */
	http_cache[i].Address.buffer = http_cache[i]._addressBuffer;
	return 0;
}

/* Finds or inserts the given entry into the cache */
static cc_result HttpCache_Lookup(struct HttpCacheEntry* e) {
	struct HttpCacheEntry* c;
	int i;

	for (i = 0; i < HTTP_CACHE_ENTRIES; i++) {
		c = &http_cache[i];
		if (c->Https == e->Https && String_Equals(&c->Address, &e->Address) && c->Port == e->Port) {
			e->Handle = c->Handle;
			return 0;
		}
	}

	for (i = 0; i < HTTP_CACHE_ENTRIES; i++) {
		if (http_cache[i].Handle) continue;
		return HttpCache_Insert(i, e);
	}

	/* TODO: Should we be consistent in which entry gets evicted? */
	i = (cc_uint8)Stopwatch_Measure() % HTTP_CACHE_ENTRIES;
	WinHttpCloseHandle(http_cache[i].Handle);
	return HttpCache_Insert(i, e);
}


static void Http_AddHeader(struct HttpRequest* req, const char* key, const cc_string* value) {
	cc_string tmp; char tmpBuffer[1024];
	WCHAR headers[NATIVE_STR_LEN];
	String_InitArray(tmp, tmpBuffer);

	String_Format2(&tmp, "%c: %s", key, value);
	Platform_EncodeUtf16(headers, &tmp);

	WinHttpAddRequestHeaders((HINTERNET)req->meta, headers, tmp.length,
							WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

/* Creates and sends a HTTP request */
static cc_result Http_StartRequest(struct HttpRequest* req, cc_string* url) {
	static const WCHAR* verbs[3] = { L"GET", L"HEAD", L"POST" };
	struct HttpCacheEntry entry;
	cc_string path; char pathBuffer[URL_MAX_SIZE + 1];
	WCHAR location[NATIVE_STR_LEN];
	DWORD flags, bufferLen;
	HINTERNET handle;
	cc_result res;

	String_InitArray_NT(path, pathBuffer);
	HttpCache_MakeEntry(url, &entry, &path);
	Platform_EncodeUtf16(location, &path);

	if ((res = HttpCache_Lookup(&entry))) return res;

	handle = WinHttpOpenRequest(entry.Handle, verbs[req->requestType], location, NULL, NULL, NULL, 
								entry.Https ? WINHTTP_FLAG_SECURE : 0);
	req->meta = handle;

	flags = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
	WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags));

	bufferLen = sizeof(flags);
	WinHttpQueryOption(handle, WINHTTP_OPTION_SECURITY_FLAGS, (void*)&bufferLen, &flags);

	if (!httpsVerify) flags |= SECURITY_FLAG_IGNORE_UNKNOWN_CA;
	WinHttpSetOption(handle,   WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

	Http_SetRequestHeaders(req);
	return WinHttpSendRequest(handle, NULL, 0, req->data, req->size, req->size, 0) ? 0 : GetLastError();
}

/* Gets headers from a HTTP response */
static cc_result Http_ProcessHeaders(struct HttpRequest* req, HINTERNET handle) {
	cc_string left, line;
	WCHAR buffer[8192];
	DWORD len = 8192;
	char tmp[8192];

	if (!WinHttpReceiveResponse(handle, NULL)) return GetLastError();
	if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS, NULL, buffer, &len, NULL)) return GetLastError();

	String_InitArray(left, tmp);
	String_AppendUtf16(&left, buffer, len);

	while (left.length) {
		String_UNSAFE_SplitBy(&left, '\0', &line);
		Http_ParseHeader(req, &line);
	}
	return 0;
}

/* Downloads the data/contents of a HTTP response */
static cc_result Http_DownloadData(struct HttpRequest* req, HINTERNET handle) {
	DWORD read, avail;
	Http_BufferInit(req);

	for (;;) {
		if (!WinHttpQueryDataAvailable(handle, &avail)) return GetLastError();
		if (!avail) break;
		Http_BufferEnsure(req, avail);

		if (!WinHttpReadData(handle, &req->data[req->size], avail, &read)) return GetLastError();
		if (!read) break;
		Http_BufferExpanded(req, read);
	}

 	http_curProgress = 100;
	return 0;
}

static cc_result HttpBackend_Do(struct HttpRequest* req, cc_string* url) {
	HINTERNET handle;
	cc_result res = Http_StartRequest(req, url);
	HttpRequest_Free(req);
	if (res) return res;

	handle = req->meta;
	http_curProgress = HTTP_PROGRESS_FETCHING_DATA;
	res = Http_ProcessHeaders(req, handle);
	if (res) { WinHttpCloseHandle(handle); return res; }

	if (req->requestType != REQUEST_TYPE_HEAD) {
		res = Http_DownloadData(req, handle);
		if (res) { WinHttpCloseHandle(handle); return res; }
	}

	return WinHttpCloseHandle(handle) ? 0 : GetLastError();
}


static void HttpBackend_Init(void) {
	static const cc_string winhttp = String_FromConst("winhttp.dll");
	cc_string appName = String_FromConst(GAME_APP_NAME);
	WCHAR appBuffer[NATIVE_STR_LEN];
	DWORD flags;
	Platform_EncodeUtf16(appBuffer, &appName);

	winhttp_lib = DynamicLib_Load2(&winhttp);

	/* TODO: Should we use WINHTTP_ACCESS_TYPE_DEFAULT_PROXY instead? */
	hInternet = WinHttpOpen(appBuffer, WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
	if (!hInternet) Logger_Abort2(GetLastError(), "Failed to init WinHttp");

	/* Older systems might only support up to TLS 1.0 */
    flags = WINHTTP_FLAG_SECURE_PROTOCOL_SSL3 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1;
    WinHttpSetOption(hInternet, WINHTTP_OPTION_SECURE_PROTOCOLS, &flags, sizeof(flags)); 
	flags = WINHTTP_FLAG_SECURE_PROTOCOL_SSL3 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hInternet, WINHTTP_OPTION_SECURE_PROTOCOLS, &flags, sizeof(flags));

}
#endif
