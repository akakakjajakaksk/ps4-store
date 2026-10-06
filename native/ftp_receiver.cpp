#include "ftp_receiver.h"
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {
static FtpReceiverSnapshot g = {};
static OrbisPthread g_thread;
static volatile int g_stop = 0;
static int g_listener = -1;

static bool safeName(const char* s) {
    if (!s || !*s || strstr(s, "..") || strchr(s, '/') || strchr(s, '\\')) return false;
    size_t n=strlen(s); return n>4 && n<80 && !strcasecmp(s+n-4, ".pkg");
}
static const char* folderFor(const char* name) {
    // Mobile clients can prefix filenames with base_, update_, or dlc_.
    if (!strncasecmp(name,"update_",7)) return "update";
    if (!strncasecmp(name,"dlc_",4)) return "dlc";
    return "base";
}
static bool sendText(int fd,const char* s){ return sceNetSend(fd,s,strlen(s),0)>=0; }
static int makeListener(uint16_t port) {
    int fd=sceNetSocket("peppy-ftp",ORBIS_NET_AF_INET,ORBIS_NET_SOCK_STREAM,0);
    if(fd<0)return fd;
    OrbisNetSockaddrIn a={}; a.sin_len=sizeof(a); a.sin_family=ORBIS_NET_AF_INET;
    a.sin_port=ORBIS_NET_HTONS(port); a.sin_addr.s_addr=ORBIS_NET_INADDR_ANY;
    if(sceNetBind(fd,(OrbisNetSockaddr*)&a,sizeof(a))<0 || sceNetListen(fd,2)<0){sceNetSocketClose(fd);return -1;}
    return fd;
}
static bool recvLine(int fd,char* out,size_t cap){
    size_t n=0; while(n+1<cap){char c=0;int r=sceNetRecv(fd,&c,1,0);if(r<=0)return false;if(c=='\n')break;if(c!='\r')out[n++]=c;}out[n]=0;return true;
}
static void* worker(void*) {
    while(!g_stop){
        OrbisNetSockaddrIn peer={}; unsigned len=sizeof(peer);
        int c=sceNetAccept(g_listener,(OrbisNetSockaddr*)&peer,&len);
        if(c<0){sceKernelUsleep(50000);continue;}
        sendText(c,"220 Peppy Store FTP - authorized PKG inbox\r\n");
        int dataListen=-1; char line[256];
        while(!g_stop && recvLine(c,line,sizeof(line))){
            if(!strncasecmp(line,"USER ",5)) sendText(c,"331 Password optional\r\n");
            else if(!strncasecmp(line,"PASS",4)) sendText(c,"230 Logged in\r\n");
            else if(!strcasecmp(line,"SYST")) sendText(c,"215 UNIX Type: L8\r\n");
            else if(!strncasecmp(line,"TYPE ",5)) sendText(c,"200 Type set\r\n");
            else if(!strcasecmp(line,"PWD")) sendText(c,"257 \"/\"\r\n");
            else if(!strncasecmp(line,"CWD ",4)) sendText(c,"250 Directory changed\r\n");
            else if(!strcasecmp(line,"PASV")){
                if(dataListen>=0)sceNetSocketClose(dataListen);
                dataListen=-1;
                for(uint16_t p=2122;p<2142 && dataListen<0;++p){
                    int x=makeListener(p); if(x>=0){dataListen=x; unsigned p1=p/256,p2=p%256; char msg[96];
                        unsigned ip=peer.sin_addr.s_addr; // control peer only selects route; advertise console IP below when available
                        (void)ip;
                        unsigned a=127,b=0,d=0,e=1;
                        if(g.ip[0]) sscanf(g.ip,"%u.%u.%u.%u",&a,&b,&d,&e);
                        snprintf(msg,sizeof(msg),"227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",a,b,d,e,p1,p2);
                        sendText(c,msg);
                    }
                }
                if(dataListen<0)sendText(c,"425 Cannot open passive connection\r\n");
            } else if(!strncasecmp(line,"STOR ",5)){
                const char* name=line+5;
                if(!safeName(name)){sendText(c,"553 Invalid PKG filename\r\n");continue;}
                if(dataListen<0){sendText(c,"425 Use PASV first\r\n");continue;}
                sendText(c,"150 Opening data connection\r\n");
                OrbisNetSockaddrIn dp={}; unsigned dl=sizeof(dp);
                int dfd=sceNetAccept(dataListen,(OrbisNetSockaddr*)&dp,&dl);
                sceNetSocketClose(dataListen); dataListen=-1;
                if(dfd<0){sendText(c,"425 Data connection failed\r\n");continue;}
                const char* kind=folderFor(name);
                mkdir("/data/peppy-store",0777); mkdir("/data/peppy-store/inbox",0777);
                char dir[128],path[256]; snprintf(dir,sizeof(dir),"/data/peppy-store/inbox/%s",kind);mkdir(dir,0777);
                snprintf(path,sizeof(path),"%s/%s",dir,name);
                FILE* f=fopen(path,"wb"); uint64_t total=0; bool ok=f!=0; char buf[65536];
                while(ok){int r=sceNetRecv(dfd,buf,sizeof(buf),0);if(r==0)break;if(r<0){ok=false;break;}if(fwrite(buf,1,r,f)!=(size_t)r){ok=false;break;}total+=(uint64_t)r;}
                if(f)fclose(f); sceNetSocketClose(dfd);
                if(ok){g.filesReceived++;g.bytesReceived+=total;snprintf(g.lastFile,sizeof(g.lastFile),"%s",name);snprintf(g.lastKind,sizeof(g.lastKind),"%s",kind);sendText(c,"226 Transfer complete\r\n");}
                else {remove(path);g.lastError=-1;sendText(c,"451 Transfer failed\r\n");}
            } else if(!strcasecmp(line,"NOOP")) sendText(c,"200 OK\r\n");
            else if(!strcasecmp(line,"QUIT")) {sendText(c,"221 Bye\r\n");break;}
            else sendText(c,"502 Command not supported\r\n");
        }
        if(dataListen>=0)sceNetSocketClose(dataListen);
        sceNetSocketClose(c);
    }
    return 0;
}
}

bool ftpReceiverStart(){
    if(g.running)return true;
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    OrbisNetCtlInfo info={};
    if(sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS,&info)==0) snprintf(g.ip,sizeof(g.ip),"%s",info.ip_address);
    else snprintf(g.ip,sizeof(g.ip),"127.0.0.1");
    g_listener=makeListener(2121); if(g_listener<0){g.lastError=g_listener;return false;}
    g_stop=0; int rc=scePthreadCreate(&g_thread,0,worker,0,"peppy-ftp");
    if(rc){sceNetSocketClose(g_listener);g_listener=-1;g.lastError=rc;return false;}
    g.running=true;g.port=2121;return true;
}
void ftpReceiverStop(){if(!g.running)return;g_stop=1;if(g_listener>=0){sceNetSocketClose(g_listener);g_listener=-1;}scePthreadJoin(g_thread,0);g.running=false;}
FtpReceiverSnapshot ftpReceiverSnapshot(){return g;}
