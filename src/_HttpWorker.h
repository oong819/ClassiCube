#include "_HttpBase.h"
static void* workerWaitable;
static void* workerThread;
static void* pendingMutex;
static struct RequestList pendingReqs;

static void* curRequestMutex;
static struct HttpRequest http_curRequest;
static volatile int http_curProgress = HTTP_PROGRESS_NOT_WORKING_ON;

/* Allocates initial data buffer to store response contents */
static void Http_BufferInit(struct HttpRequest* req) {
	http_curProgress = 0;
	req->_capacity = req->contentLength ? req->contentLength : 1;
	req->data      = (cc_uint8*)Mem_Alloc(req->_capacity, 1, "http data");
	req->size      = 0;
}

/* Ensures data buffer has enough space left to append amount bytes, reallocates if not */
static void Http_BufferEnsure(struct HttpRequest* req, cc_uint32 amount) {
	cc_uint32 newSize = req->size + amount;
	if (newSize <= req->_capacity) return;

	req->_capacity = newSize;
	req->data      = (cc_uint8*)Mem_Realloc(req->data, newSize, 1, "http data+");
}

/* Increases size and updates current progress */
static void Http_BufferExpanded(struct HttpRequest* req, cc_uint32 read) {
	req->size += read;
	if (req->contentLength) http_curProgress = (int)(100.0f * req->size / req->contentLength);
}


/*########################################################################################################################*
*--------------------------------------------------Common downloader code-------------------------------------------------*
*#########################################################################################################################*/
/* Sets up state to begin a http request */
static void Http_BeginRequest(struct HttpRequest* req, cc_string* url) {
	Http_GetUrl(req, url);
	Platform_Log2("Fetching %s (type %b)", url, &req->requestType);

	Mutex_Lock(curRequestMutex);
	{
		http_curRequest  = *req;
		http_curProgress = HTTP_PROGRESS_MAKING_REQUEST;
	}
	Mutex_Unlock(curRequestMutex);
}

static void Http_ParseCookie(struct HttpRequest* req, const cc_string* value) {
	cc_string name, data;
	int dataEnd;
	String_UNSAFE_Separate(value, '=', &name, &data);
	/* Cookie is: __cfduid=xyz; expires=abc; path=/; domain=.classicube.net; HttpOnly */
	/* However only the __cfduid=xyz part of the cookie should be stored */
	dataEnd = String_IndexOf(&data, ';');
	if (dataEnd >= 0) data.length = dataEnd;

	EntryList_Set(req->cookies, &name, &data, '=');
}

/* Parses a HTTP header */
static void Http_ParseHeader(struct HttpRequest* req, const cc_string* line) {
	static const cc_string httpVersion = String_FromConst("HTTP");
	cc_string name, value, parts[3];
	int numParts;

	/* HTTP[version] [status code] [status reason] */
	if (String_CaselessStarts(line, &httpVersion)) {
		numParts = String_UNSAFE_Split(line, ' ', parts, 3);
		if (numParts >= 2) Convert_ParseInt(&parts[1], &req->statusCode);
	}
	/* For all other headers:  name: value */
	if (!String_UNSAFE_Separate(line, ':', &name, &value)) return;

	if (String_CaselessEqualsConst(&name, "ETag")) {
		String_CopyToRawArray(req->etag, &value);
	} else if (String_CaselessEqualsConst(&name, "Content-Length")) {
		Convert_ParseInt(&value, &req->contentLength);
	} else if (String_CaselessEqualsConst(&name, "X-Dropbox-Content-Length")) {
		/* dropbox stopped returning Content-Length header since switching to chunked transfer */
		/*  https://www.dropboxforum.com/t5/Discuss-Dropbox-Developer-API/Dropbox-media-can-t-be-access-by-azure-blob/td-p/575458 */
		Convert_ParseInt(&value, &req->contentLength);
	} else if (String_CaselessEqualsConst(&name, "Last-Modified")) {
		String_CopyToRawArray(req->lastModified, &value);
	} else if (req->cookies && String_CaselessEqualsConst(&name, "Set-Cookie")) {
		Http_ParseCookie(req, &value);
	}
}

/* Adds a http header to the request headers. */
static void Http_AddHeader(struct HttpRequest* req, const char* key, const cc_string* value);

/* Adds all the appropriate headers for a request. */
static void Http_SetRequestHeaders(struct HttpRequest* req) {
	static const cc_string contentType = String_FromConst("application/x-www-form-urlencoded");
	cc_string str, cookies; char cookiesBuffer[1024];
	int i;

	if (req->lastModified[0]) {
		str = String_FromRawArray(req->lastModified);
		Http_AddHeader(req, "If-Modified-Since", &str);
	}
	if (req->etag[0]) {
		str = String_FromRawArray(req->etag);
		Http_AddHeader(req, "If-None-Match", &str);
	}

	if (req->data) Http_AddHeader(req, "Content-Type", &contentType);
	if (!req->cookies || !req->cookies->count) return;

	String_InitArray(cookies, cookiesBuffer);
	for (i = 0; i < req->cookies->count; i++) {
		if (i) String_AppendConst(&cookies, "; ");
		str = StringsBuffer_UNSAFE_Get(req->cookies, i);
		String_AppendString(&cookies, &str);
	}
	Http_AddHeader(req, "Cookie", &cookies);
}

