#include "Core.h"
#if defined CC_BUILD_CURL
#include "_HttpWorker.h"
#include "Errors.h"
#include <stddef.h>

/* === BEGIN CURL HEADERS === */
typedef void CURL;
struct curl_slist;
typedef int CURLcode;

#define CURL_GLOBAL_DEFAULT ((1<<0) | (1<<1)) /* SSL and Win32 options */
#define CURLOPT_WRITEDATA      (10000 + 1)
#define CURLOPT_URL            (10000 + 2)
#define CURLOPT_ERRORBUFFER    (10000 + 10)
#define CURLOPT_WRITEFUNCTION  (20000 + 11)
#define CURLOPT_POSTFIELDS     (10000 + 15)
#define CURLOPT_USERAGENT      (10000 + 18)
#define CURLOPT_HTTPHEADER     (10000 + 23)
#define CURLOPT_HEADERDATA     (10000 + 29)
#define CURLOPT_VERBOSE        (0     + 41)
#define CURLOPT_HEADER         (0     + 42)
#define CURLOPT_NOBODY         (0     + 44)
#define CURLOPT_POST           (0     + 47)
#define CURLOPT_FOLLOWLOCATION (0     + 52)
#define CURLOPT_POSTFIELDSIZE  (0     + 60)
#define CURLOPT_SSL_VERIFYPEER (0     + 64)
#define CURLOPT_MAXREDIRS      (0     + 68)
#define CURLOPT_HEADERFUNCTION (20000 + 79)
#define CURLOPT_HTTPGET        (0     + 80)
#define CURLOPT_SSL_VERIFYHOST (0     + 81)

#if defined _WIN32
#define APIENTRY __cdecl
#else
#define APIENTRY
#endif

static CURLcode (APIENTRY *_curl_global_init)(long flags);
static void     (APIENTRY *_curl_global_cleanup)(void);
static CURL*    (APIENTRY *_curl_easy_init)(void);
static CURLcode (APIENTRY *_curl_easy_perform)(CURL *c);
static CURLcode (APIENTRY *_curl_easy_setopt)(CURL *c, int opt, ...);
static void     (APIENTRY *_curl_easy_cleanup)(CURL* c);
static void     (APIENTRY *_curl_slist_free_all)(struct curl_slist* l);
static struct curl_slist* (APIENTRY *_curl_slist_append)(struct curl_slist* l, const char* v);
static const char* (APIENTRY *_curl_easy_strerror)(CURLcode res);
/* === END CURL HEADERS === */

#if defined CC_BUILD_WIN
static const cc_string curlLib = String_FromConst("libcurl.dll");
static const cc_string curlAlt = String_FromConst("curl.dll");
#elif defined CC_BUILD_DARWIN
static const cc_string curlLib = String_FromConst("/usr/lib/libcurl.4.dylib");
static const cc_string curlAlt = String_FromConst("/usr/lib/libcurl.dylib");
#elif defined CC_BUILD_NETBSD
static const cc_string curlLib = String_FromConst("libcurl.so");
static const cc_string curlAlt = String_FromConst("/usr/pkg/lib/libcurl.so");
#elif defined CC_BUILD_BSD
static const cc_string curlLib = String_FromConst("libcurl.so");
static const cc_string curlAlt = String_FromConst("libcurl.so");
#else
static const cc_string curlLib = String_FromConst("libcurl.so.4");
static const cc_string curlAlt = String_FromConst("libcurl.so.3");
#endif

static cc_bool LoadCurlFuncs(void) {
	static const struct DynamicLibSym funcs[] = {
		DynamicLib_Sym(curl_global_init),    DynamicLib_Sym(curl_global_cleanup),
		DynamicLib_Sym(curl_easy_init),      DynamicLib_Sym(curl_easy_perform),
		DynamicLib_Sym(curl_easy_setopt),    DynamicLib_Sym(curl_easy_cleanup),
		DynamicLib_Sym(curl_slist_free_all), DynamicLib_Sym(curl_slist_append)
	};
	cc_bool success;
	void* lib;

	success = DynamicLib_LoadAll(&curlLib,     funcs, Array_Elems(funcs), &lib);
	if (!lib) { 
		success = DynamicLib_LoadAll(&curlAlt, funcs, Array_Elems(funcs), &lib);
	}

	/* Non-essential function missing in older curl versions */
	_curl_easy_strerror = DynamicLib_Get2(lib, "curl_easy_strerror");
	return success;
}

