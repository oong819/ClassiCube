#include "Core.h"
#if defined CC_BUILD_ANDROID && !defined CC_BUILD_CURL
#include "_HttpWorker.h"
struct HttpRequest* java_req;
static jmethodID JAVA_httpInit, JAVA_httpSetHeader, JAVA_httpPerform, JAVA_httpSetData;
static jmethodID JAVA_httpDescribeError;

cc_bool Http_DescribeError(cc_result res, cc_string* dst) {
	char buffer[NATIVE_STR_LEN];
	cc_string err;
	JNIEnv* env;
	jvalue args[1];
	jobject obj;
	
	JavaGetCurrentEnv(env);
	args[0].i = res;
	obj       = JavaSCall_Obj(env, JAVA_httpDescribeError, args);
	if (!obj) return false;

	err = JavaGetString(env, obj, buffer);
	String_AppendString(dst, &err);
	(*env)->DeleteLocalRef(env, obj);
	return true;
}

static void Http_AddHeader(struct HttpRequest* req, const char* key, const cc_string* value) {
	JNIEnv* env;
	jvalue args[2];

	JavaGetCurrentEnv(env);
	args[0].l = JavaMakeConst(env,  key);
	args[1].l = JavaMakeString(env, value);

	JavaSCall_Void(env, JAVA_httpSetHeader, args);
	(*env)->DeleteLocalRef(env, args[0].l);
	(*env)->DeleteLocalRef(env, args[1].l);
}

/* Processes a HTTP header downloaded from the server */
static void JNICALL java_HttpParseHeader(JNIEnv* env, jobject o, jstring header) {
	char buffer[NATIVE_STR_LEN];
	cc_string line = JavaGetString(env, header, buffer);
	Http_ParseHeader(java_req, &line);
}

/* Processes a chunk of data downloaded from the web server */
static void JNICALL java_HttpAppendData(JNIEnv* env, jobject o, jbyteArray arr, jint len) {
	struct HttpRequest* req = java_req;
	if (!req->_capacity) Http_BufferInit(req);

	Http_BufferEnsure(req, len);
	(*env)->GetByteArrayRegion(env, arr, 0, len, (jbyte*)(&req->data[req->size]));
	Http_BufferExpanded(req, len);
}

static const JNINativeMethod methods[] = {
	{ "httpParseHeader", "(Ljava/lang/String;)V", java_HttpParseHeader },
	{ "httpAppendData",  "([BI)V",                java_HttpAppendData }
};
static void CacheMethodRefs(JNIEnv* env) {
	JAVA_httpInit      = JavaGetSMethod(env, "httpInit",      "(Ljava/lang/String;Ljava/lang/String;)I");
	JAVA_httpSetHeader = JavaGetSMethod(env, "httpSetHeader", "(Ljava/lang/String;Ljava/lang/String;)V");
	JAVA_httpPerform   = JavaGetSMethod(env, "httpPerform",   "()I");
	JAVA_httpSetData   = JavaGetSMethod(env, "httpSetData",   "([B)I");

	JAVA_httpDescribeError = JavaGetSMethod(env, "httpDescribeError", "(I)Ljava/lang/String;");
}

static void HttpBackend_Init(void) {
	JNIEnv* env;
	JavaGetCurrentEnv(env);
	JavaRegisterNatives(env, methods);
	CacheMethodRefs(env);
}

static cc_result Http_InitReq(JNIEnv* env, struct HttpRequest* req, cc_string* url) {
	static const char* verbs[3] = { "GET", "HEAD", "POST" };
	jvalue args[2];
	jint res;

	args[0].l = JavaMakeString(env, url);
	args[1].l = JavaMakeConst(env,  verbs[req->requestType]);

	res = JavaSCall_Int(env, JAVA_httpInit, args);
	(*env)->DeleteLocalRef(env, args[0].l);
	(*env)->DeleteLocalRef(env, args[1].l);
	return res;
}

static cc_result Http_SetData(JNIEnv* env, struct HttpRequest* req) {
	jvalue args[1];
	jint res;

	args[0].l = JavaMakeBytes(env, req->data, req->size);
	res = JavaSCall_Int(env, JAVA_httpSetData, args);
	(*env)->DeleteLocalRef(env, args[0].l);
	return res;
}

static cc_result HttpBackend_Do(struct HttpRequest* req, cc_string* url) {
	static const cc_string userAgent = String_FromConst(GAME_APP_NAME);
	JNIEnv* env;
	jint res;

	JavaGetCurrentEnv(env);
	if ((res = Http_InitReq(env, req, url))) return res;
	java_req = req;

	Http_SetRequestHeaders(req);
	Http_AddHeader(req, "User-Agent", &userAgent);
	if (req->data && (res = Http_SetData(env, req))) return res;

	req->_capacity   = 0;
	http_curProgress = HTTP_PROGRESS_FETCHING_DATA;
	res = JavaSCall_Int(env, JAVA_httpPerform, NULL);
	http_curProgress = 100;
	return res;
}
#endif
