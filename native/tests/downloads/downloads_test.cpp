static const char* testDirectory();
#define PEPPY_DOWNLOAD_DIRECTORY testDirectory()
#include "../../downloads.cpp"
#include <assert.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <vector>
#include <stdlib.h>

static std::vector<uint8_t> payload;
static size_t cursor;
static int mode, requests, closedRequests, closedConnections, closedTemplates, closedHttp, closedSsl, closedPools;
static std::atomic<bool> aborted(false), blocked(false), releaseRead(false);
static std::string requestUrl;
static bool secure=false, manual=false;
static int moduleLoads=0, moduleProbes=0, stateQueries=0, netError=0;
static size_t nativeHeaderCap=5000, connectionHeaderCap=5000;
static bool headerConfigured=false;
static uint64_t advertisedLength=0;
static int contentLengthCalls=0;
static const int32_t LARGE_READ_ERROR=(int32_t)0x80431ff0U;
static const int MODE_MEDIAFIRE = 32;
static const int32_t SOURCE_NATIVE_ERROR = (int32_t)0x80431fe0U;
static const size_t SOURCE_BODY_LIMIT = 1024 * 1024;
static const char* SOURCE_PAGE_URL = "https://www.mediafire.com/file/ABC123/sample.pkg/file";
static const char* SOURCE_CDN_URL = "https://download2392.mediafire.com/token/sample.pkg?key=a&part=b";

