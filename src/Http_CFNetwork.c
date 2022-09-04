#include "Core.h"
#if defined CC_BUILD_CFNETWORK && !defined CC_BUILD_CURL
#include "_HttpWorker.h"
#include "Errors.h"
#include <stddef.h>
#include <CFNetwork/CFNetwork.h>

cc_bool Http_DescribeError(cc_result res, cc_string* dst) {
    return false;
}

static void HttpBackend_Init(void) {
    
}

static void Http_AddHeader(struct HttpRequest* req, const char* key, const cc_string* value) {
    char tmp[NATIVE_STR_LEN];
    CFStringRef keyCF, valCF;
    CFHTTPMessageRef msg = (CFHTTPMessageRef)req->meta;
    Platform_EncodeUtf8(tmp, value);
    
    keyCF = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    valCF = CFStringCreateWithCString(NULL, tmp, kCFStringEncodingUTF8);
    CFHTTPMessageSetHeaderFieldValue(msg, keyCF, valCF);
    CFRelease(keyCF);
    CFRelease(valCF);
}

static void Http_CheckHeader(const void* k, const void* v, void* ctx) {
    cc_string line; char lineBuffer[2048];
    char keyBuf[128]  = { 0 };
    char valBuf[1024] = { 0 };
    String_InitArray(line, lineBuffer);
    
    CFStringGetCString((CFStringRef)k, keyBuf, sizeof(keyBuf), kCFStringEncodingUTF8);
    CFStringGetCString((CFStringRef)v, valBuf, sizeof(valBuf), kCFStringEncodingUTF8);
    
    String_Format2(&line, "%c:%c", keyBuf, valBuf);
    Http_ParseHeader((struct HttpRequest*)ctx, &line);
    ctx = NULL;
}

static cc_result ParseResponseHeaders(struct HttpRequest* req, CFReadStreamRef stream) {
    CFHTTPMessageRef response = CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPResponseHeader);
    if (!response) return ERR_INVALID_ARGUMENT;
    
    CFDictionaryRef headers = CFHTTPMessageCopyAllHeaderFields(response);
    CFDictionaryApplyFunction(headers, Http_CheckHeader, req);
    req->statusCode = CFHTTPMessageGetResponseStatusCode(response);
    
    CFRelease(headers);
    CFRelease(response);
    return 0;
}

static cc_result HttpBackend_Do(struct HttpRequest* req, cc_string* url) {
    static const cc_string userAgent = String_FromConst(GAME_APP_NAME);
    static CFStringRef verbs[] = { CFSTR("GET"), CFSTR("HEAD"), CFSTR("POST") };
    cc_bool gotHeaders = false;
    char tmp[NATIVE_STR_LEN];
    CFHTTPMessageRef request;
    CFStringRef urlCF;
    CFURLRef urlRef;
    cc_result result = 0;
    
    Platform_EncodeUtf8(tmp, url);
    urlCF  = CFStringCreateWithCString(NULL, tmp, kCFStringEncodingUTF8);
    urlRef = CFURLCreateWithString(NULL, urlCF, NULL);
    // TODO e.g. "http://www.example.com/skin/1 2.png" causes this to return null
    // TODO release urlCF
    if (!urlRef) return ERR_INVALID_DATA_URL;
    
    request = CFHTTPMessageCreateRequest(NULL, verbs[req->requestType], urlRef, kCFHTTPVersion1_1);
    req->meta = request;
    Http_SetRequestHeaders(req);
    Http_AddHeader(req, "User-Agent", &userAgent);
    CFRelease(urlRef);
    
    if (req->data && req->size) {
        CFDataRef body = CFDataCreate(NULL, req->data, req->size);
        CFHTTPMessageSetBody(request, body);
        CFRelease(body); /* TODO: ???? */
        
        req->data = NULL;
        req->size = 0;
        Mem_Free(req->data);
    }
    
    CFReadStreamRef stream = CFReadStreamCreateForHTTPRequest(NULL, request);
    CFReadStreamSetProperty(stream, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue);
    //CFHTTPReadStreamSetRedirectsAutomatically(stream, TRUE);
    CFReadStreamOpen(stream);
    UInt8 buf[1024];
    
    for (;;) {
        CFIndex read = CFReadStreamRead(stream, buf, sizeof(buf));
        if (read <= 0) break;
        
        // reading headers before for loop doesn't work
        if (!gotHeaders) {
            gotHeaders = true;
            if ((result = ParseResponseHeaders(req, stream))) break;
        }
        
        if (!req->_capacity) Http_BufferInit(req);
        Http_BufferEnsure(req, read);
        
        Mem_Copy(&req->data[req->size], buf, read);
        Http_BufferExpanded(req, read);
    }
    
    if (!gotHeaders)
        result = ParseResponseHeaders(req, stream);
    
    //Thread_Sleep(1000);
    CFRelease(request);
    return result;
}
#endif