static CURL* curl;
static cc_bool curlSupported;

cc_bool Http_DescribeError(cc_result res, cc_string* dst) {
	const char* err;
	
	if (!_curl_easy_strerror) return false;
	err = _curl_easy_strerror((CURLcode)res);
	if (!err) return false;

	String_AppendConst(dst, err);
	return true;
}

static void HttpBackend_Init(void) {
	static const cc_string msg = String_FromConst("Failed to init libcurl. All HTTP requests will therefore fail.");
	CURLcode res;

	if (!LoadCurlFuncs()) { Logger_WarnFunc(&msg); return; }
	res = _curl_global_init(CURL_GLOBAL_DEFAULT);
	if (res) { Logger_SimpleWarn(res, "initing curl"); return; }

	curl = _curl_easy_init();
	if (!curl) { Logger_SimpleWarn(res, "initing curl_easy"); return; }
	curlSupported = true;
}

static void Http_AddHeader(struct HttpRequest* req, const char* key, const cc_string* value) {
	cc_string tmp; char tmpBuffer[1024];
	String_InitArray_NT(tmp, tmpBuffer);
	String_Format2(&tmp, "%c: %s", key, value);

	tmp.buffer[tmp.length] = '\0';
	req->meta = _curl_slist_append((struct curl_slist*)req->meta, tmp.buffer);
}

/* Processes a HTTP header downloaded from the server */
static size_t Http_ProcessHeader(char* buffer, size_t size, size_t nitems, void* userdata) {
	struct HttpRequest* req = (struct HttpRequest*)userdata;
	size_t len = nitems;
	cc_string line;
	/* line usually has \r\n at end */
	if (len && buffer[len - 1] == '\n') len--;
	if (len && buffer[len - 1] == '\r') len--;

	line = String_Init(buffer, len, len);
	Http_ParseHeader(req, &line);
	return nitems;
}

/* Processes a chunk of data downloaded from the web server */
static size_t Http_ProcessData(char *buffer, size_t size, size_t nitems, void* userdata) {
	struct HttpRequest* req = (struct HttpRequest*)userdata;

	if (!req->_capacity) Http_BufferInit(req);
	Http_BufferEnsure(req, nitems);

	Mem_Copy(&req->data[req->size], buffer, nitems);
	Http_BufferExpanded(req, nitems);
	return nitems;
}

/* Sets general curl options for a request */
static void Http_SetCurlOpts(struct HttpRequest* req) {
	_curl_easy_setopt(curl, CURLOPT_USERAGENT,      GAME_APP_NAME);
	_curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	_curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      20L);

	_curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, Http_ProcessHeader);
	_curl_easy_setopt(curl, CURLOPT_HEADERDATA,     req);
	_curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  Http_ProcessData);
	_curl_easy_setopt(curl, CURLOPT_WRITEDATA,      req);

	if (httpsVerify) return;
	_curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
}

static cc_result HttpBackend_Do(struct HttpRequest* req, cc_string* url) {
	char urlStr[NATIVE_STR_LEN];
	void* post_data = req->data;
	CURLcode res;
	if (!curlSupported) return ERR_NOT_SUPPORTED;

	req->meta = NULL;
	Http_SetRequestHeaders(req);
	_curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->meta);

	Http_SetCurlOpts(req);
	Platform_EncodeUtf8(urlStr, url);
	_curl_easy_setopt(curl, CURLOPT_URL, urlStr);

	if (req->requestType == REQUEST_TYPE_HEAD) {
		_curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	} else if (req->requestType == REQUEST_TYPE_POST) {
		_curl_easy_setopt(curl, CURLOPT_POST,   1L);
		_curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, req->size);
		_curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    req->data);

		/* per curl docs, we must persist POST data until request finishes */
		req->data = NULL;
		req->size = 0;
	} else {
		/* Undo POST/HEAD state */
		_curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	}

	req->_capacity   = 0;
	http_curProgress = HTTP_PROGRESS_FETCHING_DATA;
	res = _curl_easy_perform(curl);
	http_curProgress = 100;

	_curl_slist_free_all((struct curl_slist*)req->meta);
	/* can free now that request has finished */
	Mem_Free(post_data);
	return res;
}
#endif