struct SourceResponse {
    std::string url;
    std::vector<uint8_t> body;
    std::string headers;
    int status, statusRc, sendRc, lengthRc, lengthType, readRc, sslCode;
    uint32_t sslDetail;
    uint64_t length;
    size_t readFailureAfter, blockAfter, fragment;
    bool page, overrideLength;
    SourceResponse(const char* expectedUrl, bool sourcePage = false)
        : url(expectedUrl), status(200), statusRc(0), sendRc(0), lengthRc(0),
          lengthType(ORBIS_HTTP_CONTENTLEN_EXIST), readRc(0), sslCode(0),
          sslDetail(0), length(0), readFailureAfter(SIZE_MAX), blockAfter(SIZE_MAX),
          fragment(3), page(sourcePage), overrideLength(false) {}
};
static std::vector<SourceResponse> sourceResponses;
static std::vector<std::string> sourceRequestUrls;
static size_t sourceCursor, sourceBytes, sourceReadCalls, packageReadCalls;
static int templateCreates, tlsEnables;
static SourceResponse& sourceResponse() {
    assert(requests > 0 && (size_t)requests <= sourceResponses.size());
    return sourceResponses[(size_t)requests - 1];
}
static const char* testDirectory(){static char path[]="/tmp/peppy-download-test-XXXXXX";static const char* created=mkdtemp(path);assert(created);return created;}
static std::string outputPath(bool partial=false){return std::string(testDirectory())+"/sample.pkg"+(partial?".part":"");}
extern "C" size_t __real_fwrite(const void*,size_t,size_t,FILE*);
extern "C" int __real_fflush(FILE*);
extern "C" int __real_fclose(FILE*);
extern "C" size_t __wrap_fwrite(const void* p,size_t s,size_t n,FILE* f){if(mode==11)return n? n-1:0;return __real_fwrite(p,s,n,f);}
extern "C" int __wrap_fflush(FILE* f){if(mode==12){errno=ENOSPC;return EOF;}return __real_fflush(f);}
extern "C" int __wrap_fclose(FILE* f){int rc=__real_fclose(f);if(mode==13){errno=ENOSPC;return EOF;}return rc;}
extern "C" int32_t sceKernelUsleep(uint32_t n){std::this_thread::sleep_for(std::chrono::microseconds(n==100000?100:n));return 0;}
extern "C" int32_t scePthreadAttrInit(OrbisPthreadAttr* a){*a=0;return mode==25?(int32_t)0x80020022:0;}
extern "C" int32_t scePthreadAttrDestroy(OrbisPthreadAttr*){return 0;}
extern "C" int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr* a,int n){assert(n==1);*a=1;return mode==26?(int32_t)0x80020016:0;}
extern "C" int32_t scePthreadCreate(OrbisPthread* t,const OrbisPthreadAttr* a,void*(*f)(void*),void* p,const char*){assert(*a==1);if(mode==27)return (int32_t)0x8002000c;*t=1;std::thread(f,p).detach();return 0;}
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal){++moduleLoads;return mode==16||mode==17?0x805a1001:0;}
extern "C" int32_t mockIsLoaded(OrbisSysModuleInternal) __asm__("sceSysmoduleIsLoadedInternal");
extern "C" int32_t mockIsLoaded(OrbisSysModuleInternal){++moduleProbes;return mode==15||(mode==16&&moduleProbes%2==0)?0:-1;}
extern "C" int32_t* mockNetErrno() __asm__("sceNetErrnoLoc");
extern "C" int32_t* mockNetErrno(){return &netError;}
extern "C" int32_t sceNetInit(){return mode==23?-1:0;}
extern "C" int32_t sceNetCtlInit(){return mode==18||mode==19?(int32_t)0x80412101:0;}
extern "C" int32_t mockNetState(int32_t*) __asm__("sceNetCtlGetState");
extern "C" int32_t mockNetState(int32_t* state){++stateQueries;*state=mode==21||mode==22?0:3;return mode==19||mode==20?(int32_t)0x80412103:0;}
extern "C" int32_t sceNetPoolCreate(const char*,int32_t bytes,int32_t){assert(bytes==1024*1024);return mode==23?-1:1;}
extern "C" void sceNetPoolDestroy(int32_t){++closedPools;}
extern "C" int32_t sceSslInit(size_t){return 2;}
extern "C" int32_t mockSslTerm(int32_t) __asm__("sceSslTerm");
extern "C" int32_t mockSslTerm(int32_t){++closedSsl;return 0;}
extern "C" int32_t sceHttpInit(int32_t,int32_t,size_t){return 3;}
extern "C" int32_t sceHttpCreateTemplate(int32_t,const char*,int32_t,int32_t proxy){assert(proxy==0);++templateCreates;nativeHeaderCap=5000;headerConfigured=false;return 4;}
extern "C" int32_t mockHeaderCap(int32_t,size_t) __asm__("sceHttpSetResponseHeaderMaxSize");
extern "C" int32_t mockHeaderCap(int32_t templateId,size_t size){assert(templateId==4&&size==65536);if(mode==30)return (int32_t)0x804311fe;nativeHeaderCap=size;headerConfigured=true;return 0;}
extern "C" int32_t sceHttpsEnableOption(int32_t,uint32_t flags){assert(flags==0xbd);++tlsEnables;secure=true;return 0;}
extern "C" int32_t mockAuto(int32_t,int32_t) __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t mockAuto(int32_t,int32_t e){assert(e==0);manual=true;return 0;}
extern "C" int32_t mockRecv(int32_t,uint32_t) __asm__("sceHttpSetRecvTimeOut");
extern "C" int32_t mockRecv(int32_t,uint32_t n){assert(n==15000000);return 0;}
extern "C" int32_t mockSslError(int32_t,int32_t*,uint32_t*) __asm__("sceHttpsGetSslError");
extern "C" int32_t mockSslError(int32_t,int32_t* e,uint32_t* d){if(mode==MODE_MEDIAFIRE){*e=sourceResponse().sslCode;*d=sourceResponse().sslDetail;}else{*e=mode==7?1:0;*d=0;}return 0;}
extern "C" int32_t sceHttpSetResolveTimeOut(int32_t,uint32_t){return mode==24?(int32_t)0x804310fe:0;}
extern "C" int32_t sceHttpSetConnectTimeOut(int32_t,uint32_t){return 0;}
extern "C" int32_t sceHttpSetSendTimeOut(int32_t,uint32_t){return 0;}
extern "C" int32_t sceHttpCreateConnectionWithURL(int32_t templateId,const char* url,bool){connectionHeaderCap=nativeHeaderCap;requestUrl=url;if(mode==MODE_MEDIAFIRE){assert(templateId==4&&secure&&manual&&headerConfigured);assert((size_t)requests<sourceResponses.size());assert(sourceResponses[(size_t)requests].url==url);assert(closedRequests==requests&&closedConnections==requests);sourceRequestUrls.push_back(url);}return 5;}
extern "C" int32_t sceHttpCreateRequestWithURL(int32_t,int32_t,const char* url,uint64_t){if(mode==MODE_MEDIAFIRE){assert(requestUrl==url);sourceCursor=0;}return 10+(++requests);}
extern "C" int32_t sceHttpAddRequestHeader(int32_t,const char*,const char*,int32_t){return 0;}
extern "C" int32_t sceHttpSendRequest(int32_t,const void*,size_t){assert(secure&&manual);if(mode==MODE_MEDIAFIRE)return sourceResponse().sendRc;if(mode==6){blocked=true;while(!aborted.load())sceKernelUsleep(1000);return -1;}if(mode==7)return -1;size_t headers=mode==29?65537:mode==28?8000:500;if(headers>connectionHeaderCap)return (int32_t)0x80431073;return 0;}
extern "C" int32_t sceHttpGetStatusCode(int32_t,int32_t* out){if(mode==MODE_MEDIAFIRE){*out=sourceResponse().status;return sourceResponse().statusRc;}*out=((mode==1||mode==28)&&requests==1)||mode==2||mode==8?302:mode==3?404:200;return 0;}
extern "C" int32_t sceHttpGetLastErrno(int32_t,int32_t* out){*out=mode==29?(int32_t)0x80431073:netError;return 0;}
extern "C" int32_t sceHttpGetAllResponseHeaders(int32_t,char** out,size_t* n){static char good[]="HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/file.pkg?token=abc\r\n\r\n";static char evil[]="Location: https://github.com.evil.example/file.pkg\r\n";static char empty[]="";static std::string large;if(mode==MODE_MEDIAFIRE){std::string& headers=sourceResponse().headers;*out=headers.empty()?empty:&headers[0];*n=headers.size();return 0;}if(mode==28){large=std::string(good)+"X-Security: "+std::string(8000,'a')+"\r\n";*out=&large[0];*n=large.size();}else{*out=mode==2?evil:good;*n=strlen(*out);}return 0;}
extern "C" int32_t sceHttpGetResponseContentLength(int32_t,int32_t* type,size_t* n){++contentLengthCalls;if(mode==MODE_MEDIAFIRE){SourceResponse& value=sourceResponse();*type=value.lengthType;*n=(size_t)(value.overrideLength?value.length:value.body.size());return value.lengthRc;}*type=mode==14?1:ORBIS_HTTP_CONTENTLEN_EXIST;*n=advertisedLength?(size_t)advertisedLength:mode==9||mode==10?8:payload.size()+(mode==4?1:0);return 0;}
extern "C" int32_t sceHttpReadData(int32_t,void* out,uint32_t max){
    if(mode==MODE_MEDIAFIRE){
        SourceResponse& value=sourceResponse();
        if(value.page){
            ++sourceReadCalls;
            assert(downloadSnapshot().received==0);
            assert(access(outputPath(true).c_str(),F_OK)!=0);
        }else ++packageReadCalls;
        if(sourceCursor>=value.blockAfter){blocked=true;while(!aborted.load()&&!releaseRead.load())sceKernelUsleep(1000);if(aborted.load())return -1;value.blockAfter=SIZE_MAX;}
        if(sourceCursor>=value.readFailureAfter)return value.readRc;
        if(sourceCursor==value.body.size())return 0;
        size_t n=value.body.size()-sourceCursor;
        if(n>max)n=max;
        if(n>value.fragment)n=value.fragment;
        memcpy(out,value.body.data()+sourceCursor,n);sourceCursor+=n;
        if(value.page)sourceBytes+=n;
        return (int32_t)n;
    }
    if(cursor==payload.size())return mode==31?LARGE_READ_ERROR:0;
    size_t n=payload.size()-cursor;if(n>max)n=max;if(n>3)n=3;
    memcpy(out,payload.data()+cursor,n);cursor+=n;return (int32_t)n;
}
extern "C" int32_t sceHttpAbortRequest(int32_t){aborted=true;return 0;}
extern "C" int32_t sceHttpDeleteRequest(int32_t){++closedRequests;return 0;}
extern "C" int32_t sceHttpDeleteConnection(int32_t){++closedConnections;return 0;}
extern "C" int32_t sceHttpDeleteTemplate(int32_t){++closedTemplates;return 0;}
extern "C" int32_t sceHttpTerm(int32_t){++closedHttp;return 0;}