static void Http_SignalWorker(void) { Waitable_Signal(workerWaitable); }

/* Adds a req to the list of pending requests, waking up worker thread if needed */
static void HttpBackend_Add(struct HttpRequest* req, cc_uint8 flags) {
	Mutex_Lock(pendingMutex);
	{	
		RequestList_Append(&pendingReqs, req, flags);
	}
	Mutex_Unlock(pendingMutex);
	Http_SignalWorker();
}

static void HttpBackend_Init(void);
static cc_result HttpBackend_Do(struct HttpRequest* req, cc_string* url);


/*########################################################################################################################*
*----------------------------------------------------Http public api------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Http_GetResult(int reqID, struct HttpRequest* item) {
	int i;
	Mutex_Lock(processedMutex);
	{
		i = RequestList_Find(&processedReqs, reqID);
		if (i >= 0) *item = processedReqs.entries[i];
		if (i >= 0) RequestList_RemoveAt(&processedReqs, i);
	}
	Mutex_Unlock(processedMutex);
	return i >= 0;
}

cc_bool Http_GetCurrent(int* reqID, int* progress) {
	Mutex_Lock(curRequestMutex);
	{
		*reqID    = http_curRequest.id;
		*progress = http_curProgress;
	}
	Mutex_Unlock(curRequestMutex);
	return *reqID != 0;
}

int Http_CheckProgress(int reqID) {
	int curReqID, progress;
	Http_GetCurrent(&curReqID, &progress);

	if (reqID != curReqID) progress = HTTP_PROGRESS_NOT_WORKING_ON;
	return progress;
}

void Http_ClearPending(void) {
	Mutex_Lock(pendingMutex);
	{
		RequestList_Free(&pendingReqs);
	}
	Mutex_Unlock(pendingMutex);
}

void Http_TryCancel(int reqID) {
	Mutex_Lock(pendingMutex);
	{
		RequestList_TryFree(&pendingReqs, reqID);
	}
	Mutex_Unlock(pendingMutex);

	Mutex_Lock(processedMutex);
	{
		RequestList_TryFree(&processedReqs, reqID);
	}
	Mutex_Unlock(processedMutex);
}


/*########################################################################################################################*
*-----------------------------------------------------Worker frontend-----------------------------------------------------*
*#########################################################################################################################*/
static void ClearCurrentRequest(void) {
	Mutex_Lock(curRequestMutex);
	{
		http_curRequest.id = 0;
		http_curProgress   = HTTP_PROGRESS_NOT_WORKING_ON;
	}
	Mutex_Unlock(curRequestMutex);
}

static void WorkerLoop(void) {
	char urlBuffer[URL_MAX_SIZE]; cc_string url;
	struct HttpRequest request;
	cc_bool hasRequest;
	cc_uint64 beg, end;
	int elapsed;

	for (;;) {
		hasRequest = false;

		Mutex_Lock(pendingMutex);
		{
			if (pendingReqs.count) {
				request    = pendingReqs.entries[0];
				hasRequest = true;
				RequestList_RemoveAt(&pendingReqs, 0);
			}
		}
		Mutex_Unlock(pendingMutex);

		/* Block until another thread submits a request to do */
		if (!hasRequest) {
			Platform_LogConst("Going back to sleep...");
			Waitable_Wait(workerWaitable);
			continue;
		}

		String_InitArray(url, urlBuffer);
		Http_BeginRequest(&request, &url);

		beg = Stopwatch_Measure();
		request.result = HttpBackend_Do(&request, &url);
		end = Stopwatch_Measure();

		elapsed = Stopwatch_ElapsedMS(beg, end);
		Platform_Log4("HTTP: result %i (http %i) in %i ms (%i bytes)",
					&request.result, &request.statusCode, &elapsed, &request.size);

		Http_FinishRequest(&request);
		ClearCurrentRequest();
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Http component------------------------------------------------------*
*#########################################################################################################################*/
static void Http_Init(void) {
	Http_InitCommon();
	/* Http component gets initialised multiple times on Android */
	if (workerThread) return;

	HttpBackend_Init();
	workerWaitable = Waitable_Create();
	RequestList_Init(&pendingReqs);
	RequestList_Init(&processedReqs);

	pendingMutex    = Mutex_Create();
	processedMutex  = Mutex_Create();
	curRequestMutex = Mutex_Create();
	workerThread    = Thread_Create(WorkerLoop);

	Thread_Start2(workerThread, WorkerLoop);
}
