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
static std::atomic<bool> aborted(false), blocked(false);
static std::string requestUrl;
static bool secure=false, manual=false;
static int moduleLoads=0, moduleProbes=0, stateQueries=0, netError=0;
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
extern "C" int32_t sceHttpCreateTemplate(int32_t,const char*,int32_t,int32_t proxy){assert(proxy==0);return 4;}
extern "C" int32_t sceHttpsEnableOption(int32_t,uint32_t flags){assert(flags==0xbd);secure=true;return 0;}
extern "C" int32_t mockAuto(int32_t,int32_t) __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t mockAuto(int32_t,int32_t e){assert(e==0);manual=true;return 0;}
extern "C" int32_t mockRecv(int32_t,uint32_t) __asm__("sceHttpSetRecvTimeOut");
extern "C" int32_t mockRecv(int32_t,uint32_t n){assert(n==15000000);return 0;}
extern "C" int32_t mockSslError(int32_t,int32_t*,uint32_t*) __asm__("sceHttpsGetSslError");
extern "C" int32_t mockSslError(int32_t,int32_t* e,uint32_t* d){*e=mode==7?1:0;*d=0;return 0;}
extern "C" int32_t sceHttpSetResolveTimeOut(int32_t,uint32_t){return mode==24?(int32_t)0x804310fe:0;}
extern "C" int32_t sceHttpSetConnectTimeOut(int32_t,uint32_t){return 0;}
extern "C" int32_t sceHttpSetSendTimeOut(int32_t,uint32_t){return 0;}
extern "C" int32_t sceHttpCreateConnectionWithURL(int32_t,const char* url,bool){requestUrl=url;return 5;}
extern "C" int32_t sceHttpCreateRequestWithURL(int32_t,int32_t,const char*,uint64_t){return 10+(++requests);}
extern "C" int32_t sceHttpAddRequestHeader(int32_t,const char*,const char*,int32_t){return 0;}
extern "C" int32_t sceHttpSendRequest(int32_t,const void*,size_t){assert(secure&&manual);if(mode==6){blocked=true;while(!aborted.load())sceKernelUsleep(1000);return -1;}if(mode==7)return -1;return 0;}
extern "C" int32_t sceHttpGetStatusCode(int32_t,int32_t* out){*out=(mode==1&&requests==1)||mode==2||mode==8?302:mode==3?404:200;return 0;}
extern "C" int32_t sceHttpGetLastErrno(int32_t,int32_t* out){*out=netError;return 0;}
extern "C" int32_t sceHttpGetAllResponseHeaders(int32_t,char** out,size_t* n){static char good[]="HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/file.pkg?token=abc\r\n\r\n";static char evil[]="Location: https://github.com.evil.example/file.pkg\r\n";*out=mode==2?evil:good;*n=strlen(*out);return 0;}
extern "C" int32_t sceHttpGetResponseContentLength(int32_t,int32_t* type,size_t* n){*type=mode==14?1:ORBIS_HTTP_CONTENTLEN_EXIST;*n=mode==9||mode==10?8:payload.size()+(mode==4?1:0);return 0;}
extern "C" int32_t sceHttpReadData(int32_t,void* out,uint32_t max){if(cursor==payload.size())return 0;size_t n=payload.size()-cursor;if(n>max)n=max;if(n>3)n=3;memcpy(out,payload.data()+cursor,n);cursor+=n;return n;}
extern "C" int32_t sceHttpAbortRequest(int32_t){aborted=true;return 0;}
extern "C" int32_t sceHttpDeleteRequest(int32_t){++closedRequests;return 0;}
extern "C" int32_t sceHttpDeleteConnection(int32_t){++closedConnections;return 0;}
extern "C" int32_t sceHttpDeleteTemplate(int32_t){++closedTemplates;return 0;}
extern "C" int32_t sceHttpTerm(int32_t){++closedHttp;return 0;}

std::string hashText(const std::string& input){Sha256 s;for(size_t i=0;i<input.size();i+=7)s.update((const uint8_t*)input.data()+i,input.size()-i>7?7:input.size()-i);uint8_t digest[32];s.finish(digest);char buf[65];for(int i=0;i<32;++i)sprintf(buf+2*i,"%02x",digest[i]);return buf;}
DownloadSnapshot finished(){for(int i=0;i<5000;++i){if(!__atomic_load_n(&g_busy,__ATOMIC_ACQUIRE))return downloadSnapshot();sceKernelUsleep(1000);}assert(false);return downloadSnapshot();}
void reset(int nextMode){assert(!__atomic_load_n(&g_busy,__ATOMIC_ACQUIRE));mode=nextMode;cursor=0;requests=0;moduleLoads=moduleProbes=stateQueries=0;netError=0;aborted=false;blocked=false;secure=false;manual=false;closedRequests=closedConnections=closedTemplates=closedHttp=closedSsl=closedPools=0;payload={0x7f,0x43,0x4e,0x54,'a','b','c','d'};unlink(outputPath().c_str());unlink(outputPath(true).c_str());}
void cleanHandles(){assert(closedRequests==requests&&closedConnections==requests);assert(closedTemplates==1&&closedHttp==1&&closedSsl==1&&closedPools==1);assert(access(outputPath(true).c_str(),F_OK)!=0);}
void seedPrevious(){FILE* f=fopen(outputPath().c_str(),"wb");assert(f);assert(__real_fwrite("previous",1,8,f)==8);assert(__real_fclose(f)==0);}
void checkPrevious(){FILE* f=fopen(outputPath().c_str(),"rb");assert(f);char data[9]={0};assert(fread(data,1,8,f)==8);assert(!strcmp(data,"previous"));assert(__real_fclose(f)==0);}
int main(){
 assert(hashText("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 assert(hashText("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 assert(hashText("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
 assert(hashText(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
 assert(safeUrl("https://github.com/a/b/releases/download/v1/a.pkg"));assert(!safeUrl("http://github.com/a"));assert(!safeUrl("https://github.com.evil/a"));assert(!safeUrl("https://github.com@evil/a"));assert(!safeUrl("https://github.com:444/a"));assert(!safeFilename("../evil.pkg"));assert(!safeFilename("bad/name.pkg"));
 char url[URL_CAP];const char* h="Location: /next.pkg\r\n";assert(redirectUrl("https://github.com/a",h,strlen(h),url)&&!strcmp(url,"https://github.com/next.pkg"));h="Location: /a\r\nLocation: /b\r\n";assert(!redirectUrl("https://github.com/a",h,strlen(h),url));
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
 spec.filename="../sample.pkg";assert(!startDownload(spec));assert(downloadSnapshot().errorCode==DOWNLOAD_ERROR_SPEC);
 FILE* log=fopen((std::string(testDirectory())+"/download.log").c_str(),"rb");assert(log);char logged[8192]={0};fread(logged,1,sizeof(logged)-1,log);__real_fclose(log);assert(!strstr(logged,"https://")&&!strstr(logged,"token="));
 unlink(outputPath().c_str());unlink((std::string(testDirectory())+"/download.log").c_str());rmdir(testDirectory());
 puts("All digest, URL, redirect, length, I/O failure, cleanup, TLS, and cancellation tests passed.");
}