std::string hashText(const std::string& input){Sha256 s;for(size_t i=0;i<input.size();i+=7)s.update((const uint8_t*)input.data()+i,input.size()-i>7?7:input.size()-i);uint8_t digest[32];s.finish(digest);char buf[65];for(int i=0;i<32;++i)sprintf(buf+2*i,"%02x",digest[i]);return buf;}
DownloadSnapshot finished(){for(int i=0;i<5000;++i){if(!__atomic_load_n(&g_busy,__ATOMIC_ACQUIRE))return downloadSnapshot();sceKernelUsleep(1000);}assert(false);return downloadSnapshot();}
void reset(int nextMode){assert(!__atomic_load_n(&g_busy,__ATOMIC_ACQUIRE));mode=nextMode;cursor=0;requests=0;moduleLoads=moduleProbes=stateQueries=0;advertisedLength=0;contentLengthCalls=0;netError=0;aborted=false;blocked=false;releaseRead=false;secure=false;manual=false;closedRequests=closedConnections=closedTemplates=closedHttp=closedSsl=closedPools=0;payload={0x7f,0x43,0x4e,0x54,'a','b','c','d'};sourceResponses.clear();sourceRequestUrls.clear();sourceCursor=sourceBytes=sourceReadCalls=packageReadCalls=0;templateCreates=tlsEnables=0;unlink(outputPath().c_str());unlink(outputPath(true).c_str());}
void cleanHandles(){assert(closedRequests==requests&&closedConnections==requests);assert(closedTemplates==1&&closedHttp==1&&closedSsl==1&&closedPools==1);assert(access(outputPath(true).c_str(),F_OK)!=0);}
void seedPrevious(){FILE* f=fopen(outputPath().c_str(),"wb");assert(f);assert(__real_fwrite("previous",1,8,f)==8);assert(__real_fclose(f)==0);}
void checkPrevious(){FILE* f=fopen(outputPath().c_str(),"rb");assert(f);char data[9]={0};assert(fread(data,1,8,f)==8);assert(!strcmp(data,"previous"));assert(__real_fclose(f)==0);}

std::string sourceAnchor(const char* target = SOURCE_CDN_URL) {
    std::string url(target);
    size_t at = 0;
    while ((at = url.find('&', at)) != std::string::npos) {
        url.replace(at, 1, "&amp;"); at += 5;
    }
    return "<!doctype html><html><body><a class='download' id='downloadButton' href=\"" +
           url + "\">Download</a></body></html>";
}
SourceResponse sourcePage(const std::string& html, const char* url = SOURCE_PAGE_URL) {
    SourceResponse response(url, true);
    response.body.assign(html.begin(), html.end());
    return response;
}
SourceResponse sourcePackage(const char* url = SOURCE_CDN_URL) {
    SourceResponse response(url);
    response.body = payload;
    return response;
}
SourceResponse sourceRedirect(const char* from, const char* to, bool page = true) {
    SourceResponse response(from, page);
    response.status = 302;
    response.headers = std::string("HTTP/1.1 302 Found\r\nLocation: ") + to + "\r\n\r\n";
    return response;
}
void resetSource(const std::string& html = sourceAnchor()) {
    reset(MODE_MEDIAFIRE);
    sourceResponses.push_back(sourcePage(html));
    sourceResponses.push_back(sourcePackage());
}
void checkSourceFailure(const DownloadSpec& spec, int category, int where,
                        int32_t native = 0, int expectedRequests = 1) {
    seedPrevious();
    assert(startDownload(spec));
    DownloadSnapshot value = finished();
    assert(value.state == FAILED && value.errorCode == category);
    assert(value.stage == where && value.nativeCode == native);
    assert(value.received == 0 && !packageReadCalls && requests == expectedRequests);
    cleanHandles(); checkPrevious();
}
void sourceUrlTests() {
    assert(safeUrl(SOURCE_PAGE_URL));
    assert(safeUrl("https://mediafire.com/file/ABC123/sample.pkg/file"));
    assert(safeUrl(SOURCE_CDN_URL));
    const char* rejected[] = {
        "http://www.mediafire.com/file/ABC123/sample.pkg/file",
        "https://www.mediafire.com.evil.example/file/ABC123/sample.pkg/file",
        "https://www.mediafire.com@evil.example/file/ABC123/sample.pkg/file",
        "https://www.mediafire.com:443/file/ABC123/sample.pkg/file",
        "https://www.mediafire.com/download/ABC123/sample.pkg",
        "https://www.mediafire.com/file/ABC123/sample.pkg",
        "https://www.mediafire.com/file/ABC123/sample.pkg/file/extra",
        "https://www.mediafire.com/file/ABC123/sample.pkg/FILE",
        "https://www.mediafire.com/file//sample.pkg/file",
        "https://files.mediafire.com/file/ABC123/sample.pkg/file",
        "https://download2392.mediafire.com.evil.example/token/sample.pkg",
        "https://download2392.mediafire.com@evil.example/token/sample.pkg",
        "https://download2392.mediafire.com:443/token/sample.pkg",
        "https://downloadx.mediafire.com/token/sample.pkg",
        "https://download2392.mediafire.com/",
        "https://download2392.mediafire.com/token/sample.pkg#fragment"
    };
    for (const char* url : rejected) {
        assert(!safeUrl(url));
        reset(MODE_MEDIAFIRE);
        DownloadSpec bad = { url, "sample.pkg", 8, 0 };
        assert(!startDownload(bad));
        DownloadSnapshot value = downloadSnapshot();
        assert(value.state == FAILED && value.errorCode == DOWNLOAD_ERROR_SPEC);
        assert(value.stage == DOWNLOAD_STAGE_SPEC && !requests && !moduleLoads);
    }
}
void sourceBodyTests(const DownloadSpec& spec) {
    resetSource();
    // A much larger HTML length must never be compared with the eight-byte PKG.
    assert(sourceResponses[0].body.size() > spec.expectedBytes);
    assert(startDownload(spec));
    DownloadSnapshot value = finished();
    assert(value.state == DONE && value.received == 8 && value.total == 8);
    assert(requests == 2 && contentLengthCalls == 2 && sourceReadCalls > 2 && packageReadCalls == 4);
    assert(sourceRequestUrls[0] == SOURCE_PAGE_URL && sourceRequestUrls[1] == SOURCE_CDN_URL);
    assert(sourceBytes == sourceResponses[0].body.size() && templateCreates == 1 && tlsEnables == 1);
    cleanHandles();

    const std::string failedHtml[] = {
        "<html><body>Complete the CAPTCHA to continue.</body></html>",
        "<a href='https://download2392.mediafire.com/token/sample.pkg'>Download</a>",
        "<script>var fake=\"<a id='downloadButton' href='https://download2392.mediafire.com/token/sample.pkg'>Download</a>\";</script>",
        "<!-- <a id='downloadButton' href='https://download2392.mediafire.com/token/sample.pkg'>Download</a> -->",
        sourceAnchor() + sourceAnchor("https://download1.mediafire.com/another/sample.pkg"),
        sourceAnchor("https://download2392.mediafire.com.evil.example/token/sample.pkg"),
        sourceAnchor("https://github.com/official/repo/releases/download/v1/sample.pkg"),
        sourceAnchor("http://download2392.mediafire.com/token/sample.pkg"),
        sourceAnchor("https://download2392.mediafire.com:443/token/sample.pkg"),
        sourceAnchor("https://download2392.mediafire.com@evil.example/token/sample.pkg"),
        sourceAnchor("https://www.mediafire.com/file/ABC123/sample.pkg/file")
    };
    for (const std::string& html : failedHtml) {
        resetSource(html);
        checkSourceFailure(spec, DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_PARSE);
    }

    resetSource(); sourceResponses[0].lengthType = 1;
    assert(startDownload(spec)); assert(finished().state == DONE); cleanHandles();

    std::string exact = sourceAnchor();
    exact.resize(SOURCE_BODY_LIMIT, ' ');
    for (bool known : { true, false }) {
        resetSource(exact);
        sourceResponses[0].fragment = 65536;
        if (!known) sourceResponses[0].lengthType = 1;
        assert(startDownload(spec)); value = finished();
        assert(value.state == DONE && value.received == 8 && sourceBytes == SOURCE_BODY_LIMIT);
        cleanHandles();
    }
    resetSource();
    sourceResponses[0].overrideLength = true; sourceResponses[0].length = SOURCE_BODY_LIMIT + 1;
    checkSourceFailure(spec, DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ);
    assert(!sourceReadCalls && !sourceBytes);

    exact.push_back(' ');
    resetSource(exact); sourceResponses[0].lengthType = 1; sourceResponses[0].fragment = 65536;
    checkSourceFailure(spec, DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ);
    assert(sourceBytes <= SOURCE_BODY_LIMIT + 1);

    resetSource(); sourceResponses[0].overrideLength = true;
    sourceResponses[0].length = sourceResponses[0].body.size() + 1;
    checkSourceFailure(spec, DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ);
    resetSource(); sourceResponses[0].overrideLength = true;
    sourceResponses[0].length = sourceResponses[0].body.size() - 1;
    checkSourceFailure(spec, DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ);

    resetSource(); sourceResponses[0].blockAfter = 3;
    seedPrevious(); assert(startDownload(spec));
    for (int attempt = 0; !blocked.load() && attempt < 5000; ++attempt) sceKernelUsleep(1000);
    assert(blocked.load() && downloadSnapshot().stage == DOWNLOAD_STAGE_SOURCE_READ);
    assert(downloadSnapshot().received == 0 && requests == 1 && !packageReadCalls);
    assert(access(outputPath(true).c_str(), F_OK) != 0);
    cancelDownload(); value = finished();
    assert(value.state == CANCELLED && value.received == 0 && aborted.load());
    assert(sourceBytes == 3); cleanHandles(); checkPrevious();
}
void sourceNativeErrorTests(const DownloadSpec& spec) {
    for (bool tls : { false, true }) {
        resetSource(); sourceResponses[0].sendRc = SOURCE_NATIVE_ERROR;
        sourceResponses[0].sslCode = tls ? 7 : 0;
        netError = 321;
        checkSourceFailure(spec, tls ? DOWNLOAD_ERROR_TLS : DOWNLOAD_ERROR_NETWORK,
                           DOWNLOAD_STAGE_SEND, SOURCE_NATIVE_ERROR);
        assert(downloadSnapshot().networkCode == 321 && downloadSnapshot().sslCode == (tls ? 7 : 0));

        resetSource(); sourceResponses[0].readRc = SOURCE_NATIVE_ERROR;
        sourceResponses[0].readFailureAfter = 3;
        sourceResponses[0].sslDetail = tls ? 0x100 : 0;
        netError = 322;
        checkSourceFailure(spec, tls ? DOWNLOAD_ERROR_TLS : DOWNLOAD_ERROR_NETWORK,
                           DOWNLOAD_STAGE_SOURCE_READ, SOURCE_NATIVE_ERROR);
        assert(downloadSnapshot().networkCode == 322 && downloadSnapshot().sslDetails == (tls ? 0x100U : 0U));
        assert(sourceBytes == 3);

        resetSource(); sourceResponses[0].lengthRc = SOURCE_NATIVE_ERROR;
        sourceResponses[0].sslCode = tls ? 9 : 0;
        checkSourceFailure(spec, tls ? DOWNLOAD_ERROR_TLS : DOWNLOAD_ERROR_NETWORK,
                           DOWNLOAD_STAGE_SOURCE_READ, SOURCE_NATIVE_ERROR);
        assert(!sourceReadCalls && downloadSnapshot().sslCode == (tls ? 9 : 0));
    }
    resetSource(); sourceResponses[0].statusRc = SOURCE_NATIVE_ERROR;
    checkSourceFailure(spec, DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_STATUS, SOURCE_NATIVE_ERROR);
    for (int status : { 403, 404, 429, 503 }) {
        resetSource(); sourceResponses[0].status = status;
        checkSourceFailure(spec, DOWNLOAD_ERROR_HTTP, DOWNLOAD_STAGE_STATUS, status);
        assert(!sourceReadCalls && !contentLengthCalls);
    }
}
void sourceRedirectTests(const DownloadSpec& spec) {
    const char* secondPage = "https://mediafire.com/file/DEF456/sample.pkg/file";
    const char* secondCdn = "https://download5.mediafire.com/other/sample.pkg";
    reset(MODE_MEDIAFIRE);
    sourceResponses.push_back(sourceRedirect(SOURCE_PAGE_URL, secondPage));
    sourceResponses.push_back(sourcePage(sourceAnchor(), secondPage));
    sourceResponses.push_back(sourcePackage());
    assert(startDownload(spec)); assert(finished().state == DONE && requests == 3); cleanHandles();

    resetSource(); sourceResponses[1] = sourceRedirect(SOURCE_CDN_URL, secondCdn, false);
    sourceResponses.push_back(sourcePackage(secondCdn));
    assert(startDownload(spec)); assert(finished().state == DONE && requests == 3); cleanHandles();

    for (const char* forbidden : { "https://www.mediafire.com.evil.example/file/ABC123/sample.pkg/file",
                                  "https://github.com/official/repo/releases/download/v1/sample.pkg" }) {
        resetSource(); sourceResponses[0] = sourceRedirect(SOURCE_PAGE_URL, forbidden);
        checkSourceFailure(spec, DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS);
    }
    for (const char* forbidden : { SOURCE_PAGE_URL,
                                  "https://github.com/official/repo/releases/download/v1/sample.pkg",
                                  "https://download5.mediafire.com.evil.example/other/sample.pkg" }) {
        resetSource(); sourceResponses[1] = sourceRedirect(SOURCE_CDN_URL, forbidden, false);
        checkSourceFailure(spec, DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS, 0, 2);
    }
    const char* githubUrl = "https://github.com/official/repo/releases/download/v1/sample.pkg";
    for (const char* forbidden : { SOURCE_PAGE_URL, SOURCE_CDN_URL }) {
        reset(MODE_MEDIAFIRE); sourceResponses.push_back(sourceRedirect(githubUrl, forbidden, false));
        DownloadSpec github = { githubUrl, "sample.pkg", 8, 0 };
        checkSourceFailure(github, DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS);
    }

    // Five HTTP redirects alone exhaust the budget without resolving a page.
    reset(MODE_MEDIAFIRE);
    for (int i = 0; i < 6; ++i) sourceResponses.push_back(sourceRedirect(SOURCE_PAGE_URL, SOURCE_PAGE_URL));
    checkSourceFailure(spec, DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS, 0, 6);
    assert(!sourceReadCalls);

    // Page resolution consumes one of the same five transitions; no seventh request.
    resetSource(); sourceResponses.erase(sourceResponses.begin() + 1, sourceResponses.end());
    for (int i = 0; i < 5; ++i) sourceResponses.push_back(sourceRedirect(SOURCE_CDN_URL, SOURCE_CDN_URL, false));
    checkSourceFailure(spec, DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS, 0, 6);

    // Four page redirects plus one resolution fit exactly within the budget.
    reset(MODE_MEDIAFIRE);
    for (int i = 0; i < 4; ++i) sourceResponses.push_back(sourceRedirect(SOURCE_PAGE_URL, SOURCE_PAGE_URL));
    sourceResponses.push_back(sourcePage(sourceAnchor())); sourceResponses.push_back(sourcePackage());
    assert(startDownload(spec)); assert(finished().state == DONE && requests == 6); cleanHandles();
}
void sourceIntegrityTests(DownloadSpec spec) {
    resetSource(); sourceResponses[1].body[0] = 0;
    seedPrevious(); assert(startDownload(spec)); DownloadSnapshot value = finished();
    assert(value.errorCode == DOWNLOAD_ERROR_PACKAGE && value.stage == DOWNLOAD_STAGE_PACKAGE);
    assert(value.received == 3 && requests == 2 && packageReadCalls == 2);
    cleanHandles(); checkPrevious();

    resetSource(); sourceResponses[1].overrideLength = true; sourceResponses[1].length = 9;
    checkSourceFailure(spec, DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_CONTENT_LENGTH, 0, 2);

    resetSource(); sourceResponses[1].body.resize(5); sourceResponses[1].overrideLength = true;
    sourceResponses[1].length = 8;
    seedPrevious(); assert(startDownload(spec)); value = finished();
    assert(value.errorCode == DOWNLOAD_ERROR_LENGTH && value.stage == DOWNLOAD_STAGE_READ);
    assert(value.received == 5 && requests == 2); cleanHandles(); checkPrevious();

    resetSource(); sourceResponses[1].body.resize(11, 'x'); sourceResponses[1].overrideLength = true;
    sourceResponses[1].length = 8;
    seedPrevious(); assert(startDownload(spec)); value = finished();
    assert(value.errorCode == DOWNLOAD_ERROR_LENGTH && value.stage == DOWNLOAD_STAGE_READ);
    assert(value.received <= 8 && requests == 2); cleanHandles(); checkPrevious();

    resetSource(); spec.sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    seedPrevious(); assert(startDownload(spec)); value = finished();
    assert(value.errorCode == DOWNLOAD_ERROR_HASH && value.stage == DOWNLOAD_STAGE_HASH);
    assert(value.received == 8 && requests == 2); cleanHandles(); checkPrevious();

    resetSource(); std::string digest = hashText(std::string((char*)payload.data(), payload.size()));
    spec.sha256 = digest.c_str(); assert(startDownload(spec)); assert(finished().state == DONE); cleanHandles();

    // A second run must fetch the original page and use its fresh ephemeral URL.
    const char* fresh = "https://download8.mediafire.com/fresh-token/sample.pkg";
    sourceResponses.push_back(sourcePage(sourceAnchor(fresh)));
    sourceResponses.push_back(sourcePackage(fresh));
    assert(startDownload(spec)); value = finished();
    assert(value.state == DONE && requests == 4 && sourceRequestUrls[2] == SOURCE_PAGE_URL);
    assert(sourceRequestUrls[3] == fresh && templateCreates == 2 && tlsEnables == 2);
    assert(closedRequests == 4 && closedConnections == 4 && closedTemplates == 2 && closedHttp == 2);
    assert(closedSsl == 2 && closedPools == 2 && access(outputPath(true).c_str(), F_OK) != 0);
}
void mediafireTests() {
    DownloadSpec spec = { SOURCE_PAGE_URL, "sample.pkg", 8, 0 };
    sourceUrlTests(); sourceBodyTests(spec); sourceNativeErrorTests(spec);
    sourceRedirectTests(spec); sourceIntegrityTests(spec);
}

static const size_t CID_TEST_HEADER = 0x438;
static const char* CID_TEST_VALUE = "UP2047-CUSA10216_00-AGONY666AMERICAS";
static const char* CID_GITHUB_URL = "https://github.com/official/repo/releases/download/v1/sample.pkg";
void fixtureBe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    assert(offset + 4 <= bytes.size());
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = (uint8_t)(value >> (24 - i * 8));
}
void fixtureBe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    assert(offset + 8 <= bytes.size());
    for (size_t i = 0; i < 8; ++i) bytes[offset + i] = (uint8_t)(value >> (56 - i * 8));
}
std::vector<uint8_t> contentIdPackage(size_t bytes = CID_TEST_HEADER,
                                     uint32_t flags = 0x0a000000,
                                     const char* cid = CID_TEST_VALUE) {
    assert(bytes >= CID_TEST_HEADER && strlen(cid) == 36);
    std::vector<uint8_t> data(bytes, 0);
    data[0] = 0x7f; data[1] = 0x43; data[2] = 0x4e; data[3] = 0x54;
    memcpy(data.data() + 0x40, cid, 36);
    fixtureBe32(data, 0x74, 0x1a);
    fixtureBe32(data, 0x78, flags);
    fixtureBe64(data, 0x430, bytes);
    return data;
}
void resetContentIdFixture(bool mediafire, size_t bytes = CID_TEST_HEADER,
                          uint32_t flags = 0x0a000000, const char* cid = CID_TEST_VALUE) {
    if (mediafire) resetSource(); else reset(0);
    payload = contentIdPackage(bytes, flags, cid);
    if (mediafire) sourceResponses[1].body = payload;
}
DownloadSpec contentIdSpec(bool mediafire, uint64_t bytes = CID_TEST_HEADER) {
    DownloadSpec spec = { mediafire ? SOURCE_PAGE_URL : CID_GITHUB_URL, "sample.pkg", bytes, 0 };
    return spec;
}
void checkPackageOutput(const std::vector<uint8_t>& expected) {
    FILE* file = fopen(outputPath().c_str(), "rb"); assert(file);
    std::vector<uint8_t> actual(expected.size());
    assert(fread(actual.data(), 1, actual.size(), file) == actual.size());
    assert(fgetc(file) == EOF && !ferror(file));
    assert(__real_fclose(file) == 0 && actual == expected);
}
void checkContentIdFailure(bool mediafire, const DownloadSpec& spec) {
    seedPrevious(); assert(startDownload(spec, CID_TEST_VALUE));
    DownloadSnapshot value = finished();
    assert(value.state == FAILED && value.errorCode == DOWNLOAD_ERROR_PACKAGE);
    assert(value.stage == DOWNLOAD_STAGE_PACKAGE && value.nativeCode == 0);
    assert(requests == (mediafire ? 2 : 1)); cleanHandles(); checkPrevious();
}
void contentIdSpecTests() {
    std::vector<std::string> invalid;
    invalid.push_back(std::string(CID_TEST_VALUE).substr(0, 35));
    invalid.push_back(std::string(CID_TEST_VALUE) + "X");
    const size_t positions[] = { 0, 2, 6, 7, 11, 16, 17, 19, 20, 35, 35, 35 };
    const char replacements[] = { 'u', 'A', '_', 'c', 'A', '-', 'A', '_', 'a', '_', '\t', (char)0x80 };
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        std::string cid(CID_TEST_VALUE); cid[positions[i]] = replacements[i]; invalid.push_back(cid);
    }
    for (bool mediafire : { false, true }) {
        for (const std::string& cid : invalid) {
            resetContentIdFixture(mediafire); seedPrevious();
            DownloadSpec spec = contentIdSpec(mediafire);
            assert(!startDownload(spec, cid.c_str()));
            DownloadSnapshot value = downloadSnapshot();
            assert(value.state == FAILED && value.errorCode == DOWNLOAD_ERROR_SPEC);
            assert(value.stage == DOWNLOAD_STAGE_SPEC && !requests && !moduleLoads && !contentLengthCalls);
            assert(!__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE));
            assert(access(outputPath(true).c_str(), F_OK) != 0); checkPrevious();
        }
        const uint64_t undersized[] = { 0, 8, CID_TEST_HEADER - 1 };
        for (uint64_t bytes : undersized) {
            resetContentIdFixture(mediafire); seedPrevious();
            DownloadSpec spec = contentIdSpec(mediafire, bytes);
            assert(!startDownload(spec, CID_TEST_VALUE));
            DownloadSnapshot value = downloadSnapshot();
            assert(value.state == FAILED && value.errorCode == DOWNLOAD_ERROR_SPEC);
            assert(value.stage == DOWNLOAD_STAGE_SPEC && !requests && !moduleLoads && !contentLengthCalls);
            assert(access(outputPath(true).c_str(), F_OK) != 0); checkPrevious();
        }
        // An absent optional identity retains existing homebrew/magic-only behavior.
        for (const char* optional : { (const char*)0, "" }) {
            if (mediafire) resetSource(); else reset(0);
            DownloadSpec spec = contentIdSpec(mediafire, 8);
            assert(startDownload(spec, optional)); assert(finished().state == DONE); cleanHandles();
        }
    }
}
void contentIdTransferTests() {
    for (bool mediafire : { false, true }) {
        for (uint32_t flags : { 0x0a000000U, 0x0e000000U }) {
            for (size_t bytes : { CID_TEST_HEADER, CID_TEST_HEADER + 13 }) {
                resetContentIdFixture(mediafire, bytes, flags);
                DownloadSpec spec = contentIdSpec(mediafire, bytes);
                std::string digest = hashText(std::string((char*)payload.data(), payload.size()));
                spec.sha256 = digest.c_str();
                seedPrevious(); assert(startDownload(spec, CID_TEST_VALUE));
                DownloadSnapshot value = finished();
                assert(value.state == DONE && value.received == bytes && value.total == bytes);
                assert(requests == (mediafire ? 2 : 1));
                if (mediafire) assert(packageReadCalls >= (bytes + 2) / 3);
                else assert(cursor == bytes);
                cleanHandles(); checkPackageOutput(payload);
            }
        }
        // The canonical grammar also accepts a four-letter legacy title prefix.
        const char* alternate = "AB1234-SLUS12345_01-ZYXWVUTSRQPONMLK";
        resetContentIdFixture(mediafire, CID_TEST_HEADER, 0x0a000000, alternate);
        DownloadSpec spec = contentIdSpec(mediafire);
        assert(startDownload(spec, alternate)); assert(finished().state == DONE); cleanHandles();

        resetContentIdFixture(mediafire);
        if (mediafire) sourceResponses[1].body[0x40] = 'E'; else payload[0x40] = 'E';
        checkContentIdFailure(mediafire, spec);

        for (uint32_t type : { 0U, 0x1bU, 0x1a00U }) {
            resetContentIdFixture(mediafire);
            fixtureBe32(mediafire ? sourceResponses[1].body : payload, 0x74, type);
            checkContentIdFailure(mediafire, spec);
        }
        // Flags are exact values: patch flags and extra unknown bits cannot pass a mask.
        for (uint32_t flags : { 0x62300000U, 0U, 0x01000000U, 0x0a000001U, 0x0e000001U }) {
            resetContentIdFixture(mediafire);
            fixtureBe32(mediafire ? sourceResponses[1].body : payload, 0x78, flags);
            checkContentIdFailure(mediafire, spec);
        }
        const uint64_t wrongSizes[] = { CID_TEST_HEADER - 1, CID_TEST_HEADER + 1,
                                       (1ULL << 32) + CID_TEST_HEADER, UINT64_MAX };
        for (uint64_t bytes : wrongSizes) {
            resetContentIdFixture(mediafire);
            fixtureBe64(mediafire ? sourceResponses[1].body : payload, 0x430, bytes);
            checkContentIdFailure(mediafire, spec);
        }
        // HTTP declares the full required size, then EOF arrives before the header.
        // Header failure must retain PACKAGE priority over generic truncation.
        for (size_t bytes : { (size_t)3, CID_TEST_HEADER - 1 }) {
            resetContentIdFixture(mediafire);
            if (mediafire) {
                sourceResponses[1].body.resize(bytes); sourceResponses[1].overrideLength = true;
                sourceResponses[1].length = CID_TEST_HEADER;
            } else { payload.resize(bytes); advertisedLength = CID_TEST_HEADER; }
            checkContentIdFailure(mediafire, spec);
        }
    }

    // Header size fields remain 64-bit even when only a small fixture is streamed.
    reset(31); payload = contentIdPackage();
    const uint64_t large = 5368709243ULL;
    advertisedLength = large; fixtureBe64(payload, 0x430, large);
    DownloadSpec spec = contentIdSpec(false, large);
    seedPrevious(); assert(startDownload(spec, CID_TEST_VALUE)); DownloadSnapshot value = finished();
    assert(value.errorCode == DOWNLOAD_ERROR_NETWORK && value.stage == DOWNLOAD_STAGE_READ);
    assert(value.nativeCode == LARGE_READ_ERROR && value.total == large && value.received == CID_TEST_HEADER);
    cleanHandles(); checkPrevious();

    // The controller may reuse its argument storage after startDownload returns.
    resetContentIdFixture(true); sourceResponses[0].blockAfter = 3;
    char copied[37]; memcpy(copied, CID_TEST_VALUE, sizeof(copied));
    spec = contentIdSpec(true);
    assert(startDownload(spec, copied));
    for (int attempt = 0; !blocked.load() && attempt < 5000; ++attempt) sceKernelUsleep(1000);
    assert(blocked.load()); copied[0] = 'X'; releaseRead = true;
    assert(finished().state == DONE); cleanHandles(); checkPackageOutput(payload);
}
void contentIdTests() { contentIdSpecTests(); contentIdTransferTests(); }

void largePackageTests(DownloadSpec& spec) {
    const uint64_t cap = 274877906944ULL; // Independently specified 256 GiB.
    assert(PEPPY_MAX_PACKAGE_BYTES == cap);
    const uint64_t sizes[] = { 5368709243ULL, cap }; // 5 GiB + 123, then limit.
    for (uint64_t bytes : sizes) {
        reset(31);
        advertisedLength = bytes;
        spec.expectedBytes = bytes;
        seedPrevious();
        assert(startDownload(spec));
        DownloadSnapshot value = finished();
        // Advertise the full native 64-bit Content-Length, then transfer only
        // the eight-byte fixture and fail. No huge payload or allocation.
        assert(value.state == FAILED && value.errorCode == DOWNLOAD_ERROR_NETWORK);
        assert(value.stage == DOWNLOAD_STAGE_READ && value.nativeCode == LARGE_READ_ERROR);
        assert(value.total == bytes && value.received == 8 && contentLengthCalls == 1);
        assert(cursor == 8 && payload.size() == 8 && requests == 1);
        cleanHandles(); checkPrevious();
    }
    // An unknown catalog size can also accept a bounded large native length.
    reset(31); advertisedLength = cap; spec.expectedBytes = 0;
    assert(startDownload(spec));
    DownloadSnapshot value = finished();
    assert(value.errorCode == DOWNLOAD_ERROR_NETWORK && value.stage == DOWNLOAD_STAGE_READ);
    assert(value.total == cap && value.received == 8); cleanHandles();
    // The response cap applies even when the caller has no expected length.
    reset(31); advertisedLength = cap + 1;
    assert(startDownload(spec)); value = finished();
    assert(value.errorCode == DOWNLOAD_ERROR_LENGTH && value.stage == DOWNLOAD_STAGE_CONTENT_LENGTH);
    assert(value.received == 0 && cursor == 0); cleanHandles();
    for (uint64_t bytes : { cap + 1, UINT64_MAX }) {
        reset(31); spec.expectedBytes = bytes;
        assert(!startDownload(spec)); value = downloadSnapshot();
        assert(value.state == FAILED && value.errorCode == DOWNLOAD_ERROR_SPEC);
        assert(value.stage == DOWNLOAD_STAGE_SPEC && !requests && !moduleLoads && !contentLengthCalls);
        assert(!__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE));
        assert(access(outputPath(true).c_str(), F_OK) != 0);
    }
    spec.expectedBytes = 8;
}
int main(){
 assert(hashText("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 assert(hashText("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 assert(hashText("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
 assert(hashText(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
 assert(safeUrl("https://github.com/a/b/releases/download/v1/a.pkg"));assert(!safeUrl("http://github.com/a"));assert(!safeUrl("https://github.com.evil/a"));assert(!safeUrl("https://github.com@evil/a"));assert(!safeUrl("https://github.com:444/a"));assert(!safeFilename("../evil.pkg"));assert(!safeFilename("bad/name.pkg"));
 char url[URL_CAP];const char* h="Location: /next.pkg\r\n";assert(redirectUrl("https://github.com/a",h,strlen(h),url)&&!strcmp(url,"https://github.com/next.pkg"));h="Location: /a\r\nLocation: /b\r\n";assert(!redirectUrl("https://github.com/a",h,strlen(h),url));
 std::string largeHeader="Location: https://release-assets.githubusercontent.com/a.pkg\r\nX-Padding: ";largeHeader.resize(RESPONSE_HEADER_CAP,'a');assert(redirectUrl("https://github.com/a",largeHeader.data(),largeHeader.size(),url));largeHeader.push_back('a');assert(!redirectUrl("https://github.com/a",largeHeader.data(),largeHeader.size(),url));
 DownloadSpec spec={"https://github.com/official/repo/releases/download/v1/app.pkg","sample.pkg",8,0};
 reset(0);assert(startDownload(spec));assert(finished().state==DONE);cleanHandles();assert(access(outputPath().c_str(),F_OK)==0);
 reset(1);assert(startDownload(spec));assert(finished().state==DONE&&requests==2);cleanHandles();
 reset(2);assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_REDIRECT);cleanHandles();assert(access(outputPath().c_str(),F_OK)!=0);
 reset(3);assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_HTTP);cleanHandles();
 reset(4);assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_LENGTH);cleanHandles();
 reset(5);seedPrevious();payload[0]=0;assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_PACKAGE);cleanHandles();checkPrevious();
 reset(6);assert(startDownload(spec));while(!blocked.load())sceKernelUsleep(1000);assert(!startDownload(spec));cancelDownload();assert(finished().state==CANCELLED);cleanHandles();
 reset(7);assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_TLS);cleanHandles();
 assert(downloadSnapshot().stage==DOWNLOAD_STAGE_SEND&&downloadSnapshot().nativeCode==-1&&downloadSnapshot().sslCode==1);
 reset(8);assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_REDIRECT&&requests==6);cleanHandles();
 reset(9);payload.resize(5);assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_LENGTH);cleanHandles();
 reset(10);payload.resize(11,'e');assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_LENGTH);cleanHandles();
 for(int fault=11;fault<=13;++fault){reset(fault);assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_FILESYSTEM);cleanHandles();assert(access(outputPath().c_str(),F_OK)!=0);}
 reset(14);assert(startDownload(spec));assert(finished().state==DONE);cleanHandles();
 reset(14);spec.expectedBytes=0;assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_LENGTH);cleanHandles();spec.expectedBytes=8;
 reset(0);spec.sha256="0000000000000000000000000000000000000000000000000000000000000000";assert(startDownload(spec));assert(finished().errorCode==DOWNLOAD_ERROR_HASH);cleanHandles();
 reset(0);std::string digest=hashText(std::string((char*)payload.data(),payload.size()));spec.sha256=digest.c_str();assert(startDownload(spec));assert(finished().state==DONE);cleanHandles();
 spec.sha256=0;
 reset(15);assert(startDownload(spec));assert(finished().state==DONE&&moduleLoads==0);cleanHandles();
 reset(16);assert(startDownload(spec));assert(finished().state==DONE&&moduleLoads==4);cleanHandles();
 reset(17);assert(startDownload(spec));DownloadSnapshot diag=finished();assert(diag.state==FAILED&&diag.stage==DOWNLOAD_STAGE_MODULE_NET&&(uint32_t)diag.nativeCode==0x805a1001);assert(closedPools==0&&closedHttp==0);
 reset(18);assert(startDownload(spec));assert(finished().state==DONE);cleanHandles();
 reset(19);assert(startDownload(spec));diag=finished();assert(diag.stage==DOWNLOAD_STAGE_NETCTL_INIT&&(uint32_t)diag.nativeCode==0x80412101);
 reset(20);assert(startDownload(spec));diag=finished();assert(diag.stage==DOWNLOAD_STAGE_NETCTL_STATE&&(uint32_t)diag.nativeCode==0x80412103);
 reset(21);assert(startDownload(spec));diag=finished();assert(diag.errorCode==DOWNLOAD_ERROR_NOT_READY&&diag.stage==DOWNLOAD_STAGE_NETCTL_STATE&&diag.networkState==0&&stateQueries==101);
 reset(22);assert(startDownload(spec));while(downloadSnapshot().stage!=DOWNLOAD_STAGE_NETCTL_STATE)sceKernelUsleep(1000);cancelDownload();assert(finished().state==CANCELLED&&closedPools==0);
 reset(23);netError=200;assert(startDownload(spec));diag=finished();assert(diag.stage==DOWNLOAD_STAGE_NET_POOL&&diag.nativeCode==-1&&diag.networkCode==200);
 reset(24);assert(startDownload(spec));diag=finished();assert(diag.stage==DOWNLOAD_STAGE_RESOLVE_TIMEOUT&&(uint32_t)diag.nativeCode==0x804310fe&&closedPools==1&&closedHttp==1);
 reset(0);assert(startDownload(spec));assert(finished().state==DONE);cleanHandles();
 cursor=0;assert(startDownload(spec));assert(finished().state==DONE);assert(closedPools==2&&closedHttp==2&&closedSsl==2);
 const uint32_t threadErrors[]={0x80020022,0x80020016,0x8002000c};
 for(int fault=25;fault<=27;++fault){reset(fault);assert(!startDownload(spec));diag=downloadSnapshot();assert(diag.state==FAILED&&diag.errorCode==DOWNLOAD_ERROR_THREAD&&diag.stage==DOWNLOAD_STAGE_THREAD&&(uint32_t)diag.nativeCode==threadErrors[fault-25]&&moduleLoads==0&&!__atomic_load_n(&g_busy,__ATOMIC_ACQUIRE));}
 reset(28);secure=manual=true;connectionHeaderCap=5000;assert((uint32_t)sceHttpSendRequest(1,0,0)==0x80431073);assert(startDownload(spec));assert(finished().state==DONE&&requests==2&&nativeHeaderCap==65536&&headerConfigured);cleanHandles();
 reset(29);assert(startDownload(spec));diag=finished();assert(diag.state==FAILED&&diag.errorCode==DOWNLOAD_ERROR_RESPONSE_HEADERS&&diag.stage==DOWNLOAD_STAGE_SEND&&(uint32_t)diag.nativeCode==0x80431073&&(uint32_t)diag.networkCode==0x80431073&&diag.sslCode==0&&diag.sslDetails==0);cleanHandles();
 reset(30);assert(startDownload(spec));diag=finished();assert(diag.state==FAILED&&diag.stage==DOWNLOAD_STAGE_HEADER_LIMIT&&(uint32_t)diag.nativeCode==0x804311fe&&requests==0&&closedTemplates==1&&closedHttp==1&&closedSsl==1&&closedPools==1);
 largePackageTests(spec);
 mediafireTests();
 contentIdTests();
 spec.filename="../sample.pkg";assert(!startDownload(spec));assert(downloadSnapshot().errorCode==DOWNLOAD_ERROR_SPEC);
 FILE* log=fopen((std::string(testDirectory())+"/download.log").c_str(),"rb");assert(log);char logged[8192]={0};size_t loggedBytes=fread(logged,1,sizeof(logged)-1,log);assert(loggedBytes>0&&!ferror(log));assert(__real_fclose(log)==0);assert(!strstr(logged,"https://")&&!strstr(logged,"token="));
 unlink(outputPath().c_str());unlink((std::string(testDirectory())+"/download.log").c_str());rmdir(testDirectory());
 puts("All digest, URL, redirect, length, I/O failure, cleanup, TLS, cancellation, MediaFire source, and Content ID tests passed.");
}
