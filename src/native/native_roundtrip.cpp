// This file is never part of the default dry-run build.
#if !defined(ITB_SL_NATIVE_EXPERIMENT) || ITB_SL_NATIVE_EXPERIMENT != 1
#error Explicit ITB_SL_NATIVE_EXPERIMENT=1 is required
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <assert.h>
#include "native_action.h"
#ifdef ITB_SL_P04_SERVICE
#include "p04_service.h"
#endif
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"ole32.lib")

static HMODULE module;
static std::wstring root, target;
static std::string session, expectedHash, savedName, savedHash;
static slnative::Action action;
static unsigned char argument[24];
static unsigned int currentThis;
static void *saveFn, *loadFn, *continueA, *targetA;
static unsigned char originalA[10];
static volatile LONG hookA=0, inA=0, visits=0, reentry=0;
static volatile LONG nativeAL=-1, targetBytes=0, targetLuaOK=0;
static DWORD actionTid=0;
static ULONGLONG queuedAt=0;
static unsigned long emittedSeq=0;
static std::string ackText;
static const char* exeHash="31fe352655982398fb3ee8b0bbe80efd5d65e3a9aa11e3dc39d0364354493fe9";
#ifdef ITB_SL_P04_SERVICE
static volatile LONG p04Tick=0;
static char p04RejectReason[64]={0};
static bool P04PermitValid(unsigned long seq,const std::string& kind,const std::string& name);
static void P04RejectNoCall(unsigned long seq,const char* kind,const char* reason);
static bool P04BeforeCall();
static DWORD P04WorkerInit();
static volatile LONG p04NativeCalled=0;
static bool p04PrepareOk=false;
static std::string p04Profile;
#endif

static std::string Utf8(const std::wstring& value) {
    int n=WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),NULL,0,NULL,NULL);
    std::string text(n,'\0'); if(n) WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),&text[0],n,NULL,NULL); return text;
}
static void Log(const char* fmt,...) {
    if(root.empty()) return;
    FILE* file=NULL; if(_wfopen_s(&file,(root+L"\\native_probe.log").c_str(),L"a") || !file) return;
    fprintf(file,"[%lu] ",GetTickCount()); va_list args; va_start(args,fmt); vfprintf(file,fmt,args); va_end(args); fclose(file);
}
static std::string Hash(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg=NULL; BCRYPT_HASH_HANDLE hash=NULL;
    unsigned char digest[32],buf[16384]; DWORD count=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)==0 && BCryptCreateHash(alg,&hash,NULL,0,NULL,0,0)==0) {
        ok=true;
        for(;;) { if(!ReadFile(file,buf,sizeof(buf),&count,NULL)) {ok=false;break;} if(!count)break;
            if(BCryptHashData(hash,buf,count,0)!=0) {ok=false;break;} }
        if(ok) ok=BCryptFinishHash(hash,digest,32,0)==0;
    }
    if(hash) BCryptDestroyHash(hash); if(alg) BCryptCloseAlgorithmProvider(alg,0); CloseHandle(file);
    if(!ok)return ""; char hex[65]; for(int i=0;i<32;++i)sprintf_s(hex+i*2,65-i*2,"%02x",digest[i]); return hex;
}
static std::string Read(const wchar_t* name) { std::string text; slprobe::ReadRecord(root+L"\\"+name,text); return text; }
static std::vector<std::string> Split(std::string text) {
    if(!text.empty() && text.back()=='\n') text.pop_back();
    std::vector<std::string> parts; size_t begin=0,pos;
    while((pos=text.find('|',begin))!=std::string::npos){parts.push_back(text.substr(begin,pos-begin));begin=pos+1;}
    parts.push_back(text.substr(begin)); return parts;
}
static void SetAck(const char* phase) {
    char tail[180]; sprintf_s(tail,"|%010lu|%s|native|%s\n",action.last,
        action.last?action.request.kind.c_str():"ready",phase);
    ackText=session+tail;
    slprobe::Publish(root+L"\\native_ack.txt",ackText);
}
static bool Calling() { return action.phase==slnative::Running && actionTid==GetCurrentThreadId(); }
static bool IsTarget(const std::wstring& path) {
    std::wstring p=path; if(p.rfind(L"\\\\?\\",0)==0)p.erase(0,4);
    for(auto& c:p)if(c==L'/')c=L'\\'; return _wcsicmp(p.c_str(),target.c_str())==0;
}
// Exists-gate observation: wrap only the two CALLs at 0x609B5F / 0x609B6D.
// The wrapper records bounded inputs, calls the original target exactly once and
// hands its return value back untouched. File I/O happens after the native call.
static void* origExistsFn;
static volatile LONG inGate, gateNested;
struct GateRecord {
    volatile LONG written;
    unsigned int label, tid, ptr, len, cap, dl, al;
    int sso, heap, readable, truncated, complete;
    char text[260];
};
static GateRecord gateRecords[2];
static unsigned int gateEntryECX, gateEntryEDX, gateEntryLabel, gateReturnEAX;
static std::string EscapeText(const char* text,int length) {
    std::string safe;
    for(int i=0;i<length;++i) {
        unsigned char c=(unsigned char)text[i];
        if(c>=32 && c<127) safe+=(char)c;
        else { char hex[5]; sprintf_s(hex,"\\x%02X",c); safe+=hex; }
    }
    return safe;
}
extern "C" void RecordGate(unsigned int label,unsigned int ecx,unsigned int edx,unsigned int eax) {
    if(!Calling())return;
    int idx=label?1:0;
    GateRecord& r=gateRecords[idx];
    r.written=1; r.label=label; r.tid=GetCurrentThreadId(); r.ptr=ecx;
    r.dl=edx&0xFF; r.al=eax&0xFF; r.len=0; r.cap=0; r.sso=0; r.heap=0;
    r.readable=0; r.truncated=0; r.complete=0; r.text[0]=0;
    // Both layouts confirmed for this build are read with a fixed 256-byte cap.
    // Unknown layouts and unreadable pointers stay unreadable; nothing is invented.
    __try {
        unsigned int len=*(unsigned int*)(ecx+16);
        unsigned int cap=*(unsigned int*)(ecx+20);
        r.len=len; r.cap=cap;
        if(cap==15 && len<=15) {
            r.sso=1;
            memcpy(r.text,(const void*)ecx,len);
            r.text[len]=0;
            r.readable=1;
        } else {
            r.heap=1;
            const char* data=*(const char**)ecx;
            if(len==0) {
                r.text[0]=0;
                r.readable=1;
            } else if(data) {
                unsigned int n=len<256?len:256;
                memcpy(r.text,data,n);
                r.text[n]=0;
                r.truncated=len>256;
                r.readable=1;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { r.readable=0; }
    r.complete=r.readable;
}
static void ResetGates() {
    for(int i=0;i<2;++i) memset(&gateRecords[i],0,sizeof(gateRecords[i]));
    gateNested=0; inGate=0;
}
static void PublishGates() {
    int nested=gateNested;
    for(int i=0;i<2;++i) {
        GateRecord& r=gateRecords[i];
        if(!r.written)continue;
        Log("EXISTS seq=%lu path=%s tid=%lu ptr=%08X len=%u cap=%u sso=%d heap=%d readable=%d truncated=%d dl=%u al=%u complete=%d text=%s\n",
            action.last,i?"backup":"main",r.tid,r.ptr,r.len,r.cap,r.sso,r.heap,r.readable,r.truncated,r.dl,r.al,r.complete&&!nested,
            EscapeText(r.text,(int)strlen(r.text)).c_str());
    }
    if(nested)Log("EXISTS_NESTED seq=%lu\n",action.last);
    if(!gateRecords[0].written && !gateRecords[1].written)Log("EXISTS_MISSING seq=%lu\n",action.last);
}
extern "C" void ThunkCommon();
extern "C" __declspec(naked) void ThunkMain() { __asm {
    mov eax,0
    jmp ThunkCommon
} }
extern "C" __declspec(naked) void ThunkBackup() { __asm {
    mov eax,1
    jmp ThunkCommon
} }
extern "C" __declspec(naked) void ThunkCommon() { __asm {
    pushfd
    pushad
    mov ebp,eax
    mov eax,1
    xchg eax,dword ptr [inGate]
    test eax,eax
    jnz gate_nested
    mov dword ptr [gateEntryLabel],ebp
    mov dword ptr [gateEntryECX],ecx
    mov dword ptr [gateEntryEDX],edx
    popad
    popfd
    call dword ptr [origExistsFn]
    mov dword ptr [gateReturnEAX],eax
    pushfd
    pushad
    push dword ptr [gateReturnEAX]
    push dword ptr [gateEntryEDX]
    push dword ptr [gateEntryECX]
    push dword ptr [gateEntryLabel]
    call RecordGate
    add esp,16
    popad
    popfd
    mov dword ptr [inGate],0
    ret
gate_nested:
    mov dword ptr [gateNested],1
    popad
    popfd
    call dword ptr [origExistsFn]
    ret
} }
// Tree-lookup observation: wrap only 0x46DFD0. Read-only, bounded, no extra lookups.
struct FindRecord {
    volatile LONG written;
    unsigned int seq, map, key, node, entry, state, len, cap;
    int sso, heap, readable, truncated, isEnd;
    char text[260];
};
static FindRecord findRecords[16];
static volatile LONG findCount, findOverflow, inFind;
static void* findTrampoline;
static unsigned int findKey, findMap, findRet, findCallerRet;
extern "C" void RecordFind(unsigned int map,unsigned int key,unsigned int node) {
    if(!Calling())return;
    LONG index=InterlockedIncrement(&findCount)-1;
    if(index>=16){findOverflow=1;return;}
    FindRecord& r=findRecords[index];
    r.written=1; r.seq=action.last; r.map=map; r.key=key; r.node=node;
    r.len=0; r.cap=0; r.sso=0; r.heap=0; r.readable=0; r.truncated=0;
    r.isEnd=0; r.entry=0; r.state=0;
    __try {
        unsigned int len=*(unsigned int*)(key+16);
        unsigned int cap=*(unsigned int*)(key+20);
        r.len=len; r.cap=cap;
        if(cap==15 && len<=15) {
            r.sso=1;
            memcpy(r.text,(const void*)key,len);
            r.text[len]=0;
            r.readable=1;
        } else {
            r.heap=1;
            const char* data=*(const char**)key;
            if(len==0) { r.text[0]=0; r.readable=1; }
            else if(data) {
                unsigned int n=len<256?len:256;
                memcpy(r.text,data,n);
                r.text[n]=0;
                r.truncated=len>256;
                r.readable=1;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { r.readable=0; }
    r.isEnd=(node==*(unsigned int*)map);
    if(!r.isEnd) {
        __try {
            r.entry=*(unsigned int*)(node+0x28);
            if(r.entry) r.state=*(unsigned int*)(r.entry+0x20);
        } __except(EXCEPTION_EXECUTE_HANDLER) { r.state=0xFFFFFFFFu; }
    }
}
static void ResetFind(){ memset(findRecords,0,sizeof(findRecords)); findCount=0; findOverflow=0; }
static void PublishFind(){
    LONG count=findCount; if(count>16)count=16;
    for(int i=0;i<count;++i) {
        FindRecord& r=findRecords[i]; if(!r.written)continue;
        Log("FIND seq=%lu idx=%d map=%08X key=%08X node=%08X end=%d entry=%08X state=%08X len=%u cap=%u sso=%d heap=%d readable=%d truncated=%d text=%s\n",
            action.last,i,r.map,r.key,r.node,r.isEnd,r.entry,r.state,r.len,r.cap,r.sso,r.heap,r.readable,r.truncated,
            EscapeText(r.text,(int)strlen(r.text)).c_str());
    }
    if(findOverflow)Log("FIND_OVERFLOW seq=%lu\n",action.last);
    if(count==0)Log("FIND_NONE seq=%lu\n",action.last);
}
#ifdef NATIVE_OFFLINE_ABI_TEST
static bool gateClobberECX=false;
#endif
extern "C" bool GateCalling(){
    bool result=Calling();
#ifdef NATIVE_OFFLINE_ABI_TEST
    if(gateClobberECX) { __asm { xor ecx,ecx } }
#endif
    return result;
}
extern "C" __declspec(naked) void ThunkFind(){__asm{
    pushfd
    pushad
    call GateCalling
    test al,al
    jz find_pass
    mov eax,1
    xchg eax,dword ptr [inFind]
    test eax,eax
    jnz find_pass
    mov eax,dword ptr [esp+40]
    mov dword ptr [findKey],eax
    mov dword ptr [findMap],ecx
    mov eax,dword ptr [esp+36]
    mov dword ptr [findCallerRet],eax
    popad
    popfd
    mov dword ptr [esp],offset AfterFind
    jmp dword ptr [findTrampoline]
find_pass:
    popad
    popfd
    jmp dword ptr [findTrampoline]
AfterFind:
    mov dword ptr [findRet],eax
    mov dword ptr [inFind],0
    pushfd
    pushad
    push dword ptr [findRet]
    push dword ptr [findKey]
    push dword ptr [findMap]
    call RecordFind
    add esp,12
    popad
    popfd
    jmp dword ptr [findCallerRet]
}}
static void* MakeTrampoline(unsigned char* target,int length) {
    unsigned char* mem=(unsigned char*)VirtualAlloc(NULL,32,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!mem)return NULL;
    memcpy(mem,target,length);
    mem[length]=0xE9;
    int rel=(int)((target+length)-(mem+length+5));
    memcpy(mem+length+1,&rel,4);
    return mem;
}
static bool PatchJump(unsigned char* site,void* target,int length) {
    if(length<5)return false;
    DWORD old;
    if(!VirtualProtect(site,length,PAGE_EXECUTE_READWRITE,&old))return false;
    unsigned char patch[8]; patch[0]=0xE9;
    int rel=(int)((unsigned char*)target-(site+5));
    memcpy(patch+1,&rel,4);
    for(int i=5;i<length;++i)patch[i]=0x90;
    memcpy(site,patch,length);
    DWORD unused; VirtualProtect(site,length,old,&unused);
    FlushInstructionCache(GetCurrentProcess(),site,length);
    return true;
}
// Entry-check observation: wrap the 9-byte vtable check inside 0x46F260.
struct EntryCheckRecord {
    volatile LONG written;
    unsigned int seq, entry, vtable, checkFn, request, requestResult, al;
};
static EntryCheckRecord entryCheckRecords[8];
static volatile LONG entryCheckCount, entryCheckOverflow;
static unsigned int entryPtr, entryVtable, entryCheckFn, entryRequest;
extern "C" void RecordEntryCheck(unsigned int entry,unsigned int vtable,unsigned int checkFn,unsigned int al) {
    if(!Calling())return;
    LONG index=InterlockedIncrement(&entryCheckCount)-1;
    if(index>=8){entryCheckOverflow=1;return;}
    EntryCheckRecord& r=entryCheckRecords[index];
    r.written=1; r.seq=action.last; r.entry=entry; r.vtable=vtable;
    r.checkFn=checkFn; r.al=al&0xFF; r.request=entryRequest; r.requestResult=0;
    if(entryRequest) {
        __try { r.requestResult=*(unsigned int*)(entryRequest+4); }
        __except(EXCEPTION_EXECUTE_HANDLER) { r.requestResult=0xFFFFFFFFu; }
    }
}
static void ResetEntryCheck(){
    memset(entryCheckRecords,0,sizeof(entryCheckRecords));
    entryCheckCount=0; entryCheckOverflow=0;
    entryPtr=0; entryVtable=0; entryCheckFn=0; entryRequest=0;
}
static void PublishEntryCheck(){
    LONG count=entryCheckCount; if(count>8)count=8;
    for(int i=0;i<count;++i) {
        EntryCheckRecord& r=entryCheckRecords[i];
        if(!r.written)continue;
        Log("ENTRY_CHECK seq=%lu idx=%d entry=%08X vtable=%08X check=%08X al=%u request=%08X result=%08X\n",
            action.last,i,r.entry,r.vtable,r.checkFn,r.al,r.request,r.requestResult);
    }
    if(entryCheckOverflow)Log("ENTRY_CHECK_OVERFLOW seq=%lu\n",action.last);
    if(count==0)Log("ENTRY_CHECK_MISSING seq=%lu\n",action.last);
}
extern "C" __declspec(naked) void ThunkEntryCheck(){__asm{
    pushfd
    pushad
    call GateCalling
    test al,al
    jz entry_pass
    mov dword ptr [entryPtr],esi
    mov eax,dword ptr [esi]
    mov dword ptr [entryVtable],eax
    mov eax,dword ptr [eax+4]
    mov dword ptr [entryCheckFn],eax
    mov eax,dword ptr [ebp+0Ch]
    mov dword ptr [entryRequest],eax
    popad
    popfd
    mov eax,dword ptr [esi]
    mov ecx,esi
    mov eax,dword ptr [eax+4]
    call eax
    push eax
    pushfd
    pushad
    mov eax,dword ptr [esp+36]
    push eax
    push dword ptr [entryCheckFn]
    push dword ptr [entryVtable]
    push dword ptr [entryPtr]
    call RecordEntryCheck
    add esp,16
    popad
    popfd
    pop eax
    ret
entry_pass:
    popad
    popfd
    mov eax,dword ptr [esi]
    mov ecx,esi
    mov eax,dword ptr [eax+4]
    jmp eax
}}
// File-entry-check observation: wrap the 5-byte vtable load at 0x46D51E,
// inside the request method 0x46D4D0. The check target is [entry]+4; the
// real result is request+4, written at 0x46D52C only when AL==0.
struct FileCheckRecord {
    volatile LONG written;
    unsigned int seq, tid, request, entry, vtable, checkFn, parent, key;
    unsigned int al, req4Before, req4After;
    unsigned int before[4], after[4];
    int readableBefore, readableAfter;
};
static FileCheckRecord fileCheckRecords[8];
static volatile LONG fileCheckCount, fileCheckOverflow;
static unsigned int fileRequest, fileEntry, fileVtable, fileCheckFn, fileParent, fileKey;
static unsigned int fileReq4Before, fileReq4After, fileCheckAL;
static int fileReadableBefore, fileReadableAfter;
static unsigned int fileBefore[4], fileAfter[4];
static void* fileResume;
static int ReadRequestWords(unsigned int request,unsigned int* words) {
    words[0]=words[1]=words[2]=words[3]=0;
    __try { for(int i=0;i<4;++i) words[i]=*(unsigned int*)(request+i*4); return 1; }
    __except(EXCEPTION_EXECUTE_HANDLER) { words[0]=0xFFFFFFFFu; return 0; }
}
static unsigned int ReadReq4(unsigned int request) {
    unsigned int value=0xFFFFFFFFu;
    __try { value=*(unsigned char*)(request+4); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return value;
}
extern "C" void RecordFileCheck(unsigned int al) {
    if(!Calling())return;
    LONG index=InterlockedIncrement(&fileCheckCount)-1;
    if(index>=8){fileCheckOverflow=1;return;}
    FileCheckRecord& r=fileCheckRecords[index];
    r.written=1; r.seq=action.last; r.tid=GetCurrentThreadId();
    r.request=fileRequest; r.entry=fileEntry; r.vtable=fileVtable; r.checkFn=fileCheckFn;
    r.parent=fileParent; r.key=fileKey; r.al=al&0xFF;
    r.req4Before=fileReq4Before; r.req4After=fileReq4After;
    r.readableBefore=fileReadableBefore; r.readableAfter=fileReadableAfter;
    memcpy(r.before,fileBefore,sizeof(r.before)); memcpy(r.after,fileAfter,sizeof(r.after));
}
static void ResetFileCheck(){
    memset(fileCheckRecords,0,sizeof(fileCheckRecords));
    fileCheckCount=0; fileCheckOverflow=0;
    fileRequest=0; fileEntry=0; fileVtable=0; fileCheckFn=0; fileParent=0; fileKey=0;
    fileReq4Before=0; fileReq4After=0; fileCheckAL=0;
    fileReadableBefore=0; fileReadableAfter=0;
    memset(fileBefore,0,sizeof(fileBefore)); memset(fileAfter,0,sizeof(fileAfter));
}
static void PublishFileCheck(){
    LONG count=fileCheckCount; if(count>8)count=8;
    for(int i=0;i<count;++i) {
        FileCheckRecord& r=fileCheckRecords[i]; if(!r.written)continue;
        Log("ENTRY_CHECK2 seq=%lu idx=%d request=%08X entry=%08X vtable=%08X check=%08X al=%u req4_before=%u req4_after_check=%u parent=%08X key=%08X before=%08X,%08X,%08X,%08X after=%08X,%08X,%08X,%08X readable=%d/%d\n",
            action.last,i,r.request,r.entry,r.vtable,r.checkFn,r.al,r.req4Before,r.req4After,
            r.parent,r.key,r.before[0],r.before[1],r.before[2],r.before[3],
            r.after[0],r.after[1],r.after[2],r.after[3],r.readableBefore,r.readableAfter);
    }
    if(fileCheckOverflow)Log("ENTRY_CHECK2_OVERFLOW seq=%lu\n",action.last);
    if(count==0)Log("ENTRY_CHECK2_MISSING seq=%lu\n",action.last);
}
extern "C" __declspec(naked) void ThunkFileCheck(){__asm{
    pushfd
    pushad
    call GateCalling
    test al,al
    jz file_pass
    mov eax,dword ptr [ebp-4]
    mov dword ptr [fileRequest],eax
    mov eax,dword ptr [ebp+8]
    mov dword ptr [fileParent],eax
    mov eax,dword ptr [ebp+0Ch]
    mov dword ptr [fileKey],eax
    mov ecx,dword ptr [esp+24]
    mov dword ptr [fileEntry],ecx
    mov eax,dword ptr [ecx]
    mov dword ptr [fileVtable],eax
    mov eax,dword ptr [eax+4]
    mov dword ptr [fileCheckFn],eax
    mov eax,dword ptr [fileRequest]
    push eax
    call ReadReq4
    add esp,4
    mov dword ptr [fileReq4Before],eax
    push offset fileBefore
    mov eax,dword ptr [fileRequest]
    push eax
    call ReadRequestWords
    add esp,8
    mov dword ptr [fileReadableBefore],eax
    popad
    popfd
    mov eax,dword ptr [ecx]
    mov eax,dword ptr [eax+4]
    call eax
    push eax
    pushfd
    pushad
    mov eax,dword ptr [esp+36]
    mov dword ptr [fileCheckAL],eax
    mov eax,dword ptr [fileRequest]
    push eax
    call ReadReq4
    add esp,4
    mov dword ptr [fileReq4After],eax
    push offset fileAfter
    mov eax,dword ptr [fileRequest]
    push eax
    call ReadRequestWords
    add esp,8
    mov dword ptr [fileReadableAfter],eax
    push dword ptr [fileCheckAL]
    call RecordFileCheck
    add esp,4
    popad
    popfd
    pop eax
    jmp dword ptr [fileResume]
file_pass:
    popad
    popfd
    mov eax,dword ptr [ecx]
    mov eax,dword ptr [eax+4]
    call eax
    jmp dword ptr [fileResume]
}}
// File-entry branch observation: 0x46D4D0 entry and its cmp/key/state branches.
struct FileBranchRecord {
    volatile LONG written;
    unsigned int seq, kind, request, arg1, arg2, node, sentinel, hit, al, entry, state, tid;
};
static FileBranchRecord fbRecords[16];
static volatile LONG fbCount, fbOverflow;
static unsigned int fbKind, fbRequest, fbArg1, fbArg2, fbNode, fbSentinel, fbHit, fbAl, fbEntry, fbState;
static void *feResume, *fcmpResumeHit, *fcmpResumeMiss, *fkeyResume, *fstateResume;
struct KeySnapshot {
    unsigned int object, data, len, cap, copied;
    int sso, heap, readable, truncated;
    unsigned char bytes[64];
};
struct KeyCompareRecord {
    unsigned int seq, tid, frame, request, parent, query, node, al;
    int complete;
    KeySnapshot queryValue, nodeValue;
};
static KeyCompareRecord keyCompareRecords[16];
static LONG keyCompareCount, keyCompareOverflow, keyCompareUnmatched;
static void ReadKeySnapshot(unsigned int object,KeySnapshot* value) {
    memset(value,0,sizeof(*value)); value->object=object;
    __try {
        value->len=*(unsigned int*)(object+0x10);
        value->cap=*(unsigned int*)(object+0x14);
        value->sso=value->cap<16; value->heap=!value->sso;
        if(value->len>value->cap)return;
        value->data=value->sso?object:*(unsigned int*)object;
        unsigned int count=value->len<64?value->len:64;
        value->truncated=value->len>64;
        if(count)memcpy(value->bytes,(void*)value->data,count);
        value->copied=count; value->readable=1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        value->copied=0; value->readable=0;
        memset(value->bytes,0,sizeof(value->bytes));
    }
}
static bool ReadKeyFrame(unsigned int frame,unsigned int* request,unsigned int* parent,unsigned int* query) {
    __try {
        *request=*(unsigned int*)(frame-4);
        *parent=*(unsigned int*)(frame+8);
        *query=*(unsigned int*)(frame+0x0C);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
extern "C" void BeginKeyCompare(unsigned int frame,unsigned int node,unsigned int sentinel) {
    if(!Calling() || node==sentinel)return;
    unsigned int request=0,parent=0,query=0;
    if(!ReadKeyFrame(frame,&request,&parent,&query)){++keyCompareUnmatched;return;}
    LONG index=keyCompareCount++;
    if(index>=16){keyCompareOverflow=1;return;}
    KeyCompareRecord& record=keyCompareRecords[index];
    memset(&record,0,sizeof(record));
    record.seq=action.last; record.tid=GetCurrentThreadId();
    record.frame=frame; record.request=request; record.parent=parent;
    record.query=query; record.node=node;
    ReadKeySnapshot(query,&record.queryValue);
    ReadKeySnapshot(node+0x10,&record.nodeValue);
}
extern "C" void EndKeyCompare(unsigned int frame,unsigned int node,unsigned int al) {
    if(!Calling())return;
    unsigned int request=0,parent=0,query=0;
    if(!ReadKeyFrame(frame,&request,&parent,&query)){++keyCompareUnmatched;return;}
    LONG count=keyCompareCount<16?keyCompareCount:16;
    for(LONG index=count-1;index>=0;--index) {
        KeyCompareRecord& record=keyCompareRecords[index];
        if(!record.complete && record.seq==action.last && record.tid==GetCurrentThreadId() &&
           record.frame==frame && record.request==request && record.parent==parent &&
           record.query==query && record.node==node) {
            record.al=al&0xFF; record.complete=1;return;
        }
    }
    ++keyCompareUnmatched;
}
static void ResetKeyCompare() {
    memset(keyCompareRecords,0,sizeof(keyCompareRecords));
    keyCompareCount=0; keyCompareOverflow=0; keyCompareUnmatched=0;
}
static void PublishKeyString(int index,const char* side,const KeySnapshot& value) {
    char hex[129]={0};
    for(unsigned int offset=0;offset<value.copied;++offset)sprintf_s(hex+offset*2,sizeof(hex)-offset*2,"%02X",value.bytes[offset]);
    Log("KEY_STRING seq=%lu idx=%d side=%s object=%08X data=%08X len=%u cap=%u sso=%d heap=%d readable=%d copied=%u truncated=%d hex=%s text=%s\n",
        action.last,index,side,value.object,value.data,value.len,value.cap,value.sso,value.heap,value.readable,value.copied,value.truncated,
        hex,EscapeText((const char*)value.bytes,value.copied).c_str());
}
static void PublishKeyCompare() {
    LONG count=keyCompareCount<16?keyCompareCount:16;
    for(int index=0;index<count;++index) {
        const KeyCompareRecord& record=keyCompareRecords[index];
        Log("KEY_COMPARE session=%s seq=%u idx=%d tid=%u frame=%08X request=%08X parent=%08X node=%08X query=%08X complete=%d al=%u\n",
            session.c_str(),record.seq,index,record.tid,record.frame,record.request,record.parent,record.node,record.query,record.complete,record.al);
        PublishKeyString(index,"query",record.queryValue);
        PublishKeyString(index,"node",record.nodeValue);
    }
    if(!count)Log("KEY_COMPARE_MISSING seq=%lu\n",action.last);
    if(keyCompareOverflow)Log("KEY_COMPARE_OVERFLOW seq=%lu count=%ld\n",action.last,keyCompareCount);
    if(keyCompareUnmatched)Log("KEY_COMPARE_UNMATCHED seq=%lu count=%ld\n",action.last,keyCompareUnmatched);
}
extern "C" void RecordFileBranch() {
    if(!Calling())return;
    LONG index=InterlockedIncrement(&fbCount)-1;
    if(index>=16){fbOverflow=1;return;}
    FileBranchRecord& r=fbRecords[index];
    r.written=1; r.seq=action.last; r.kind=fbKind;
    r.request=fbRequest; r.arg1=fbArg1; r.arg2=fbArg2;
    r.node=fbNode; r.sentinel=fbSentinel; r.hit=fbHit; r.al=fbAl;
    r.entry=fbEntry; r.state=fbState; r.tid=GetCurrentThreadId();
}
static void ResetFileBranch(){
    memset(fbRecords,0,sizeof(fbRecords));
    fbCount=0; fbOverflow=0;
    fbKind=0; fbRequest=0; fbArg1=0; fbArg2=0; fbNode=0; fbSentinel=0;
    fbHit=0; fbAl=0; fbEntry=0; fbState=0;
}
static void PublishFileBranch(){
    LONG count=fbCount; if(count>16)count=16;
    for(int i=0;i<count;++i) {
        FileBranchRecord& r=fbRecords[i]; if(!r.written)continue;
        Log("FILE_BRANCH seq=%lu idx=%d kind=%u request=%08X arg1=%08X arg2=%08X node=%08X sentinel=%08X hit=%u al=%u entry=%08X state=%u tid=%lu\n",
            action.last,i,r.kind,r.request,r.arg1,r.arg2,r.node,r.sentinel,r.hit,r.al,r.entry,r.state,r.tid);
    }
    if(fbOverflow)Log("FILE_BRANCH_OVERFLOW seq=%lu\n",action.last);
    if(count==0)Log("FILE_BRANCH_MISSING seq=%lu\n",action.last);
}
extern "C" __declspec(naked) void ThunkFileEntry(){__asm{
    push ebp
    mov ebp,esp
    push ecx
    push ebx
    pushfd
    pushad
    call GateCalling
    test al,al
    jz fe_pass
    mov eax,dword ptr [ebp-4]
    mov dword ptr [fbRequest],eax
    mov eax,dword ptr [ebp+8]
    mov dword ptr [fbArg1],eax
    mov eax,dword ptr [ebp+0Ch]
    mov dword ptr [fbArg2],eax
    mov dword ptr [fbKind],0
    call RecordFileBranch
fe_pass:
    popad
    popfd
    jmp dword ptr [feResume]
}}
extern "C" __declspec(naked) void ThunkFileCmp(){__asm{
    pushad
    cmp esi,dword ptr [ebx+28h]
    setz al
    movzx eax,al
    mov dword ptr [fbHit],eax
    mov dword ptr [fbNode],esi
    mov eax,dword ptr [ebx+28h]
    mov dword ptr [fbSentinel],eax
    mov dword ptr [fbKind],1
    call RecordFileBranch
    push dword ptr [ebx+28h]
    push esi
    push ebp
    call BeginKeyCompare
    add esp,12
    popad
    cmp esi,dword ptr [ebx+28h]
    jz fcmp_hit
    jmp dword ptr [fcmpResumeMiss]
fcmp_hit:
    jmp dword ptr [fcmpResumeHit]
}}
extern "C" __declspec(naked) void ThunkFileKey(){__asm{
    pushad
    movzx eax,al
    mov dword ptr [fbAl],eax
    mov dword ptr [fbKind],2
    call RecordFileBranch
    mov eax,dword ptr [esp+28]
    push eax
    push esi
    push ebp
    call EndKeyCompare
    add esp,12
    popad
    test al,al
    jz fkey_eq
    mov esi,dword ptr [ebx+28h]
    jmp dword ptr [fkeyResume]
fkey_eq:
    jmp dword ptr [fkeyResume]
}}
extern "C" __declspec(naked) void ThunkFileState(){__asm{
    mov ecx,dword ptr [esi+28h]
    mov eax,dword ptr [ecx+20h]
    mov dword ptr [fbNode],esi
    mov dword ptr [fbEntry],ecx
    mov dword ptr [fbState],eax
    mov dword ptr [fbKind],3
    pushad
    call RecordFileBranch
    popad
    jmp dword ptr [fstateResume]
}}
static bool PatchFileBranches(unsigned char* method) {
    feResume=method+5;
    fcmpResumeHit=method+0x31;
    fcmpResumeMiss=method+0x1E;
    fkeyResume=method+0x31;
    fstateResume=method+0x3C;
    return PatchJump(method,(void*)ThunkFileEntry,5) &&
        PatchJump(method+0x19,(void*)ThunkFileCmp,5) &&
        PatchJump(method+0x2A,(void*)ThunkFileKey,7) &&
        PatchJump(method+0x36,(void*)ThunkFileState,6);
}
static bool PatchCallN(unsigned char* site,void* thunk,int length) {
    if(length<5)return false;
    DWORD old;
    if(!VirtualProtect(site,length,PAGE_EXECUTE_READWRITE,&old))return false;
    unsigned char patch[16]; patch[0]=0xE8;
    int rel=(int)((unsigned char*)thunk-(site+5));
    memcpy(patch+1,&rel,4);
    for(int i=5;i<length;++i)patch[i]=0x90;
    memcpy(site,patch,length);
    DWORD unused; VirtualProtect(site,length,old,&unused);
    FlushInstructionCache(GetCurrentProcess(),site,length);
    return true;
}
static bool PatchCall(unsigned char* site,void* thunk) {
    DWORD old;
    if(!VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&old))return false;
    unsigned char patch[5]; patch[0]=0xE8;
    int rel=(int)((unsigned char*)thunk-(site+5));
    memcpy(patch+1,&rel,4);
    memcpy(site,patch,5);
    DWORD unused; VirtualProtect(site,5,old,&unused);
    FlushInstructionCache(GetCurrentProcess(),site,5);
    return true;
}
// Only the game's own import slots are changed, never system DLL code or disk files.
typedef HANDLE (WINAPI* OpenA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI* OpenW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL (WINAPI* Reader)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef int (__cdecl* LuaFile)(void*,const char*);
static OpenA realOpenA; static OpenW realOpenW; static Reader realRead; static LuaFile realLuaFile;
typedef BOOL (WINAPI* WriteFn)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL (WINAPI* CloseFn)(HANDLE);
static WriteFn realWrite; static CloseFn realClose;
struct HRec { HANDLE h; bool t; };
static HRec handles[64];
static void TrackHandle(HANDLE h,bool t){for(int i=0;i<64;++i){if(!handles[i].h){handles[i].h=h;handles[i].t=t;return;}}}
static bool UntrackHandle(HANDLE h){for(int i=0;i<64;++i){if(handles[i].h==h){handles[i].h=0;return handles[i].t;}}return false;}
static bool Tracked(HANDLE h){for(int i=0;i<64;++i)if(handles[i].h==h)return true;return false;}
static void TraceOpen(HANDLE file,DWORD access) {
    if(!Calling() || file==INVALID_HANDLE_VALUE)return;
    bool rd=(access&GENERIC_READ)!=0;
    bool wr=(access&(GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA))!=0;
    if(!rd && !wr)return;
    wchar_t path[1024]; DWORD n=GetFinalPathNameByHandleW(file,path,1024,FILE_NAME_NORMALIZED);
    if(!n || n>=1024)return;
    bool sel=IsTarget(path);
    Log("OPEN seq=%lu tid=%lu handle=%p target=%d rd=%d wr=%d access=%08X path=%s\n",
        action.last,actionTid,file,sel,rd?1:0,wr?1:0,access,Utf8(path).c_str());
    if(wr)TrackHandle(file,sel);
}
static void TraceOpenFail(const char* path,DWORD access,DWORD error) {
    if(!Calling())return;
    bool rd=(access&GENERIC_READ)!=0;
    bool wr=(access&(GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA))!=0;
    if(!rd && !wr)return;
    std::string safe;
    if(path)for(const unsigned char* q=(const unsigned char*)path;*q;++q) {
        if(*q<128)safe+=(char)*q;else {char hex[5];sprintf_s(hex,"\\x%02X",*q);safe+=hex;}
    }
    Log("OPENFAIL seq=%lu tid=%lu rd=%d wr=%d access=%08X err=%lu path=%s\n",
        action.last,actionTid,rd?1:0,wr?1:0,access,error,safe.c_str());
}
static void TraceOpenFailW(const wchar_t* path,DWORD access,DWORD error) {
    if(!Calling())return;
    bool rd=(access&GENERIC_READ)!=0;
    bool wr=(access&(GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA))!=0;
    if(!rd && !wr)return;
    Log("OPENFAIL seq=%lu tid=%lu rd=%d wr=%d access=%08X err=%lu path=%s\n",
        action.last,actionTid,rd?1:0,wr?1:0,access,error,path?Utf8(path).c_str():"?");
}
static BOOL WINAPI WatchWrite(HANDLE f,LPCVOID buf,DWORD n,LPDWORD got,LPOVERLAPPED ov) {
    BOOL r=realWrite(f,buf,n,got,ov); DWORD e=GetLastError();
    if(Tracked(f)) {
        wchar_t wp[1024]; DWORD k=GetFinalPathNameByHandleW(f,wp,1024,FILE_NAME_NORMALIZED);
        Log("WRITE seq=%lu handle=%p req=%lu got=%lu ok=%d err=%lu path=%s\n",
            action.last,f,n,got?*got:0,r?1:0,e,(k&&k<1024)?Utf8(wp).c_str():"?");
    }
    SetLastError(e); return r;
}
static BOOL WINAPI WatchClose(HANDLE f) {
    bool t=UntrackHandle(f);
    BOOL r=realClose(f); DWORD e=GetLastError();
    if(t) Log("CLOSE seq=%lu handle=%p ok=%d err=%lu\n",action.last,f,r?1:0,e);
    SetLastError(e); return r;
}static HANDLE WINAPI WatchOpenA(LPCSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES x,DWORD d,DWORD f,HANDLE t) {
    HANDLE result=realOpenA(p,a,s,x,d,f,t); DWORD error=GetLastError();
    if(result==INVALID_HANDLE_VALUE)TraceOpenFail(p,a,error);else TraceOpen(result,a);
    SetLastError(error);return result;
}
static HANDLE WINAPI WatchOpenW(LPCWSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES x,DWORD d,DWORD f,HANDLE t) {
    HANDLE result=realOpenW(p,a,s,x,d,f,t); DWORD error=GetLastError();
    if(result==INVALID_HANDLE_VALUE)TraceOpenFailW(p,a,error);else TraceOpen(result,a);
    SetLastError(error);return result;
}
static BOOL WINAPI WatchRead(HANDLE f,LPVOID b,DWORD size,LPDWORD got,LPOVERLAPPED ov) {
    BOOL result=realRead(f,b,size,got,ov); DWORD error=GetLastError();
    if(Calling() && result && got && *got) {
        wchar_t path[1024]; DWORD n=GetFinalPathNameByHandleW(f,path,1024,FILE_NAME_NORMALIZED);
        if(n && n<1024) {bool selected=IsTarget(path);if(selected)InterlockedExchangeAdd(&targetBytes,(LONG)*got);
            Log("READ seq=%lu handle=%p bytes=%lu target=%d path=%s\n",action.last,f,*got,selected,Utf8(path).c_str());}
    }
    SetLastError(error);return result;
}
static int __cdecl WatchLuaFile(void* state,const char* path) {
    int result=realLuaFile(state,path);
    if(Calling()) {bool selected=path && strstr(path,action.name.c_str());
        if(selected && result==0)InterlockedIncrement(&targetLuaOK);
        // Escape ANSI bytes before writing the UTF-8 evidence log.
        std::string safe; if(path)for(const unsigned char* p=(const unsigned char*)path;*p;++p) {
            if(*p<128)safe+=(char)*p;else {char hex[5];sprintf_s(hex,"\\x%02X",*p);safe+=hex;}}
        Log("LUA_FILE seq=%lu target=%d result=%d path=%s\n",action.last,selected,result,safe.c_str());}
    return result;
}
static bool Import(const char* name,void* replacement,void** original) {
    unsigned char* base=(unsigned char*)GetModuleHandleW(NULL);
    auto nt=(IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto desc=(IMAGE_IMPORT_DESCRIPTOR*)(base+dir.VirtualAddress);
    for(;desc->Name;++desc) {
        if(!desc->OriginalFirstThunk)continue;
        auto names=(IMAGE_THUNK_DATA*)(base+desc->OriginalFirstThunk);
        auto slots=(IMAGE_THUNK_DATA*)(base+desc->FirstThunk);
        for(;names->u1.AddressOfData;++names,++slots) {
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            auto entry=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(strcmp((const char*)entry->Name,name))continue;
            DWORD old; if(!VirtualProtect(&slots->u1.Function,sizeof(void*),PAGE_READWRITE,&old))return false;
            *original=(void*)slots->u1.Function;
            InterlockedExchange((volatile LONG*)&slots->u1.Function,(LONG)replacement);
            DWORD unused;VirtualProtect(&slots->u1.Function,sizeof(void*),old,&unused);
            Log("IAT name=%s slot=%p original=%p replacement=%p\n",name,&slots->u1.Function,*original,replacement);return true;
        }
    }
    return false;
}
extern "C" __declspec(naked) void NativeSave() { __asm {
    pushad
    sub esp,28
    mov esi,offset argument
    mov edi,esp
    mov ecx,24
    rep movsb
    mov dword ptr [esp+24],0
    mov ecx,dword ptr [currentThis]
    call dword ptr [saveFn]
    popad
    ret
} }
extern "C" __declspec(naked) void NativeLoad() { __asm {
    pushad
    sub esp,24
    mov esi,offset argument
    mov edi,esp
    mov ecx,24
    rep movsb
    mov ecx,dword ptr [currentThis]
    call dword ptr [loadFn]
    movzx eax,al
    mov dword ptr [nativeAL],eax
    popad
    ret
} }
static bool sourceReady=false;
static std::wstring sourceFileW;
static std::string sourceFileHash;
static std::string sourceProfile;
static bool PrepareSourceText(const std::string& path,const std::string& content);
static bool IsTriggerKind(const std::string& kind){return kind=="save"||kind=="load";}
static bool IsPrepareKind(const std::string& kind){return kind=="prep";}
// Prepare-only evidence: write AL, readback size/hash and entry existence before/after.
struct PrepareEvidence {
    int writeAL; unsigned int readSize, contentSize; int existsBefore, existsAfter;
    std::string readHash, contentHash; bool ok;
    std::string userRoot, profilePrefix, path, key; int backend=-1;
};
static bool PrepareTransfer(const std::string& path,const std::string& content,PrepareEvidence& ev);
static void PublishPrepare(const PrepareEvidence& ev);
static void ResetEvidence(PrepareEvidence& ev){
    ev.writeAL=0;ev.readSize=0;ev.contentSize=0;ev.existsBefore=-1;ev.existsAfter=-1;
    ev.readHash.clear();ev.contentHash.clear();ev.ok=false;ev.backend=-1;
    ev.userRoot.clear();ev.profilePrefix.clear();ev.path.clear();ev.key.clear();
}
static void ResetCapture();
static void PublishCapture();
static std::wstring Utf8ToWide(const std::string& text){
    int n=MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),NULL,0);
    std::wstring out(n,L'\0');
    if(n)MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),&out[0],n);
    return out;
}
static bool ReadFileBytes(const std::wstring& path,std::string& out){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size; bool ok=GetFileSizeEx(f,&size)!=0 && size.QuadPart>=0 && size.QuadPart<=0x1000000;
    if(ok){
        out.resize((size_t)size.QuadPart); DWORD got=0;
        ok=out.empty() || (ReadFile(f,&out[0],(DWORD)out.size(),&got,NULL)!=0 && got==out.size());
    }
    CloseHandle(f); if(!ok)out.clear(); return ok;
}
static bool ReadSourceLicense(){
    sourceReady=false; sourceFileW.clear(); sourceFileHash.clear(); sourceProfile.clear();
#ifdef ITB_SL_P04_SERVICE
    auto p04parts=Split(Read(L"native_source.txt"));
    if(p04parts.size()!=6)return false;
    if(p04parts[1].size()!=64 || p04parts[3].size()!=64 || p04parts[5].size()!=64)return false;
    if(p04parts[3]!=exeHash)return false;
    std::wstring p04path=Utf8ToWide(p04parts[0]);
    std::wstring p04receipt=Utf8ToWide(p04parts[4]);
    if(!p04svc::IsInsideRoot(p04path) || !p04svc::IsInsideRoot(p04receipt))return false;
    if(Hash(p04path)!=p04parts[1] || Hash(p04receipt)!=p04parts[5])return false;
    sourceFileW=p04path; sourceFileHash=p04parts[1]; sourceProfile=p04parts[2]; sourceReady=true; return true;
#else
    auto parts=Split(Read(L"native_source.txt"));
    if(parts.size()!=6)return false;
    if(parts[1].size()!=64 || parts[3].size()!=64 || parts[5].size()!=64)return false;
    if(parts[3]!=exeHash)return false;
    std::wstring path=Utf8ToWide(parts[0]);
    const std::wstring prefix=L"C:\\ITB-SL-Research\\runtime";
    if(path.rfind(prefix,0)!=0)return false;
    if(Hash(path)!=parts[1])return false;
    std::wstring receipt=Utf8ToWide(parts[4]);
    if(receipt.rfind(prefix,0)!=0 || Hash(receipt)!=parts[5])return false;
    sourceFileW=path; sourceFileHash=parts[1]; sourceProfile=parts[2]; sourceReady=true; return true;
#endif
}
static bool Fields(unsigned int ecx,unsigned int* child,unsigned int* state) {
    __try { *child=*(unsigned int*)(ecx+16);*state=*(unsigned int*)(ecx+8);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static void OnA(unsigned int ecx) {
    if(InterlockedExchange(&inA,1)) {InterlockedIncrement(&reentry);return;}
    LONG count=InterlockedIncrement(&visits);
#ifdef ITB_SL_P04_SERVICE
    InterlockedIncrement(&p04Tick);
#endif
    unsigned int child=0,state=0;
    if(!Fields(ecx,&child,&state)){InterlockedExchange(&inA,0);return;}
    if(count==1 || count%30==0)Log("OBSERVE visits=%ld tid=%lu this=%08X state=%u child=%08X phase=%ld reentry=%ld\n",count,GetCurrentThreadId(),ecx,state,child,action.phase,reentry);
    if(state==1 && child && action.phase==slnative::Queued) {
        // At 609104 the game's own update/command work is done. After this callback
        // 609109..609123 only restores the exception frame/registers/cookie and returns.
        if(action.Begin()) {
            currentThis=ecx; actionTid=GetCurrentThreadId(); nativeAL=-1;targetBytes=0;targetLuaOK=0;
            ResetGates();
            ResetFind();
            ResetEntryCheck();
            ResetFileCheck();
            ResetFileBranch();
            ResetKeyCompare();
            ResetCapture();
#ifdef ITB_SL_P04_SERVICE
            if(!P04BeforeCall()) {
                Log("P04_REFUSED seq=%lu kind=%s name=%s native_called=0\n",action.last,action.request.kind.c_str(),action.name.c_str());
                action.Reject(-4);
                InterlockedExchange(&inA,0);
                return;
            }
#endif
            Log("BEGIN session=%s seq=%lu kind=%s pid=%lu tid=%lu this=%08X state=%u child=%08X fn=%p name=%s path=%s\n",session.c_str(),action.last,action.request.kind.c_str(),GetCurrentProcessId(),actionTid,ecx,state,child,action.request.kind=="save"?saveFn:(action.request.kind=="load"?loadFn:nullptr),action.name.c_str(),Utf8(target).c_str());
            if(GetTickCount64()-queuedAt>5000) {
                Log("CANCELLED seq=%lu queue deadline; no native call\n",action.last);action.Finish(-3);
            } else if(action.request.kind=="load" && !sourceReady && Hash(target)!=expectedHash) {
                Log("REFUSED seq=%lu target hash changed before call\n",action.last); action.Finish(-2);
            } else {
                if(action.request.kind=="save") {
#ifdef ITB_SL_P04_SERVICE
                    p04NativeCalled=1;
#endif
                    NativeSave();
                }
                else if(IsPrepareKind(action.request.kind)) {
                    // Prepare-only: write S under the transfer name and read it back; never call NativeLoad.
                    std::string text; PrepareEvidence ev;
                    ResetEvidence(ev);
                    if(!sourceReady || !ReadFileBytes(sourceFileW,text)) nativeAL=-2;
                    else { ev.ok=PrepareTransfer(action.name,text,ev); nativeAL=ev.ok?1:0; }
                    PublishPrepare(ev);
                }
                else {
                    bool allow=true;
                    if(sourceReady) {
                        // Source-load: same canonical write/query/readback as prep, and the
                        // evidence stays bound to this seq/name so a failed prepare cannot
                        // reach NativeLoad or be filled in from an earlier prep.
                        std::string text; PrepareEvidence ev;
                        ResetEvidence(ev);
                        bool read=ReadFileBytes(sourceFileW,text);
                        if(!read) ev.writeAL=-1;
                        bool ok=read && PrepareTransfer(action.name,text,ev);
#ifdef ITB_SL_P04_SERVICE
                        p04PrepareOk=ok;
#endif
                        PublishPrepare(ev);
                        Log("LOAD_PREPARE seq=%lu name=%s source=%s bytes=%u ok=%d write_al=%d read=%u/%u exists_before=%d exists_after=%d read_hash=%s content_hash=%s\n",
                            action.last,action.name.c_str(),Utf8(sourceFileW).c_str(),(unsigned)text.size(),ok?1:0,
                            ev.writeAL,ev.readSize,ev.contentSize,ev.existsBefore,ev.existsAfter,
                            ev.readHash.c_str(),ev.contentHash.c_str());
                        if(!ok){ nativeAL=-2; allow=false; }
                    }
                    if(allow) {
#ifdef ITB_SL_P04_SERVICE
                        p04NativeCalled=1;
#endif
                        NativeLoad();
                    }
#ifdef ITB_SL_P04_SERVICE
                    else {P04RejectNoCall(action.last,"load","prepare_failed");action.Reject(-2);InterlockedExchange(&inA,0);return;}
#endif
                }
                unsigned int after=*(unsigned int*)(ecx+16);
                Log("RETURN seq=%lu kind=%s al=%ld before_child=%08X after_child=%08X target_read_bytes=%ld target_lua_ok=%ld\n",action.last,action.request.kind.c_str(),nativeAL,child,after,targetBytes,targetLuaOK);
                if(action.request.kind=="save")PublishCapture();
                if(action.request.kind=="load")PublishGates();
                if(action.request.kind=="load")PublishFind();
                if(action.request.kind=="load")PublishEntryCheck();
                if(action.request.kind=="load")PublishFileCheck();
                if(action.request.kind=="load")PublishFileBranch();
                if(action.request.kind=="load")PublishKeyCompare();
                if(IsPrepareKind(action.request.kind)) { PublishFind(); PublishFileBranch(); }
                action.Finish(nativeAL);
            }
        }
    }
    InterlockedExchange(&inA,0);
}
extern "C" __declspec(naked) void TrampolineA() { __asm {
    // Replace the exact CALL at 609104. Preserve its original call and output,
    // then dispatch using the Game pointer captured before that no-argument call.
    push ecx
    call dword ptr [continueA]
    pushfd
    pushad
    mov ecx,dword ptr [esp+36]
    push ecx
    call OnA
    add esp,4
    popad
    popfd
    lea esp,[esp+4]
    ret
} }
// T03 adapters: capture at 0x60A1D8 and prepare_source through 0x49F7C0.
struct GStringValue { unsigned int raw[6]; };
static void *realWriteFn, *realMalloc, *realFree;
static void *realOpenFn, *realSizeFn, *realReadFn, *realCloseFn;
static void *realRootFn, *realProfileFn, *realBackendFn, *realRelativeFn;
static unsigned int captureReturn;
static std::string capturePath, captureText;
static volatile LONG captureCount, captureAL;
static bool capturePublished=false;
static const unsigned char* ValueData(const GStringValue* v) {
    return v->raw[5]>=0x10 ? (const unsigned char*)v->raw[0] : (const unsigned char*)&v->raw[0];
}
static void CopyCapture(unsigned int pathPtr,unsigned int contentPtr) {
    if(action.request.kind!="save")return;
    const GStringValue* p=(const GStringValue*)pathPtr;
    const GStringValue* c=(const GStringValue*)contentPtr;
    unsigned int pl=p->raw[4], cl=c->raw[4];
    if(pl>0x1000 || cl>0x1000000)return;
    __try {
        capturePath.assign((const char*)ValueData(p),pl);
        captureText.assign((const char*)ValueData(c),cl);
        InterlockedIncrement(&captureCount);
        Log("CAPTURE_COPY seq=%lu count=%ld bytes=%u path=%s\n",action.last,(long)captureCount,cl,capturePath.c_str());
    } __except(EXCEPTION_EXECUTE_HANDLER) { capturePath.clear(); captureText.clear(); }
}
extern "C" void RecordCaptureAL(unsigned int al){captureAL=al&0xFF;}
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
extern "C" void SaveObsWriterPre(unsigned int pathPtr,unsigned int contentPtr,unsigned int frame);
extern "C" void SaveObsWriterPost(unsigned int al);
#endif
extern "C" __declspec(naked) void ThunkCaptureWrite(){__asm{
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
    // Keep the caller's return address on its own stack. Both by-value strings are
    // copied to a temporary argument area; the original cdecl caller still drops 48 bytes.
    pushfd
    pushad
    mov ebx,esp
    sub esp,528
    and esp,0FFFFFFF0h
    fxsave [esp]
    call GateCalling
    test al,al
    jz sobs_capture_pass
    lea eax,[ebx+40]
    lea ecx,[ebx+64]
    push ecx
    push eax
    call CopyCapture
    add esp,8
sobs_capture_pass:
    push dword ptr [ebx+8]
    lea ecx,[ebx+64]
    push ecx
    lea eax,[ebx+40]
    push eax
    call SaveObsWriterPre
    add esp,12
    fxrstor [esp]
    mov esp,ebx
    popad
    popfd
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    push dword ptr [esp+48]
    call dword ptr [realWriteFn]
    lea esp,[esp+48]
    pushfd
    pushad
    mov ebx,esp
    sub esp,528
    and esp,0FFFFFFF0h
    fxsave [esp]
    push dword ptr [ebx+28]
    call SaveObsWriterPost
    add esp,4
    push dword ptr [ebx+28]
    call RecordCaptureAL
    add esp,4
    fxrstor [esp]
    mov esp,ebx
    popad
    popfd
    ret
#else
    mov eax,dword ptr [esp]
    mov dword ptr [captureReturn],eax
    pushfd
    pushad
    call GateCalling
    test al,al
    jz capture_pass
    lea eax,[esp+40]
    lea ecx,[esp+64]
    push ecx
    push eax
    call CopyCapture
    add esp,8
capture_pass:
    popad
    popfd
    add esp,4
    call dword ptr [realWriteFn]
    pushfd
    pushad
    mov eax,dword ptr [esp+28]
    push eax
    call RecordCaptureAL
    add esp,4
    popad
    popfd
    push dword ptr [captureReturn]
    ret
#endif
}}
static void SetValue(GStringValue* v,const std::string& text){
    memset(v,0,sizeof(*v)); v->raw[4]=(unsigned int)text.size();
    if(text.size()<0x10){ v->raw[5]=0x0F; memcpy(&v->raw[0],text.data(),text.size()); }
    else {
        size_t bytes=text.size()+1;
        unsigned char* allocation=(unsigned char*)((void*(__cdecl*)(size_t))realMalloc)(bytes+(bytes>=0x1000?0x23:0));
        unsigned char* p=allocation;
        if(bytes>=0x1000){
            p=(unsigned char*)(((size_t)allocation+0x23)&~(size_t)0x1F);
            ((unsigned char**)p)[-1]=allocation;
        }
        memcpy(p,text.data(),text.size()); p[text.size()]=0;
        v->raw[0]=(unsigned int)p; v->raw[5]=(unsigned int)text.size();
    }
}
static void FreeValue(GStringValue* v){
    if(v->raw[5]>=0x10 && v->raw[0]){
        void* allocation=(void*)v->raw[0];
        if(v->raw[5]+1>=0x1000)allocation=((void**)allocation)[-1];
        ((void(__cdecl*)(void*))realFree)(allocation);
    }
    memset(v,0,sizeof(*v));
}
static std::string HashBuffer(const unsigned char* data,unsigned int len){
    BCRYPT_ALG_HANDLE alg=NULL; BCRYPT_HASH_HANDLE hash=NULL; unsigned char digest[32];
    std::string out;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)==0 && BCryptCreateHash(alg,&hash,NULL,0,NULL,0,0)==0 &&
       BCryptHashData(hash,(PUCHAR)data,len,0)==0 && BCryptFinishHash(hash,digest,32,0)==0){
        char hex[65]; for(int i=0;i<32;++i)sprintf_s(hex+i*2,65-i*2,"%02x",digest[i]); out=hex;
    }
    if(hash)BCryptDestroyHash(hash); if(alg)BCryptCloseAlgorithmProvider(alg,0); return out;
}
static volatile LONG existsAL=0;
static int ExistsValue(const std::string& name){
    GStringValue v; SetValue(&v,name);
    __asm {
        lea ecx,v
        xor edx,edx
        call dword ptr [origExistsFn]
        movzx eax,al
        mov dword ptr [existsAL],eax
    }
    int result=(int)existsAL;
    FreeValue(&v);
    return result;
}
// Same profile-prefix getter used at NativeLoad 0x60993A; no hard-coded U spelling.
static std::string EnginePrefix(bool profile){
    GStringValue value={}; void* fn=profile?realProfileFn:realRootFn;
    __asm {
        lea ecx,value
        xor edx,edx
        call fn
    }
    std::string text((const char*)ValueData(&value),value.raw[4]);
    FreeValue(&value); return text;
}
static int EngineBackend(const std::string& path){
    GStringValue value; SetValue(&value,path);
    // 0x49E3F0 consumes its by-value parameter, and defines AL only.
    return ((unsigned char(__cdecl*)(GStringValue))realBackendFn)(value);
}
static std::string EngineRelative(const std::string& path){
    GStringValue input, output={}; SetValue(&input,path);
    __asm {
        sub esp,24
        mov edx,esp
        lea eax,input
        mov ecx,6
relative_copy:
        push dword ptr [eax]
        pop dword ptr [edx]
        add eax,4
        add edx,4
        loop relative_copy
        lea ecx,output
        call dword ptr [realRelativeFn]
        add esp,24
    }
    // 0x49E4B0 consumes input; returned output belongs to this adapter.
    std::string text((const char*)ValueData(&output),output.raw[4]);
    FreeValue(&output); return text;
}
static bool ResolvePreparePath(const std::string& name,PrepareEvidence& ev){
    ev.backend=-1;ev.userRoot.clear();ev.profilePrefix.clear();ev.path.clear();ev.key.clear();
    if(!(slnative::ValidName(name) || slnative::ValidNameP04(name)) || sourceProfile.empty() || sourceProfile.find_first_of("\\/:|\r\n")!=std::string::npos ||
       sourceProfile=="." || sourceProfile=="..")return false;
    ev.userRoot=EnginePrefix(false); ev.profilePrefix=EnginePrefix(true);
    // NativeLoad appends name to this exact prefix. Require the licensed profile.
    if(ev.userRoot.empty() ||
#ifndef ITB_SL_P04_SERVICE
       _stricmp(ev.userRoot.c_str(),"C:\\ITB-SL-Research\\userdata\\Into the Breach\\")!=0 ||
#endif
       ev.profilePrefix!=ev.userRoot+"profile_"+sourceProfile+"\\")return false;
    ev.path=ev.profilePrefix+name;
    ev.backend=EngineBackend(ev.path);
    if(ev.backend!=1)return false;
    ev.key=EngineRelative(ev.path);
    bool ok=ev.key=="profile_"+sourceProfile+"\\"+name;
    Log("PREP_PATH session=%s seq=%lu tid=%lu name=%s root=%s prefix=%s path=%s backend=%d key=%s ok=%d\n",
        session.c_str(),action.last,GetCurrentThreadId(),name.c_str(),ev.userRoot.c_str(),ev.profilePrefix.c_str(),
        ev.path.c_str(),ev.backend,ev.key.c_str(),ok?1:0);
    return ok;
}
static bool ReadEngineWritten(const std::string& path,PrepareEvidence& ev){
    GStringValue value; SetValue(&value,path);
    int handle=((int(__cdecl*)(GStringValue))realOpenFn)(value);
    Log("PREP_OPEN seq=%lu tid=%lu path=%s handle=%d\n",action.last,GetCurrentThreadId(),path.c_str(),handle);
    if(handle<0)return false;
    unsigned int readSize=0;
    __asm {
        mov ecx,handle
        call dword ptr [realSizeFn]
        mov readSize,eax
    }
    ev.readSize=readSize;
    void* buffer=0;
    if(readSize==ev.contentSize && readSize<=0x1000000){
        __asm {
            mov ecx,handle
            mov edx,readSize
            push 1
            call dword ptr [realReadFn]
            add esp,4
            mov buffer,eax
        }
    }
    __asm {
        mov ecx,handle
        call dword ptr [realCloseFn]
    }
    if(!buffer)return false;
    ev.readHash=HashBuffer((const unsigned char*)buffer,readSize);
    ((void(__cdecl*)(void*))realFree)(buffer);
    return !ev.contentHash.empty() && ev.readHash==ev.contentHash;
}
static bool WriteSourceText(const std::string& path,const std::string& content,PrepareEvidence& ev){
    ev.writeAL=0;ev.readSize=0;ev.contentSize=(unsigned int)content.size();
    ev.readHash.clear();ev.contentHash.clear();ev.ok=false;
    GStringValue pv,cv; SetValue(&pv,path); SetValue(&cv,content);
    unsigned char al=((unsigned char(__cdecl*)(GStringValue,GStringValue))realWriteFn)(pv,cv);
    ev.writeAL=al;
    Log("PREP_WRITE_RETURN seq=%lu tid=%lu path=%s al=%u\n",action.last,GetCurrentThreadId(),path.c_str(),(unsigned)al);
    ev.contentHash=HashBuffer((const unsigned char*)content.data(),(unsigned int)content.size());
    return al!=0;
}
static bool PrepareSourceText(const std::string& name,const std::string& content){
    PrepareEvidence ev; return PrepareTransfer(name,content,ev);
}
static bool PrepareTransfer(const std::string& name,const std::string& content,PrepareEvidence& ev){
    ev.writeAL=0;ev.readSize=0;ev.contentSize=(unsigned int)content.size();
    ev.readHash.clear();ev.contentHash.clear();ev.ok=false;ev.existsBefore=ev.existsAfter=-1;
    if(!ResolvePreparePath(name,ev))return false;
    if(sourceReady && HashBuffer((const unsigned char*)content.data(),(unsigned int)content.size())!=sourceFileHash)return false;
    const std::string& path=ev.path;
    ev.existsBefore=ExistsValue(path);
    Log("PREP_EXISTS seq=%lu tid=%lu stage=before path=%s al=%d\n",action.last,GetCurrentThreadId(),path.c_str(),ev.existsBefore);
    if(ev.existsBefore!=0){ ev.ok=false; return false; }
    if(!WriteSourceText(path,content,ev))return false;
    ev.existsAfter=ExistsValue(path);
    Log("PREP_EXISTS seq=%lu tid=%lu stage=after path=%s al=%d\n",action.last,GetCurrentThreadId(),path.c_str(),ev.existsAfter);
    if(ev.existsAfter!=1)return false;
    bool ok=ReadEngineWritten(path,ev);
    ev.ok=ok;
    return ok;
}
static std::string FormatPrepareLine(const PrepareEvidence& ev){
    char line[420];
    sprintf_s(line,"%s|%010lu|%s|%s|%d|%u|%u|%d|%d|%s|%s|%d\n",
        session.c_str(),action.last,action.request.kind.c_str(),action.name.c_str(),ev.writeAL,ev.readSize,ev.contentSize,
        ev.existsBefore,ev.existsAfter,ev.readHash.c_str(),ev.contentHash.c_str(),ev.ok?1:0);
    return line;
}
static void PublishPrepare(const PrepareEvidence& ev){
    char seq[11];sprintf_s(seq,"%010lu",action.last);
    std::string pathLine=session+"|"+seq+"|"+action.request.kind+"|"+action.name+"|"+ev.userRoot+"|"+ev.profilePrefix+"|"+
        ev.path+"|"+ev.key+"|"+std::to_string(ev.backend)+"\n";
    slprobe::Publish(root+L"\\native_prepare_path.txt",pathLine);
    std::string line=FormatPrepareLine(ev);
    slprobe::Publish(root+L"\\native_prepare_result.txt",line);
    Log("PREPARE_EVIDENCE seq=%lu kind=%s name=%s path=%s write_al=%d read_size=%u content_size=%u exists_before=%d exists_after=%d read_hash=%s content_hash=%s ok=%d\n",
        action.last,action.request.kind.c_str(),action.name.c_str(),ev.path.c_str(),ev.writeAL,ev.readSize,ev.contentSize,ev.existsBefore,ev.existsAfter,ev.readHash.c_str(),ev.contentHash.c_str(),ev.ok?1:0);
}
static bool WriteFileBytes(const std::wstring& path,const std::string& data){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD wrote=0; bool ok=data.empty() || (WriteFile(f,data.data(),(DWORD)data.size(),&wrote,NULL)!=0 && wrote==data.size());
    CloseHandle(f); return ok;
}
static void ResetCapture(){
    captureCount=0; captureAL=-1; capturePath.clear(); captureText.clear(); capturePublished=false;
}
// A save's completion evidence is this action's single, successful write of its own
// transfer name. Anything else is rejected and stale files are removed so the
// controller cannot publish an old or partial capture.
static bool CaptureComplete(){
    if(captureCount!=1 || (captureAL&0xFF)==0)return false;
    if(captureText.empty() || capturePath.empty())return false;
    size_t slash=capturePath.find_last_of("\\/");
    std::string base=slash==std::string::npos?capturePath:capturePath.substr(slash+1);
    return _stricmp(base.c_str(),action.name.c_str())==0;
}
static void PublishCapture(){
    std::wstring base=root+L"\\native_capture";
    if(!CaptureComplete()){
        BOOL removed=DeleteFileW((base+L".lua").c_str());
        removed=DeleteFileW((base+L"_meta.txt").c_str())||removed;
        capturePublished=false;
        Log("CAPTURE_INVALID seq=%lu count=%ld al=%ld bytes=%u path=%s removed=%d\n",
            action.last,(long)captureCount,(long)captureAL,(unsigned)captureText.size(),capturePath.c_str(),removed?1:0);
        return;
    }
    bool ok=WriteFileBytes(base+L".lua",captureText);
    char meta[300]; sprintf_s(meta,"%s|%010lu|%ld|%ld\n",capturePath.c_str(),action.last,(long)captureAL,(long)captureCount);
    ok=ok && WriteFileBytes(base+L"_meta.txt",meta);
    if(!ok){
        // Invalidate metadata first: old text may still be readable but locked against writes/deletion.
        DeleteFileW((base+L"_meta.txt").c_str());
        DeleteFileW((base+L".lua").c_str());
    }
    capturePublished=ok;
    Log("CAPTURE seq=%lu path=%s bytes=%u write_al=%ld count=%ld ok=%d\n",action.last,capturePath.c_str(),(unsigned)captureText.size(),(long)captureAL,(long)captureCount,ok?1:0);
}
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
// ---- A1.2 save-path observation sites (record-only; publishing arrives in A2) ----
// Every wrapper calls the original target exactly once and hands its return value back
// untouched. The recorders only read memory; they never call into Lua or the game.
static void *sobsSetterFn, *sobsBuilderFn;
static unsigned char *sobsJoinResume;
static volatile LONG sobsCapable;
static volatile LONG sobsSetterCalls, sobsBuilderPre, sobsBuilderPost, sobsMapCalls;
static volatile LONG sobsWriterPre, sobsWriterPost;
static unsigned int sobsSetterTile, sobsSetterBoard, sobsSetterX, sobsSetterY;
static unsigned int sobsSetterArg, sobsSetterPreValue, sobsSetterPostValue, sobsSetterRet;
static unsigned int sobsBuilderEbp, sobsBuilderThis, sobsBuilderOut, sobsBuilderFlag;
static unsigned int sobsBuilderPayloadPtr, sobsBuilderPayloadLen, sobsBuilderFilenameLen;
static unsigned int sobsMapFrame, sobsMapTile, sobsMapBoard, sobsMapRowSet;
static unsigned int sobsMapX, sobsMapY, sobsMapInclude, sobsMapTerrain;
static unsigned int sobsWriterPathPtr, sobsWriterContentPtr, sobsWriterAL;
static volatile LONG sobsSetterExtra;
static int sobsSetterMatch;

// ---- A2: bounded observation state, control/arm, synchronous publishing ----
// Batch completion: a fixed 32-window bound. A controlled deployment was observed to
// produce 12 saves, so 4 slots dropped evidence; 32 leaves headroom without inventing a
// configurable capacity framework, ring buffer or automatic re-arm.
static const int SOBS_MAX_WINDOWS = 32;
static const int SOBS_MAX_MAPS = 4;
static const int SOBS_MAX_CELLS = 64;
static const unsigned int SOBS_MAX_BYTES = 1024u * 1024u;
static const char SOBS_CONTRACT[] = "bounded32-v1";
static const int SOBS_MAX_EVENTS = 1100;
struct SobsCell { unsigned int x, y, terrain, include; int valid; };
struct SobsMap {
    unsigned int board, rowSet, nameLen;
    char name[64];
    int cells, complete, truncated;
    SobsCell cell[SOBS_MAX_CELLS];
};
struct SobsWindow {
    int used, active, nested;
    unsigned int saveId, ebp, thisPtr, outPtr, flag, filenameLen;
    DWORD tid;
    int maps, cells, truncated;
    SobsMap map[SOBS_MAX_MAPS];
    unsigned int payloadBytes;
    char payloadSha[65];
    int payloadOk;
    int writerSeen, writerMatches;
    unsigned int writerAL, writerBytes, writerPathLen;
    char writerPath[300], writerSha[65];
};
struct SobsArm {
    int armed;
    char session[25], obsNonce[33], loadNonce[33], repairNonce[33];
    unsigned int seq, x, y, from, to;
    unsigned long long ttlMs, armedAt;
    char battle[64], source[65], receipt[65], target[65], raw[65];
    int setterSeen;
};
static SobsArm sobsArm;
static SobsWindow sobsWindow[SOBS_MAX_WINDOWS];
static SobsWindow* sobsActive;
static __declspec(thread) SobsWindow* sobsWriterActive;
static int sobsDepth;
static unsigned int sobsSaveId;
static std::string sobsControlHash;
static std::string sobsSeenControlHash;
static int sobsArmAccepted;
static int sobsEvents;
static volatile LONG sobsExpired;
static int sobsLimited;
// A1: capacity and publishing have to be auditable. These counters are cheap, bounded and
// only describe this observation chain; there is no queue, lock framework or retry system.
static volatile LONG sobsWindowsUsed, sobsWindowsDropped;
static volatile LONG sobsEventsWritten, sobsEventsDropped;
static volatile LONG sobsPublishErrors, sobsPublishSeq;
static unsigned long long sobsPublishedTick;
static const char* sobsPublishReason = "none";
static int sobsDeficient;  // sticky: a later "published" may never look clean again
struct SobsArmCounters {
    int valid;
    long setter, builderPre, builderPost, mapCells, writerPre, writerPost, windowsUsed;
};
static SobsArmCounters sobsArmCounters;

static bool SoHex(const std::string& text, size_t length) {
    if (text.size() != length) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}
static std::string SoJson(const char* text, size_t length) {
    std::string out;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c >= 32 && c < 127) out += (char)c;
        else out += '?';
    }
    return out;
}
static bool SobsEvent(const char* line) {
    if (!root.size()) { InterlockedIncrement(&sobsEventsDropped); sobsDeficient = 1; return false; }
    if (sobsEvents >= SOBS_MAX_EVENTS) {
        InterlockedIncrement(&sobsEventsDropped); sobsDeficient = 1; return false;
    }
    FILE* file = NULL;
    bool ok = _wfopen_s(&file, (root + L"\\native_p03_saveobs_events.jsonl").c_str(), L"a") == 0
        && file != NULL;
    if (ok) ok = fprintf(file, "%s\n", line) > 0;
    if (file && fclose(file) != 0) ok = false;
    if (!ok) {
        InterlockedIncrement(&sobsEventsDropped);
        InterlockedIncrement(&sobsPublishErrors);
        sobsDeficient = 1;
        return false;
    }
    ++sobsEvents;
    InterlockedIncrement(&sobsEventsWritten);
    return true;
}
// `state` is what happened; the emitted state is downgraded to "partial" once any evidence
// was dropped, so a successful publish can never hide an earlier gap.
static std::string SobsStatus(const char* state, const char* reason) {
    if (root.empty()) return std::string();
    InterlockedIncrement(&sobsPublishSeq);
    sobsPublishedTick = GetTickCount64();
    sobsPublishReason = reason ? reason : "unknown";
    const char* emitted = (sobsDeficient && strcmp(state, "rejected") != 0) ? "partial" : state;
    char line[2400];
    sprintf_s(line, sizeof(line),
        "{\"schema\":\"p03-saveobs-v1\",\"state\":\"%s\",\"state_detail\":\"%s\","
        "\"session\":\"%s\",\"pid\":%lu,\"obs_nonce\":\"%s\",\"control_sha256\":\"%s\","
        "\"setter_calls\":%ld,\"builder_pre\":%ld,\"builder_post\":%ld,\"map_cells\":%ld,"
        "\"writer_pre\":%ld,\"writer_post\":%ld,\"expired\":%ld,\"deficient\":%d,"
        "\"observation_contract\":\"%s\","
        "\"limits\":{\"windows\":%d,\"maps_per_window\":%d,\"cells_per_map\":%d,"
        "\"payload_bytes\":%u,\"events\":%d},"
        "\"capacity\":{\"windows\":%d,\"windows_used\":%ld,\"windows_dropped\":%ld},"
        "\"events\":{\"written\":%ld,\"dropped\":%ld},"
        "\"publish\":{\"seq\":%ld,\"tick_ms\":%llu,\"reason\":\"%s\",\"error\":%ld},"
        "\"arm_counters\":{\"valid\":%d,\"setter_calls\":%ld,\"builder_pre\":%ld,"
        "\"builder_post\":%ld,\"map_cells\":%ld,\"writer_pre\":%ld,\"writer_post\":%ld,"
        "\"windows_used\":%ld}}",
        emitted, state, session.c_str(), (unsigned long)GetCurrentProcessId(),
        sobsArm.obsNonce, sobsControlHash.c_str(), (long)sobsSetterCalls, (long)sobsBuilderPre,
        (long)sobsBuilderPost, (long)sobsMapCalls, (long)sobsWriterPre, (long)sobsWriterPost,
        (long)sobsExpired, sobsDeficient, SOBS_CONTRACT, SOBS_MAX_WINDOWS, SOBS_MAX_MAPS,
        SOBS_MAX_CELLS, SOBS_MAX_BYTES, SOBS_MAX_EVENTS, SOBS_MAX_WINDOWS,
        (long)sobsWindowsUsed, (long)sobsWindowsDropped, (long)sobsEventsWritten,
        (long)sobsEventsDropped, (long)sobsPublishSeq, (unsigned long long)sobsPublishedTick,
        sobsPublishReason, (long)sobsPublishErrors, sobsArmCounters.valid,
        sobsArmCounters.setter, sobsArmCounters.builderPre, sobsArmCounters.builderPost,
        sobsArmCounters.mapCells, sobsArmCounters.writerPre, sobsArmCounters.writerPost,
        sobsArmCounters.windowsUsed);
    if (!slprobe::Publish(root + L"\\native_p03_saveobs_status.txt", line)) {
        InterlockedIncrement(&sobsPublishErrors);
        sobsDeficient = 1;
    }
    return line;
}
static bool SobsExpired() {
    if (!sobsArm.armed) return false;
    if (GetTickCount64() - sobsArm.armedAt <= sobsArm.ttlMs) return false;
    sobsExpired = 1;
    return true;
}
static bool SobsArmRead() {
    std::string raw = Read(L"native_p03_saveobs_control.txt");
    if (raw.empty()) return false;
    std::string digest = HashBuffer((const unsigned char*)raw.data(), (unsigned int)raw.size());
    if (digest == sobsSeenControlHash) return false;
    sobsSeenControlHash = digest;
    if (sobsArmAccepted) {
        // One observation per process: never relabel retained windows or erase a gap.
        sobsArm.armed = 0;
        sobsDeficient = 1;
        SobsStatus("rejected", "rearm_rejected");
        return false;
    }
    sobsControlHash = digest;
    auto parts = Split(raw);
    bool ok = parts.size() == 19 && parts[0] == "P03SAVEOBS1"
        && parts[1] == session && SoHex(parts[3], 32) && SoHex(parts[4], 32)
        && parts[2] == std::to_string((unsigned long)GetCurrentProcessId())
        && parts[5].size() == 10 && parts[6].size() == 32 && parts[7].size() == 32
        && parts[9] == "Alpha" && SoHex(parts[10], 64) && SoHex(parts[11], 64)
        && SoHex(parts[12], 64) && SoHex(parts[13], 64)
        && atoi(parts[14].c_str()) >= 0 && atoi(parts[14].c_str()) <= 7
        && atoi(parts[15].c_str()) >= 0 && atoi(parts[15].c_str()) <= 7
        && atoi(parts[16].c_str()) >= 0 && atoi(parts[16].c_str()) <= 6
        && atoi(parts[17].c_str()) >= 0 && atoi(parts[17].c_str()) <= 6
        && atoi(parts[18].c_str()) > 0 && atoi(parts[18].c_str()) <= 600000;
    memset(&sobsArm, 0, sizeof(sobsArm));
    if (!ok) { SobsStatus("rejected", "control_rejected"); return false; }
    sobsArm.armed = 1;
    sobsArmAccepted = 1;
    strcpy_s(sobsArm.session, parts[1].c_str());
    strcpy_s(sobsArm.obsNonce, parts[4].c_str());
    strcpy_s(sobsArm.loadNonce, parts[6].c_str());
    strcpy_s(sobsArm.repairNonce, parts[7].c_str());
    strcpy_s(sobsArm.battle, parts[8].c_str());
    strcpy_s(sobsArm.source, parts[10].c_str());
    strcpy_s(sobsArm.receipt, parts[11].c_str());
    strcpy_s(sobsArm.target, parts[12].c_str());
    strcpy_s(sobsArm.raw, parts[13].c_str());
    sobsArm.seq = (unsigned int)strtoul(parts[5].c_str(), NULL, 10);
    sobsArm.x = (unsigned int)atoi(parts[14].c_str());
    sobsArm.y = (unsigned int)atoi(parts[15].c_str());
    sobsArm.from = (unsigned int)atoi(parts[16].c_str());
    sobsArm.to = (unsigned int)atoi(parts[17].c_str());
    sobsArm.ttlMs = (unsigned long long)atoi(parts[18].c_str());
    sobsArm.armedAt = GetTickCount64();
    sobsExpired = 0;
    sobsArm.setterSeen = 0;
    sobsEvents = 0;
    sobsWindowsUsed = 0;
    sobsWindowsDropped = 0;
    sobsEventsWritten = 0;
    sobsEventsDropped = 0;
    sobsPublishErrors = 0;
    sobsLimited = 0;
    sobsDeficient = 0;
    // Freeze the counters that were already running before this window, so a later status
    // cannot present pre-arm activity as this observation's own evidence.
    sobsArmCounters.valid = 1;
    sobsArmCounters.setter = (long)sobsSetterCalls;
    sobsArmCounters.builderPre = (long)sobsBuilderPre;
    sobsArmCounters.builderPost = (long)sobsBuilderPost;
    sobsArmCounters.mapCells = (long)sobsMapCalls;
    sobsArmCounters.writerPre = (long)sobsWriterPre;
    sobsArmCounters.writerPost = (long)sobsWriterPost;
    sobsArmCounters.windowsUsed = (long)sobsWindowsUsed;
    SobsStatus("armed", "arm");
    Log("SAVEOBS armed obs=%s seq=%u focus=%u,%u %u->%u ttl_ms=%llu\n",
        sobsArm.obsNonce, sobsArm.seq, sobsArm.x, sobsArm.y, sobsArm.from, sobsArm.to,
        sobsArm.ttlMs);
    return true;
}
static void SobsPoll() {
    if (!sobsCapable) return;
    if (SobsExpired()) { SobsStatus("expired", "expired"); return; }
    SobsArmRead();
}
static void SobsReady() {
    if (!sobsCapable) return;
    std::string dllHash;
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(module, modulePath, MAX_PATH)) dllHash = Hash(modulePath);
    char line[1200];
    sprintf_s(line, sizeof(line),
        "{\"schema\":\"p03-saveobs-v1\",\"session\":\"%s\",\"pid\":%lu,"
        "\"exe_sha256\":\"%s\",\"dll_sha256\":\"%s\",\"mode\":\"decision\","
        "\"capabilities\":{\"setter\":true,\"builder\":true,\"map_decision\":true,\"writer\":true},"
        "\"sites_verified\":4,\"observation_contract\":\"%s\","
        "\"limits\":{\"windows\":%d,\"maps_per_window\":%d,\"cells_per_map\":%d,"
        "\"payload_bytes\":%u,\"events\":%d}}",
        session.c_str(), (unsigned long)GetCurrentProcessId(), exeHash, dllHash.c_str(),
        SOBS_CONTRACT, SOBS_MAX_WINDOWS, SOBS_MAX_MAPS, SOBS_MAX_CELLS, SOBS_MAX_BYTES,
        SOBS_MAX_EVENTS);
    if (slprobe::Publish(root + L"\\native_p03_saveobs_ready.txt", line))
        Log("SAVEOBS ready mode=decision dll=%s\n", dllHash.c_str());
}


static bool SoBytes(const unsigned char* site, const unsigned char* value, int count) {
    return memcmp(site, value, count) == 0;
}
static bool SaveObsVerify(unsigned char* base) {
    const unsigned char setter[] = {0xE8, 0x43, 0xBC, 0x03, 0x00};
    const unsigned char builder[] = {0xE8, 0x59, 0x11, 0x00, 0x00};
    const unsigned char join[] = {0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char writer[] = {0xE8, 0xE3, 0x55, 0xE9, 0xFF};
    unsigned char* s = base + 0x165F58;
    unsigned char* b = base + 0x20A0D2;
    unsigned char* j = base + 0x1640E3;
    unsigned char* w = base + 0x20A1D8;
    if (!SoBytes(s, setter, 5) || !SoBytes(b, builder, 5) || !SoBytes(j, join, 7)
        || !SoBytes(w, writer, 5)) return false;
    int relSetter = *(const int*)(s + 1);
    int relBuilder = *(const int*)(b + 1);
    int relWriter = *(const int*)(w + 1);
    if ((unsigned char*)(s + 5 + relSetter) != base + 0x1A1BA0) return false;
    if ((unsigned char*)(b + 5 + relBuilder) != base + 0x20B230) return false;
    if ((unsigned char*)(w + 5 + relWriter) != base + 0x09F7C0) return false;
    return true;
}
static unsigned int SoReadU32(unsigned int address, bool* ok) {
    unsigned int value = 0;
    __try { value = *(const unsigned int*)address; *ok = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; }
    return value;
}
static unsigned char SoReadU8(unsigned int address, bool* ok) {
    unsigned char value = 0;
    __try { value = *(const unsigned char*)address; *ok = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; }
    return value;
}
// A separate helper keeps __try out of functions that hold C++ objects (C2712).
static int SoCopyBytes(unsigned int source, void* destination, unsigned int length) {
    __try { memcpy(destination, (const void*)source, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
extern "C" void SaveObsSetterPre(unsigned tile, unsigned board, unsigned x, unsigned y,
                                 unsigned terrain) {
    InterlockedIncrement(&sobsSetterCalls);
    sobsSetterTile = tile; sobsSetterBoard = board; sobsSetterX = x; sobsSetterY = y;
    sobsSetterArg = terrain;
    bool ok = false;
    sobsSetterPreValue = SoReadU32(tile + 0x2AE0, &ok);
    if (!ok) sobsSetterPreValue = 0xFFFFFFFFu;
    if (!sobsArm.armed || SobsExpired()) return;
    if (sobsArm.setterSeen) { InterlockedIncrement(&sobsSetterExtra); return; }
    if (GetCurrentThreadId() != actionTid) { InterlockedIncrement(&sobsSetterExtra); return; }
    if (x != sobsArm.x || y != sobsArm.y || (terrain & 0xFF) != (sobsArm.to & 0xFF)) {
        InterlockedIncrement(&sobsSetterExtra); return;
    }
    sobsSetterMatch = 1;
}
extern "C" void SaveObsSetterPost(unsigned ret) {
    bool ok = false;
    sobsSetterPostValue = SoReadU32(sobsSetterTile + 0x2AE0, &ok);
    if (!ok) sobsSetterPostValue = 0xFFFFFFFFu;
    sobsSetterRet = ret;
    if (!sobsSetterMatch) return;
    sobsSetterMatch = 0;
    sobsArm.setterSeen = 1;
    char line[600];
    sprintf_s(line, sizeof(line),
        "{\"t\":\"setter\",\"obs\":\"%s\",\"seq\":%u,\"tid\":%lu,\"tile\":%u,\"board\":%u,"
        "\"x\":%u,\"y\":%u,\"arg\":%u,\"pre\":%u,\"post\":%u,\"pre_valid\":%d,\"post_valid\":%d}",
        sobsArm.obsNonce, sobsArm.seq, (unsigned long)GetCurrentThreadId(), sobsSetterTile,
        sobsSetterBoard, sobsSetterX, sobsSetterY, sobsSetterArg, sobsSetterPreValue,
        sobsSetterPostValue, sobsSetterPreValue != 0xFFFFFFFFu,
        sobsSetterPostValue != 0xFFFFFFFFu);
    SobsEvent(line);
    SobsStatus("setter_recorded", "setter");
}
static SobsWindow* SobsNewWindow() {
    SobsWindow* slot = NULL;
    for (int i = 0; i < SOBS_MAX_WINDOWS; ++i) {
        if (!sobsWindow[i].used) { slot = &sobsWindow[i]; break; }
    }
    if (!slot) {
        InterlockedIncrement(&sobsWindowsDropped);
        sobsDeficient = 1;
        if (!sobsLimited) {
            char line[220];
            sprintf_s(line, sizeof(line),
                "{\"t\":\"limit\",\"reason\":\"payload_capacity\",\"capacity\":%d,"
                "\"dropped\":%ld}", SOBS_MAX_WINDOWS, (long)sobsWindowsDropped);
            SobsEvent(line);
            sobsLimited = 1;
        }
        SobsStatus("partial", "capacity");
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    InterlockedIncrement(&sobsWindowsUsed);
    slot->used = 1; slot->active = 1; slot->saveId = ++sobsSaveId;
    slot->tid = GetCurrentThreadId();
    slot->ebp = sobsBuilderEbp; slot->thisPtr = sobsBuilderThis;
    slot->outPtr = sobsBuilderOut; slot->flag = sobsBuilderFlag;
    return slot;
}
static void SobsEmitMap(SobsWindow* window, SobsMap* map) {
    if (!window || !map) return;
    std::string cells;
    for (int i = 0; i < map->cells; ++i) {
        const SobsCell& cell = map->cell[i];
        char piece[80];
        sprintf_s(piece, sizeof(piece), "%s{\"x\":%u,\"y\":%u,\"terrain\":%u,\"include\":%u,"
            "\"valid\":%d}", i ? "," : "", cell.x, cell.y, cell.terrain, cell.include,
            cell.valid);
        cells += piece;
    }
    std::string line;
    char head[600];
    sprintf_s(head, sizeof(head),
        "{\"t\":\"map\",\"save_id\":%u,\"board\":%u,\"rowset\":%u,\"name\":\"%s\","
        "\"cells\":%d,\"complete\":%d,\"truncated\":%d,\"items\":[",
        window->saveId, map->board, map->rowSet, SoJson(map->name, strnlen(map->name, 64)).c_str(),
        map->cells, map->complete, map->truncated);
    line = head; line += cells; line += "]}";
    SobsEvent(line.c_str());
}
static void SobsCloseWindow(SobsWindow* window) {
    if (!window || !window->active) return;
    window->active = 0;
    // Payload copy happens here, after the original builder has returned.
    bool ok = false;
    unsigned int object = window->ebp - 0x28;
    unsigned int length = SoReadU32(object + 0x10, &ok);
    unsigned int capacity = ok ? SoReadU32(object + 0x14, &ok) : 0;
    unsigned int data = object;
    if (ok && capacity >= 0x10) data = SoReadU32(object, &ok);
    if (ok && length <= SOBS_MAX_BYTES) {
        std::string text(length, '\0');
        ok = length == 0 || SoCopyBytes(data, &text[0], length);
        char name[64];
        sprintf_s(name, sizeof(name), "native_p03_saveobs_payload_%u.lua", window->saveId);
        window->payloadBytes = length;
        if (ok) strcpy_s(window->payloadSha, HashBuffer((const unsigned char*)text.data(), length).c_str());
        std::wstring path = root + L"\\" + Utf8ToWide(std::string(name));
        window->payloadOk = (ok && !root.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES
            && slprobe::Publish(path, text)) ? 1 : 0;
    } else if (ok) {
        window->payloadBytes = length;
        window->truncated = 1;
    }
    for (int i = 0; i < window->maps; ++i) {
        window->map[i].complete = window->map[i].cells == SOBS_MAX_CELLS && !window->map[i].truncated;
        SobsEmitMap(window, &window->map[i]);
    }
    if (!window->payloadOk || window->truncated || window->nested) sobsDeficient = 1;
    char line[700];
    sprintf_s(line, sizeof(line),
        "{\"t\":\"builder\",\"save_id\":%u,\"tid\":%lu,\"ebp\":%u,\"this\":%u,\"flag\":%u,"
        "\"bytes\":%u,\"sha256\":\"%s\",\"ok\":%d,\"truncated\":%d,\"maps\":%d,\"nested\":%d}",
        window->saveId, (unsigned long)window->tid, window->ebp, window->thisPtr, window->flag,
        window->payloadBytes, window->payloadSha, window->payloadOk, window->truncated,
        window->maps, window->nested);
    SobsEvent(line);
    SobsStatus("published", "builder");
}
extern "C" void SaveObsBuilderPre(void) {
    InterlockedIncrement(&sobsBuilderPre);
    if (!sobsArm.armed || SobsExpired()) return;
    if (sobsDepth++ > 0) { if (sobsActive) sobsActive->nested = 1; return; }
    sobsActive = SobsNewWindow();
}
extern "C" void SaveObsBuilderPost(void) {
    InterlockedIncrement(&sobsBuilderPost);
    if (sobsDepth > 0) --sobsDepth;
    // A nested builder returning must not close the window its caller owns.
    if (sobsDepth > 0) return;
    if (!sobsActive) {
        // Armed but no window could be allocated: still publish the latest counters.
        if (sobsArm.armed) SobsStatus("partial", "builder_no_window");
        return;
    }
    SobsCloseWindow(sobsActive);
    sobsActive = NULL;
}
extern "C" void SaveObsMapCell(unsigned frame, unsigned tile) {
    InterlockedIncrement(&sobsMapCalls);
    sobsMapFrame = frame; sobsMapTile = tile;
    bool ok = false;
    sobsMapX = SoReadU32(frame - 0x58, &ok);
    sobsMapY = ok ? SoReadU32(frame - 0x64, &ok) : 0xFFFFFFFFu;
    sobsMapInclude = ok ? SoReadU8(frame - 0x4D, &ok) : 0xFFu;
    sobsMapBoard = ok ? SoReadU32(frame - 0x48, &ok) : 0;
    sobsMapRowSet = ok ? SoReadU32(sobsMapBoard + 0x50, &ok) : 0;
    sobsMapTerrain = ok ? SoReadU32(tile + 0x2AE0, &ok) : 0xFFFFFFFFu;
    if (!ok) { sobsMapX = 0xFFFFFFFFu; sobsMapY = 0xFFFFFFFFu; sobsMapInclude = 0xFFu; }
    if (!sobsActive || !sobsArm.armed || SobsExpired()) return;
    SobsWindow* window = sobsActive;
    SobsMap* map = window->maps ? &window->map[window->maps - 1] : NULL;
    // A new occurrence starts on a different board/rows or a coordinate restart at (0,0).
    if (!map || map->board != sobsMapBoard || map->rowSet != sobsMapRowSet
        || (sobsMapX == 0 && sobsMapY == 0 && map->cells >= SOBS_MAX_CELLS)) {
        if (window->maps >= SOBS_MAX_MAPS) { window->truncated = 1; return; }
        if (map) map->complete = map->cells == SOBS_MAX_CELLS;
        map = &window->map[window->maps++];
        memset(map, 0, sizeof(*map));
        map->board = sobsMapBoard; map->rowSet = sobsMapRowSet;
        bool nameOk = false;
        unsigned int object = sobsMapBoard + 0x2C1C;
        unsigned int length = SoReadU32(object + 0x10, &nameOk);
        unsigned int capacity = nameOk ? SoReadU32(object + 0x14, &nameOk) : 0;
        unsigned int data = object;
        if (nameOk && capacity >= 0x10) data = SoReadU32(object, &nameOk);
        if (nameOk && length < 64) {
            __try { memcpy(map->name, (const void*)data, length); map->name[length] = 0; }
            __except (EXCEPTION_EXECUTE_HANDLER) { map->name[0] = 0; }
            map->nameLen = length;
        } else map->name[0] = 0;
        unsigned int dims = SoReadU32(sobsMapBoard + 0x48, &nameOk);
        map->complete = 0;
        (void)dims;
    }
    if (map->cells >= SOBS_MAX_CELLS) { map->truncated = 1; return; }
    SobsCell& cell = map->cell[map->cells++];
    cell.x = sobsMapX; cell.y = sobsMapY; cell.terrain = sobsMapTerrain;
    cell.include = sobsMapInclude;
    cell.valid = sobsMapX != 0xFFFFFFFFu && sobsMapY != 0xFFFFFFFFu
        && sobsMapInclude != 0xFFu && sobsMapTerrain != 0xFFFFFFFFu;
    window->cells++;
}
extern "C" void SaveObsWriterPre(unsigned pathPtr, unsigned contentPtr, unsigned frame) {
    InterlockedIncrement(&sobsWriterPre);
    sobsWriterPathPtr = pathPtr; sobsWriterContentPtr = contentPtr;
    sobsWriterActive = NULL;
    if (!sobsArm.armed || SobsExpired()) return;
    DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < SOBS_MAX_WINDOWS; ++i) {
        SobsWindow* window = &sobsWindow[i];
        if (!window->used || window->active || window->writerSeen || window->tid != tid || window->ebp != frame) continue;
        if (!sobsWriterActive || sobsWriterActive->saveId < window->saveId) sobsWriterActive=window;
    }
    if (sobsWriterActive) {
        SobsWindow* window=sobsWriterActive;
        window->writerSeen = 1;
        bool ok = false;
        unsigned int length = SoReadU32(pathPtr + 0x10, &ok);
        unsigned int capacity = ok ? SoReadU32(pathPtr + 0x14, &ok) : 0;
        unsigned int data = pathPtr;
        if (ok && capacity >= 0x10) data = SoReadU32(pathPtr, &ok);
        window->writerPathLen = ok ? length : 0xFFFFFFFFu;
        if (ok && length < sizeof(window->writerPath)) {
            if (!SoCopyBytes(data, window->writerPath, length)) window->writerPath[0] = 0;
            window->writerPath[length] = 0;
        }
        unsigned int contentLen = SoReadU32(contentPtr + 0x10, &ok);
        unsigned int contentCap = ok ? SoReadU32(contentPtr + 0x14, &ok) : 0;
        unsigned int contentData = contentPtr;
        if (ok && contentCap >= 0x10) contentData = SoReadU32(contentPtr, &ok);
        if (ok && contentLen <= SOBS_MAX_BYTES) {
            window->writerBytes = contentLen;
            std::string text(contentLen, '\0');
            if (contentLen == 0 || SoCopyBytes(contentData, &text[0], contentLen))
                strcpy_s(window->writerSha, HashBuffer((const unsigned char*)text.data(), contentLen).c_str());
        }
    }
    if (!sobsWriterActive) {
        sobsDeficient = 1;
        SobsEvent("{\"t\":\"unbound\",\"reason\":\"writer_without_completed_frame\"}");
    }
}
extern "C" void SaveObsWriterPost(unsigned al) {
    InterlockedIncrement(&sobsWriterPost);
    sobsWriterAL = al & 0xFF;
    if (!sobsArm.armed) return;
    SobsWindow* window = sobsWriterActive;
    sobsWriterActive = NULL;
    DWORD tid = GetCurrentThreadId();
    if (window) {
        window->writerAL = al & 0xFF;
        window->writerMatches = window->payloadOk && window->payloadSha[0]
            && strcmp(window->payloadSha, window->writerSha) == 0;
        char line[900];
        sprintf_s(line, sizeof(line),
            "{\"t\":\"writer\",\"save_id\":%u,\"tid\":%lu,\"ebp\":%u,\"bound\":1,\"path\":\"%s\",\"path_bytes\":%u,"
            "\"bytes\":%u,\"sha256\":\"%s\",\"al\":%u,\"matches_builder\":%d,"
            "\"builder_sha256\":\"%s\"}",
            window->saveId, (unsigned long)tid, window->ebp, SoJson(window->writerPath,
                strnlen(window->writerPath, sizeof(window->writerPath))).c_str(),
            window->writerPathLen, window->writerBytes, window->writerSha, window->writerAL,
            window->writerMatches, window->payloadSha);
        SobsEvent(line);
    }
    SobsStatus(sobsLimited || !window ? "partial" : "published", "writer");
}
extern "C" __declspec(naked) void ThunkSetter() { __asm {
    pushfd
    pushad
    mov ebx,esp
    sub esp,528
    and esp,0FFFFFFF0h
    fxsave [esp]
    push dword ptr [ebx+40]
    push dword ptr [ebx+16]
    push dword ptr [ebx]
    push dword ptr [ebx+4]
    push dword ptr [ebx+24]
    call SaveObsSetterPre
    add esp, 20
    fxrstor [esp]
    mov esp,ebx
    popad
    popfd
    push dword ptr [esp+4]
    call dword ptr [sobsSetterFn]
    pushfd
    pushad
    mov ebx,esp
    sub esp,528
    and esp,0FFFFFFF0h
    fxsave [esp]
    push dword ptr [ebx+28]
    call SaveObsSetterPost
    add esp, 4
    fxrstor [esp]
    mov esp,ebx
    popad
    popfd
    ret 4
} }
extern "C" __declspec(naked) void ThunkBuilder() { __asm {
    pushfd
    pushad
    mov ebx,esp
    sub esp,528
    and esp,0FFFFFFF0h
    fxsave [esp]
    mov eax, dword ptr [ebx+40]
    mov dword ptr [sobsBuilderOut], eax
    mov eax, dword ptr [ebx+44]
    mov dword ptr [sobsBuilderFlag], eax
    mov dword ptr [sobsBuilderEbp], ebp
    mov dword ptr [sobsBuilderThis], ecx
    call SaveObsBuilderPre
    fxrstor [esp]
    mov esp,ebx
    popad
    popfd
    push dword ptr [esp+8]
    push dword ptr [esp+8]
    call dword ptr [sobsBuilderFn]
    pushfd
    pushad
    mov ebx,esp
    sub esp,528
    and esp,0FFFFFFF0h
    fxsave [esp]
    call SaveObsBuilderPost
    fxrstor [esp]
    mov esp,ebx
    popad
    popfd
    ret 8
} }
extern "C" __declspec(naked) void ThunkMapJoin() { __asm {
    pushfd
    pushad
    mov ebx,esp
    sub esp,528
    and esp,0FFFFFFF0h
    fxsave [esp]
    push esi
    push ebp
    call SaveObsMapCell
    add esp, 8
    fxrstor [esp]
    mov esp,ebx
    popad
    popfd
    mov dword ptr [ebp-4], 0FFFFFFFFh
    push dword ptr [sobsJoinResume]
    ret
} }
static bool SaveObsInstall(unsigned char* base) {
    // The caller verifies all four sites on the pristine image before any patch is applied;
    // verifying here would fail because the writer CALL is already redirected by then.
    sobsSetterFn = base + 0x1A1BA0;
    sobsBuilderFn = base + 0x20B230;
    sobsJoinResume = base + 0x1640EA;
    if (!PatchCall(base + 0x165F58, (void*)ThunkSetter)) return false;
    if (!PatchCall(base + 0x20A0D2, (void*)ThunkBuilder)) return false;
    if (!PatchJump(base + 0x1640E3, (void*)ThunkMapJoin, 7)) return false;
    sobsCapable = 1;
    return true;
}
#endif

static bool Hook() {
    unsigned char* base=(unsigned char*)GetModuleHandleW(NULL);
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
    // Byte gate first: it must see the untouched image, including the writer CALL.
    if(!SaveObsVerify(base)){Log("SAVEOBS byte gate failed\n");return false;}
#endif
    targetA=base+0x209104;continueA=base+0x20A670;saveFn=base+0x20A050;loadFn=base+0x209900;
    origExistsFn=base+0x0A08C0;
    unsigned char* gateMain=base+0x209B5F;unsigned char* gateBackup=base+0x209B6D;
    unsigned char* findTarget=base+0x6DFD0;
    unsigned char* entryCheckTarget=base+0x6F306;
    unsigned char* fileCheckTarget=base+0x6D51E;
    unsigned char* fileEntryTarget=base+0x6D4D0;
    unsigned char* fileCmpTarget=base+0x6D4E9;
    unsigned char* fileKeyTarget=base+0x6D4FA;
    unsigned char* fileStateTarget=base+0x6D506;
    unsigned char* captureTarget=base+0x20A1D8;
    unsigned char expectedCapture[]={0xE8,0xE3,0x55,0xE9,0xFF};
    unsigned char expected[]={0xE8,0x67,0x15,0x00,0x00};
    unsigned char prologue[]={0x55,0x8B,0xEC,0x6A,0xFF};
    unsigned char expectedMain[]={0xE8,0x5C,0x6D,0xE9,0xFF};
    unsigned char expectedBackup[]={0xE8,0x4E,0x6D,0xE9,0xFF};
    unsigned char expectedFind[]={0x55,0x8B,0xEC,0x51,0x8B,0x01};
    unsigned char expectedEntry[]={0x8B,0x06,0x8B,0xCE,0x8B,0x40,0x04,0xFF,0xD0};
    unsigned char expectedFile[]={0x8B,0x01,0x8B,0x40,0x04};
    unsigned char expectedFE[]={0x55,0x8B,0xEC,0x51,0x53};
    unsigned char expectedFC[]={0x3B,0x73,0x28,0x74,0x10};
    unsigned char expectedFK[]={0x84,0xC0,0x74,0x03,0x8B,0x73,0x28};
    unsigned char expectedFS[]={0x8B,0x4E,0x28,0x8B,0x41,0x20};
    if(memcmp(targetA,expected,5) || memcmp(saveFn,prologue,5) || memcmp(loadFn,prologue,5))return false;
    if(memcmp(gateMain,expectedMain,5) || memcmp(gateBackup,expectedBackup,5))return false;
    if(memcmp(findTarget,expectedFind,6))return false;
    if(memcmp(entryCheckTarget,expectedEntry,9))return false;
    if(memcmp(fileCheckTarget,expectedFile,5))return false;
    if(memcmp(fileEntryTarget,expectedFE,5) || memcmp(fileCmpTarget,expectedFC,5) ||
       memcmp(fileKeyTarget,expectedFK,sizeof(expectedFK)) || memcmp(fileStateTarget,expectedFS,sizeof(expectedFS)))return false;
    if(memcmp(captureTarget,expectedCapture,5))return false;
    if(!Import("CreateFileA",(void*)WatchOpenA,(void**)&realOpenA) || !Import("CreateFileW",(void*)WatchOpenW,(void**)&realOpenW) ||
       !Import("ReadFile",(void*)WatchRead,(void**)&realRead) || !Import("WriteFile",(void*)WatchWrite,(void**)&realWrite) || !Import("CloseHandle",(void*)WatchClose,(void**)&realClose) || !Import("luaL_loadfile",(void*)WatchLuaFile,(void**)&realLuaFile))return false;
    DWORD old; if(!VirtualProtect(targetA,5,PAGE_EXECUTE_READWRITE,&old))return false;
    memcpy(originalA,targetA,5);unsigned char patch[5];patch[0]=0xE8;
    int rel=(int)((unsigned char*)TrampolineA-((unsigned char*)targetA+5));memcpy(patch+1,&rel,4);memcpy(targetA,patch,5);
    DWORD unused;VirtualProtect(targetA,5,old,&unused);FlushInstructionCache(GetCurrentProcess(),targetA,5);
    if(!PatchCall(gateMain,(void*)ThunkMain) || !PatchCall(gateBackup,(void*)ThunkBackup))return false;
    findTrampoline=MakeTrampoline(findTarget,6);
    if(!findTrampoline)return false;
    if(!PatchJump(findTarget,(void*)ThunkFind,6))return false;
    if(!PatchCallN(entryCheckTarget,(void*)ThunkEntryCheck,9))return false;
    fileResume=base+0x6D525;
    if(!PatchJump(fileCheckTarget,(void*)ThunkFileCheck,5))return false;
    if(!PatchFileBranches(fileEntryTarget))return false;
    realWriteFn=base+0x9F7C0;
    realMalloc=base+0x357870;realFree=base+0x357862;
    realOpenFn=base+0xA0BE0;realSizeFn=base+0xA0870;
    realReadFn=base+0xA0FC0;realCloseFn=base+0xA0DE0;
    realRootFn=base+0x9EC80;realProfileFn=base+0x14F600;
    realBackendFn=base+0x9E3F0;realRelativeFn=base+0x9E4B0;
    // 0x60A1D8 is a CALL site; use PatchCall so the thunk receives a return address.
    if(!PatchCall(captureTarget,(void*)ThunkCaptureWrite))return false;
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
    if(!SaveObsInstall(base))return false;
#endif
    hookA=1;return true;
}
#ifdef ITB_SL_COLD_DIAGNOSTIC
// One-shot cold diagnostic input. Explicit macro only; the normal savedName /
// savedHash gate below is kept intact and still runs for every other case.
static const char coldName[]="p01_CD78.lua";
static const char coldHash[]="8c018559eee82280578ef6862ed6086bc29ff1ac7bd422b2d9f697603a8f92e0";
static bool ColdOnceAllowed(const std::string& kind,const std::string& name,const std::string& hash,unsigned long last) {
    return kind=="load" && last==0 && name==coldName && hash==coldHash;
}
#endif
static bool PrepArmFields(const std::vector<std::string>& parts){
    return parts.size()==6 && parts[4]=="-" && parts[5].size()==64;
}
static bool Arm(const slprobe::Record& request) {
    sourceReady=false; sourceFileW.clear(); sourceFileHash.clear();
    auto parts=Split(Read(L"native_arm.txt"));
    char seq[11];sprintf_s(seq,"%010lu",request.seq);
    if(parts.size()!=6 || parts[0]!=session || parts[1]!=seq || parts[2]!=request.kind
        || !(slnative::ValidName(parts[3]) || slnative::ValidNameP04(parts[3])))return false;
    if(IsPrepareKind(request.kind)) {
        // Prepare-only has no U target: the arm path field is "-" and the hash is the source S.
        if(!PrepArmFields(parts) || !ReadSourceLicense() || parts[5]!=sourceFileHash)return false;
        target.clear(); expectedHash=parts[5];
        memset(argument,0,24); memcpy(argument,parts[3].data(),parts[3].size());
        *(unsigned int*)(argument+16)=(unsigned int)parts[3].size(); *(unsigned int*)(argument+20)=15;
        queuedAt=GetTickCount64(); return action.Accept(request,parts[3]);
    }
    // The real U path is ASCII here. A canonical, absolute path is passed by the local controller.
    std::wstring path(parts[4].begin(),parts[4].end());
    const std::wstring prefix=L"C:\\ITB-SL-Research\\userdata\\Into the Breach\\profile_";
    std::wstring name(parts[3].begin(),parts[3].end());
    if(path.rfind(prefix,0)!=0 || path.find(L"..")!=std::wstring::npos || path.substr(path.find_last_of(L"\\")+1)!=name)return false;
    if(request.kind=="save") {
        if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES || parts[5]!="-")return false;
    } else {
        if(parts[5].size()!=64)return false;
        bool normal=parts[3]==savedName && parts[5]==savedHash && Hash(path)==savedHash;
        bool source=ReadSourceLicense() && parts[5]==sourceFileHash;
        if(!source)sourceReady=false; // A valid but unrelated old license must not replace a normal/cold input.
        if(!normal && !source) {
#ifdef ITB_SL_COLD_DIAGNOSTIC
            if(!ColdOnceAllowed(request.kind,parts[3],parts[5],action.last) || Hash(path)!=parts[5])return false;
            Log("COLD_ONCE seq=%lu name=%s hash=%s\n",request.seq,parts[3].c_str(),parts[5].c_str());
#else
            return false;
#endif
        }
    }
    target=path;expectedHash=parts[5];memset(argument,0,24);memcpy(argument,parts[3].data(),parts[3].size());
    *(unsigned int*)(argument+16)=(unsigned int)parts[3].size();*(unsigned int*)(argument+20)=15;
    queuedAt=GetTickCount64();return action.Accept(request,parts[3]);
}
static DWORD WINAPI Worker(void*) {
#ifdef ITB_SL_P04_SERVICE
    return P04WorkerInit();
#else
    wchar_t modulePath[MAX_PATH],exePath[MAX_PATH];
    if(!GetModuleFileNameW(module,modulePath,MAX_PATH) || !GetModuleFileNameW(NULL,exePath,MAX_PATH))return 1;
    root=modulePath;root.resize(root.find_last_of(L"\\/"));root.resize(root.find_last_of(L"\\/"));
    if(Hash(exePath)!=exeHash){Log("ABORT unsupported EXE hash\n");return 1;}
    if(!Hook()){Log("ABORT hook/import setup\n");return 1;}
    slprobe::RequestChannel epoch;if(!epoch.Start(root+L"\\native_epoch")){Log("ABORT epoch start\n");return 1;}
    session=epoch.Session();SetAck("observe");
    if(!slprobe::Publish(root+L"\\native_ready.txt",session+"|native\n"))return 1;
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
    SobsReady();
#endif
    Log("START mode=native build=%s_%s pid=%lu session=%s supported=%s armed=0 update_tail_call=%p save=%p load=%p queue_deadline_ms=5000\n",__DATE__,__TIME__,GetCurrentProcessId(),session.c_str(),exeHash,targetA,saveFn,loadFn);
    for(;;) {
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
        SobsPoll();
#endif
        Sleep(100);
        // Publish retries never rerun an action.
        slprobe::Publish(root+L"\\native_ack.txt",ackText);
        if(action.phase==slnative::Queued && GetTickCount64()-queuedAt>5000 && action.Begin()) {
            Log("CANCELLED seq=%lu queue deadline; no native call\n",action.last);action.Finish(-3);
        }
        if(action.phase==slnative::Returned) {
            if(emittedSeq!=action.last) {
                char line[160];sprintf_s(line,"%s|%010lu|%s|native|returned|al=%ld|read=%ld|lua=%ld\n",session.c_str(),action.last,action.request.kind.c_str(),action.al,targetBytes,targetLuaOK);
                if(slprobe::Publish(root+L"\\native_result.txt",line)){emittedSeq=action.last;SetAck("returned");}
            }
            auto verify=Split(Read(L"native_verify.txt"));char seq[11];sprintf_s(seq,"%010lu",action.last);
            if(emittedSeq==action.last && verify.size()==4 && verify[0]==session && verify[1]==seq && verify[2]==action.request.kind && verify[3]=="verified") {
                std::string hash=Hash(target);
                bool saveOk=false;
                if(action.request.kind=="save") {
                    // The capture text, not the physical target, is what the engine wrote.
                    // The target may flush later; completeness is bound to this seq/name.
                    saveOk = action.al==-1 && capturePublished && CaptureComplete();
                    if(saveOk){ savedName=action.name; savedHash=HashBuffer((const unsigned char*)captureText.data(),(unsigned int)captureText.size()); }
                    Log("VERIFY_SAVE seq=%lu al=%ld capture_published=%d count=%ld capture_al=%ld bytes=%u path=%s disk_hash=%s ok=%d\n",
                        action.last,(long)action.al,capturePublished?1:0,(long)captureCount,(long)captureAL,
                        (unsigned)captureText.size(),capturePath.c_str(),hash.c_str(),saveOk?1:0);
                }
                bool allowed=action.request.kind=="save"?saveOk:action.al==1;
                if(allowed && action.Verify(action.last)){SetAck("verified");Log("VERIFIED seq=%lu kind=%s saved_hash=%s\n",action.last,action.request.kind.c_str(),savedHash.c_str());}
                else if(!allowed) Log("VERIFY_REFUSED seq=%lu kind=%s al=%ld\n",action.last,action.request.kind.c_str(),(long)action.al);
            }
            continue;
        }
        if(action.phase!=slnative::Idle && action.phase!=slnative::Verified && action.phase!=slnative::Rejected)continue;
        std::string text=Read(L"native_trigger.txt");slprobe::Record request;
        if(slprobe::ParseRequest(text,request) && request.session==session && request.seq>action.last && IsTriggerKind(request.kind)) {
            if(Arm(request)){SetAck("queued");Log("QUEUED seq=%lu kind=%s name=%s\n",request.seq,request.kind.c_str(),action.name.c_str());}
            continue;
        }
        // Prepare-only has its own file: the Lua hotkey path never sends it and never calls load.
        std::string prepareText=Read(L"native_prepare.txt");slprobe::Record prepareRequest;
        if(slprobe::ParseRequest(prepareText,prepareRequest) && prepareRequest.session==session && prepareRequest.seq>action.last && IsPrepareKind(prepareRequest.kind)) {
            if(Arm(prepareRequest)){SetAck("queued");Log("QUEUED seq=%lu kind=%s name=%s\n",prepareRequest.seq,prepareRequest.kind.c_str(),action.name.c_str());}
        }
    }
#endif
}
#ifdef ITB_SL_P04_SERVICE
#include "p04_native.inc"
#endif
extern "C" __declspec(dllexport) int luaopen_itb_sl_hook(void*) {return 0;}
#ifdef ITB_SL_P04_SERVICE
extern "C" __declspec(dllexport) int luaopen_itb_sl_p04(void* L) {return P04Open(L);}
#endif
#ifndef NATIVE_OFFLINE_ABI_TEST
BOOL APIENTRY DllMain(HMODULE h,DWORD why,void*) {
    if(why==DLL_PROCESS_ATTACH){module=h;DisableThreadLibraryCalls(h);
#ifndef ITB_SL_P04_SERVICE
        HANDLE thread=CreateThread(NULL,0,Worker,NULL,0,NULL);if(thread)CloseHandle(thread);
#endif
    }
    // Loaded for the process lifetime; no hot unload. OS tears down the private process mappings.
    return TRUE;
}
#else
static unsigned int testThis=0,testSize=0,testCap=0,testFlag=99;
static LONG testCalls=0,testAL=0;
__declspec(naked) void FakeTail(){__asm{
    mov dword ptr [testThis],ecx
    mov eax,13579BDFh
    xor ecx,ecx
    ret
}}
__declspec(naked) void FakeSave(){__asm{
    inc dword ptr [testCalls]
    mov dword ptr [testThis],ecx
    mov eax,dword ptr [esp+20]
    mov dword ptr [testSize],eax
    mov eax,dword ptr [esp+24]
    mov dword ptr [testCap],eax
    mov eax,dword ptr [esp+28]
    mov dword ptr [testFlag],eax
    ret 28
}}
__declspec(naked) void FakeLoad(){__asm{
    inc dword ptr [testCalls]
    mov eax,dword ptr [testAL]
    ret 24
}}
// T03 capture fixture: fake 0x49F7C0 and resume target.
static int fakeCaptureCalls=0;
static unsigned int fakeCaptureAL=1;
static unsigned int captureESPBefore=0, captureESPAfter=0;
extern "C" __declspec(naked) void FakeCaptureWrite(){__asm{
    inc dword ptr [fakeCaptureCalls]
    mov eax,dword ptr [fakeCaptureAL]
    ret
}}
static void CallThunkCapture(const std::string& path,const std::string& content){
    GStringValue pv,cv; SetValue(&pv,path); SetValue(&cv,content);
    __asm {
        mov captureESPBefore,esp
        sub esp,48
        lea esi,pv
        mov edi,esp
        mov ecx,6
        rep movsd
        lea esi,cv
        lea edi,[esp+24]
        mov ecx,6
        rep movsd
        call ThunkCaptureWrite
        add esp,48
    }
}
// T03 prepare fixtures: fake write/readback through the T02 contract.
static std::string fakePreparedContent;
static std::string fakeReadOverride;
static bool fakeReadUseOverride=false;
static int fakeWriteAL=1, fakeOpenCalls=0, fakeReadCalls=0, fakeCloseCalls=0;
static unsigned int fakeGateAL=1;
static bool fakeWriteWrong=false, fakeWriteMakesEntry=true;
static const char* fixtureUserRoot="C:\\ITB-SL-Research\\userdata\\Into The Breach\\";
static std::string fakeProfilePrefix, fakePreparedPath, fakeOpenedPath;
static std::vector<std::string> fakeQueriedPaths;
static unsigned int fakeBackendAL=1;
static bool fakeRelativeWrong=false;
static GStringValue* __fastcall FakeRoot(GStringValue* out,void*){
    SetValue(out,fixtureUserRoot);return out;
}
static GStringValue* __fastcall FakeProfile(GStringValue* out,void*){
    SetValue(out,fakeProfilePrefix);return out;
}
static unsigned int __cdecl FakeBackend(GStringValue path){
    std::string text((const char*)ValueData(&path),path.raw[4]);
    FreeValue(&path);
    return text.rfind(fixtureUserRoot,0)==0?fakeBackendAL:0;
}
static GStringValue* __cdecl FakeRelativeBody(GStringValue* out,GStringValue input){
    std::string text((const char*)ValueData(&input),input.raw[4]);
    FreeValue(&input);
    SetValue(out,fakeRelativeWrong?"wrong_key":text.substr(strlen(fixtureUserRoot)));
    return out;
}
// Mirror 0x49E4B0: ECX=output, a consumed 24-byte stack value, caller cleans stack.
extern "C" __declspec(naked) void FakeRelative(){__asm{
    push ebp
    mov ebp,esp
    push dword ptr [ebp+28]
    push dword ptr [ebp+24]
    push dword ptr [ebp+20]
    push dword ptr [ebp+16]
    push dword ptr [ebp+12]
    push dword ptr [ebp+8]
    push ecx
    call FakeRelativeBody
    add esp,28
    pop ebp
    ret
}}
static bool FixtureValueValid(const GStringValue& value){
    if(value.raw[5]+1<0x1000)return true;
    size_t data=value.raw[0];
    if(data&0x1F)return false;
    size_t allocation=(size_t)((void**)data)[-1];
    return allocation<data && data-allocation>=4 && data-allocation<=0x23;
}
static void* fixtureAllocation=0;
static void* fixtureRaw=0;
static void* __cdecl FixtureMalloc(size_t bytes){
    assert(!fixtureAllocation);
    fixtureAllocation=malloc(bytes+64);
    fixtureRaw=(void*)((((size_t)fixtureAllocation+0x1F)&~(size_t)0x1F)+8);
    return fixtureRaw;
}
static void __cdecl FixtureFree(void* allocation){
    assert(allocation==fixtureRaw);
    free(fixtureAllocation); fixtureAllocation=fixtureRaw=0;
}
static int __cdecl FakePrepareWrite(GStringValue path,GStringValue content){
    assert(FixtureValueValid(path) && FixtureValueValid(content));
    std::string pathText((const char*)ValueData(&path),path.raw[4]);
    fakePreparedPath=pathText;
    fakePreparedContent.assign((const char*)ValueData(&content),content.raw[4]);
    std::string written=fakeWriteWrong?std::string(fakePreparedContent.size(),'y'):fakePreparedContent;
    fakePreparedContent=written;
    // Logical engine paths may point to real U; fixture I/O is always build-local.
    HANDLE file=CreateFileA("p01_AB12.lua",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file!=INVALID_HANDLE_VALUE){ DWORD wrote=0; WriteFile(file,written.data(),(DWORD)written.size(),&wrote,NULL); CloseHandle(file); }
    FreeValue(&path); FreeValue(&content);
    if((fakeWriteAL&0xFF) && fakeWriteMakesEntry)fakeGateAL=1;
    return fakeWriteAL;
}
static void* __cdecl FakeOpen(GStringValue path){
    ++fakeOpenCalls;
    assert(FixtureValueValid(path));
    fakeOpenedPath.assign((const char*)ValueData(&path),path.raw[4]);
    FreeValue(&path);
    return (void*)1;
}
static unsigned int __cdecl FakeSize(){ return (unsigned int)fakePreparedContent.size(); }
static void* __cdecl FakeRead(){
    ++fakeReadCalls;
    const std::string& source=fakeReadUseOverride?fakeReadOverride:fakePreparedContent;
    size_t n=source.size();
    char* p=(char*)malloc(n+1);
    memcpy(p,source.data(),n); p[n]=0;
    return p;
}
static void __cdecl FakeClose(){ ++fakeCloseCalls; }
// Offline fixtures for the exists-gate wrapper. They exercise the same naked
// thunks used in the game build; the original callee pointer is redirected.
struct FakeSSO { char buf[16]; unsigned int len; unsigned int cap; };
struct FakeHeap { const char* data; unsigned int pad[3]; unsigned int len; unsigned int cap; };
static FakeSSO GateSSO(const char* text) {
    FakeSSO s; memset(&s,0,sizeof(s));
    size_t n=strlen(text); if(n>15)n=15;
    memcpy(s.buf,text,n); s.len=(unsigned int)n; s.cap=15; return s;
}
static unsigned int gateESPBefore, gateESPAfter, gateRet;
static int fakeGateCalls=0;
static unsigned int fakeGateECX=0, fakeGateEDX=0;
static unsigned int __fastcall FakePrepareExists(GStringValue* path,void*){
    ++fakeGateCalls;
    fakeQueriedPaths.emplace_back((const char*)ValueData(path),path->raw[4]);
    return fakeGateAL;
}
extern "C" __declspec(naked) void FakeExists(){__asm{
    inc dword ptr [fakeGateCalls]
    mov dword ptr [fakeGateECX],ecx
    mov dword ptr [fakeGateEDX],edx
    mov eax,dword ptr [fakeGateAL]
    ret
}}
static int nestCalls=0;
extern "C" __declspec(naked) void FakeExistsNested(){__asm{
    inc dword ptr [nestCalls]
    cmp dword ptr [nestCalls],1
    jne nest_done
    call ThunkMain
nest_done:
    mov eax,1
    ret
}}
static void CallThunkMain(void* s,unsigned int flag){
    __asm {
        mov gateESPBefore,esp
        mov ecx,s
        mov edx,flag
        call ThunkMain
        mov gateRet,eax
        mov gateESPAfter,esp
    }
}
static void CallThunkBackup(void* s,unsigned int flag){
    __asm {
        mov gateESPBefore,esp
        mov ecx,s
        mov edx,flag
        call ThunkBackup
        mov gateRet,eax
        mov gateESPAfter,esp
    }
}
// Offline fixtures for the tree-lookup wrapper: fake map, node, entry and callee.
struct FakeMap { void* end; };
struct FakeNode { char pad[0x28]; void* entry; };
struct FakeEntry { char pad[0x20]; unsigned int state; };
static int fakeFindCalls=0;
static unsigned int fakeFindKey=0, fakeFindNode=0;
extern "C" __declspec(naked) void FakeFind(){__asm{
    inc dword ptr [fakeFindCalls]
    mov eax,dword ptr [esp+4]
    mov dword ptr [fakeFindKey],eax
    mov eax,dword ptr [fakeFindNode]
    ret 4
}}
static void CallThunkFind(void* map,void* key){
    __asm {
        push key
        mov ecx,map
        call ThunkFind
    }
}
static int fakeNestedCalls=0;
static unsigned int nestedKey=0, nestedMap=0, nestedNode=0;
extern "C" __declspec(naked) void FakeFindNested(){__asm{
    inc dword ptr [fakeNestedCalls]
    cmp dword ptr [fakeNestedCalls],1
    jne nested_done
    push dword ptr [nestedKey]
    mov ecx,dword ptr [nestedMap]
    call ThunkFind
nested_done:
    mov eax,dword ptr [nestedNode]
    ret 4
}}
struct FakeCheckVtable { void* pad0; void* check; };
struct FakeCheckEntry { void* vtable; };
static int fakeCheckCalls=0;
static unsigned int fakeCheckThis=0, fakeCheckAL=1;
extern "C" __declspec(naked) void FakeCheck(){__asm{
    inc dword ptr [fakeCheckCalls]
    mov dword ptr [fakeCheckThis],esi
    mov eax,dword ptr [fakeCheckAL]
    ret
}}
static void CallThunkEntryCheck(void* entry){
    __asm {
        mov esi,entry
        call ThunkEntryCheck
    }
}
// Offline fixture for the file-entry-check wrapper: fake request/method frame,
// entry and check with ret 0, and a resume target.
static int fakeFileCheckCalls=0;
static unsigned int fakeFileCheckThis=0, fakeFileCheckAL=1;
static unsigned int fileESPBefore=0, fileESPAfter=0;
extern "C" __declspec(naked) void FakeFileCheck(){__asm{
    inc dword ptr [fakeFileCheckCalls]
    mov dword ptr [fakeFileCheckThis],ecx
    mov eax,dword ptr [fakeFileCheckAL]
    ret
}}
extern "C" __declspec(naked) void FakeFileResume(){__asm{
    test al,al
    jnz fake_file_resume_done
    mov eax,dword ptr [ebp-4]
    mov byte ptr [eax+4],1
fake_file_resume_done:
    mov dword ptr [fileESPAfter],esp
    ret
}}
static unsigned int fakeFrame[32];
static void CallThunkFileCheck(void* entry,void* request,unsigned int parent,unsigned int key){
    __asm {
        mov eax,offset fakeFrame
        add eax,64
        mov ecx,request
        mov dword ptr [eax-4],ecx
        mov ecx,parent
        mov dword ptr [eax+8],ecx
        mov ecx,key
        mov dword ptr [eax+0Ch],ecx
        push ebp
        push ebx
        push esi
        push edi
        mov ecx,entry
        mov ebp,eax
        mov fileESPBefore,esp
        call ThunkFileCheck
        pop edi
        pop esi
        pop ebx
        pop ebp
    }
}
// Offline fixtures for the 0x46D4D0 entry and cmp/key/state wrappers.
static unsigned int feESPBefore=0, feESPAfter=0;
static unsigned int fcmpESPBefore=0, fcmpESPAfter=0;
static unsigned int fkeyESPBefore=0, fkeyESPAfter=0, fkeyESIAfter=0;
static unsigned int fstateESPBefore=0, fstateESPAfter=0, fstateEAX=0, fstateECX=0;
extern "C" __declspec(naked) void FakeEntryResume(){__asm{
    mov ebp,dword ptr [esp+8]
    add esp,12
    mov dword ptr [feESPAfter],esp
    ret
}}
extern "C" __declspec(naked) void FakeCmpResumeHit(){__asm{
    mov dword ptr [fcmpESPAfter],esp
    ret
}}
extern "C" __declspec(naked) void FakeCmpResumeMiss(){__asm{
    mov dword ptr [fcmpESPAfter],esp
    ret
}}
extern "C" __declspec(naked) void FakeKeyResume(){__asm{
    mov dword ptr [fkeyESIAfter],esi
    mov dword ptr [fkeyESPAfter],esp
    ret
}}
extern "C" __declspec(naked) void FakeStateResume(){__asm{
    mov dword ptr [fstateEAX],eax
    mov dword ptr [fstateECX],ecx
    mov dword ptr [fstateESPAfter],esp
    ret
}}
static unsigned int fakeNode[16];
static unsigned int fakeEntry[16];
static void CallThunkFileEntry(void* request,unsigned int arg1,unsigned int arg2){
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov feESPBefore,esp
        push arg2
        push arg1
        mov ecx,request
        call ThunkFileEntry
        add esp,8
        pop edi
        pop esi
        pop ebx
        pop ebp
    }
}
static void CallThunkFileCmp(unsigned int node,unsigned int sentinel){
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov fcmpESPBefore,esp
        mov esi,node
        mov ebx,offset fakeFrame
        mov eax,sentinel
        mov dword ptr [ebx+28h],eax
        call ThunkFileCmp
        pop edi
        pop esi
        pop ebx
        pop ebp
    }
}
static void CallThunkFileKey(unsigned int node,unsigned int alValue,unsigned int sentinel){
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov fkeyESPBefore,esp
        mov esi,node
        mov ebx,offset fakeFrame
        mov eax,sentinel
        mov dword ptr [ebx+28h],eax
        mov eax,alValue
        call ThunkFileKey
        pop edi
        pop esi
        pop ebx
        pop ebp
    }
}
static void CallThunkFileState(unsigned int node){
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov fstateESPBefore,esp
        mov esi,node
        call ThunkFileState
        pop edi
        pop esi
        pop ebx
        pop ebp
    }
}
static unsigned int methodCompareAL, methodCompareCalls, methodQuery, methodNodeKey;
extern "C" __declspec(naked) void FakeMethodCompare(){__asm{
    inc dword ptr [methodCompareCalls]
    mov eax,dword ptr [esp+4]
    mov dword ptr [methodQuery],eax
    mov eax,dword ptr [esp+8]
    mov dword ptr [methodNodeKey],eax
    mov eax,dword ptr [methodCompareAL]
    ret 8
}}
static void CallFileMethod(void* method,void* request,void* parent,void* key){
    __asm {
        push key
        push parent
        mov ecx,request
        call method
    }
}
static void TestKeySnapshots(){
    KeySnapshot value;
    FakeSSO shortValue=GateSSO("p01_CD78.lua");
    ReadKeySnapshot((unsigned int)&shortValue,&value);
    assert(value.readable && value.sso && !value.heap && !value.truncated);
    assert(value.object==(unsigned int)&shortValue && value.data==(unsigned int)&shortValue);
    assert(value.len==12 && value.cap==15 && value.copied==12);
    assert(memcmp(value.bytes,shortValue.buf,12)==0);
    shortValue.buf[3]=0;
    ReadKeySnapshot((unsigned int)&shortValue,&value);
    assert(value.readable && value.copied==12 && memcmp(value.bytes,shortValue.buf,12)==0);
    FakeSSO empty=GateSSO("");
    ReadKeySnapshot((unsigned int)&empty,&value);
    assert(value.readable && value.copied==0 && value.sso);
    std::string text(96,'K');text[2]=0;
    FakeHeap heap={text.data(),{0,0,0},96,96};
    ReadKeySnapshot((unsigned int)&heap,&value);
    assert(value.readable && value.heap && value.truncated && value.len==96 && value.cap==96 && value.copied==64);
    assert(value.data==(unsigned int)text.data() && memcmp(value.bytes,text.data(),64)==0);
    SYSTEM_INFO systemInfo;GetSystemInfo(&systemInfo);
    unsigned char* pages=(unsigned char*)VirtualAlloc(NULL,systemInfo.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    assert(pages);DWORD old;
    assert(VirtualProtect(pages+systemInfo.dwPageSize,systemInfo.dwPageSize,PAGE_NOACCESS,&old));
    unsigned char* boundary=pages+systemInfo.dwPageSize-64;
    memset(boundary,'Z',64);
    FakeHeap exact={(const char*)boundary,{0,0,0},64,64};
    ReadKeySnapshot((unsigned int)&exact,&value);
    assert(value.readable && value.copied==64 && !value.truncated);
    exact.len=65;exact.cap=65;
    ReadKeySnapshot((unsigned int)&exact,&value);
    assert(value.readable && value.copied==64 && value.truncated);
    assert(VirtualFree(pages,0,MEM_RELEASE));
    ReadKeySnapshot(1,&value);assert(!value.readable && !value.copied);
    FakeHeap bad={(const char*)1,{0,0,0},1,16};
    ReadKeySnapshot((unsigned int)&bad,&value);assert(!value.readable && !value.copied);
    bad.data=nullptr;bad.len=0;
    ReadKeySnapshot((unsigned int)&bad,&value);assert(value.readable && !value.copied && value.heap);
    bad.len=17;
    ReadKeySnapshot((unsigned int)&bad,&value);assert(!value.readable && !value.copied);
}
static void TestKeyPairing(){
    unsigned int frame[32]={0},node[16]={0};
    FakeSSO query=GateSSO("p01_CD78.lua"),candidate=GateSSO("saveData.lua");
    memcpy(node+4,&candidate,sizeof(candidate));
    unsigned int address=(unsigned int)(frame+16),nodeAddress=(unsigned int)node;
    frame[15]=0x12340000;frame[18]=0x56780000;frame[19]=(unsigned int)&query;
    action.phase=slnative::Running;actionTid=GetCurrentThreadId();action.last=1;
    ResetKeyCompare();BeginKeyCompare(address,nodeAddress,nodeAddress);
    assert(!keyCompareCount);
    action.phase=slnative::Idle;BeginKeyCompare(address,nodeAddress,0);
    assert(!keyCompareCount);
    action.phase=slnative::Running;actionTid=GetCurrentThreadId()+1;
    BeginKeyCompare(address,nodeAddress,0);assert(!keyCompareCount);
    actionTid=GetCurrentThreadId();BeginKeyCompare(address,nodeAddress,0);
    assert(keyCompareCount==1 && !keyCompareRecords[0].complete);
    assert(keyCompareRecords[0].queryValue.readable && keyCompareRecords[0].nodeValue.readable);
    EndKeyCompare(address,nodeAddress+4,1);
    assert(!keyCompareRecords[0].complete && keyCompareUnmatched==1);
    EndKeyCompare(address,nodeAddress,0xAB01);
    assert(keyCompareRecords[0].complete && keyCompareRecords[0].al==1);
    assert(keyCompareRecords[0].request==frame[15] && keyCompareRecords[0].parent==frame[18]);
    assert(memcmp(keyCompareRecords[0].queryValue.bytes,query.buf,12)==0);
    assert(memcmp(keyCompareRecords[0].nodeValue.bytes,candidate.buf,12)==0);
    std::string longQuery(32,'q'),longNode(80,'n');
    FakeHeap queryHeap={longQuery.data(),{0,0,0},32,32};
    FakeHeap nodeHeap={longNode.data(),{0,0,0},80,80};
    frame[19]=(unsigned int)&queryHeap;memcpy(node+4,&nodeHeap,sizeof(nodeHeap));
    ResetKeyCompare();BeginKeyCompare(address,nodeAddress,0);EndKeyCompare(address,nodeAddress,1);
    assert(keyCompareRecords[0].queryValue.heap && keyCompareRecords[0].queryValue.readable);
    assert(keyCompareRecords[0].nodeValue.heap && keyCompareRecords[0].nodeValue.readable);
    assert(keyCompareRecords[0].queryValue.copied==32 && !keyCompareRecords[0].queryValue.truncated);
    assert(keyCompareRecords[0].nodeValue.copied==64 && keyCompareRecords[0].nodeValue.truncated);
    assert(memcmp(keyCompareRecords[0].queryValue.bytes,longQuery.data(),32)==0);
    assert(memcmp(keyCompareRecords[0].nodeValue.bytes,longNode.data(),64)==0);
    frame[19]=(unsigned int)&query;memcpy(node+4,&candidate,sizeof(candidate));
    ResetKeyCompare();
    for(int index=0;index<17;++index) {
        BeginKeyCompare(address,nodeAddress,0);EndKeyCompare(address,nodeAddress,index&1);
    }
    assert(keyCompareCount==17 && keyCompareOverflow && keyCompareUnmatched==1);
    assert(keyCompareRecords[15].complete && keyCompareRecords[15].al==1);
    ResetKeyCompare();BeginKeyCompare(address,nodeAddress,0);
    action.last=2;EndKeyCompare(address,nodeAddress,0);
    assert(!keyCompareRecords[0].complete && keyCompareUnmatched==1);
    action.last=1;ResetKeyCompare();
    puts("PASS key snapshots/pairing: SSO, heap, embedded zero, empty, protected 64-byte boundary, truncation, invalid reads, sentinel/idle/wrong-thread exclusion, exact identity and overflow");
}
static void TestPatchedFileMethod(){
    const unsigned char originalMethod[]={
        0x55,0x8B,0xEC,0x51,0x53,0x8B,0x5D,0x08,0x56,0xFF,0x75,0x0C,
        0x89,0x4D,0xFC,0x8D,0x4B,0x28,0xE8,0xE9,0x0A,0,0,0x8B,0xF0,
        0x3B,0x73,0x28,0x74,0x10,0x8D,0x46,0x10,0x50,0xFF,0x75,0x0C,
        0xE8,0xF6,0x03,0,0,0x84,0xC0,0x74,0x03,0x8B,0x73,0x28,
        0x3B,0x73,0x28,0x74,0x10,0x8B,0x4E,0x28,0x8B,0x41,0x20,
        0x83,0xF8,0x03,0x74,0x05,0x83,0xF8,0x01,0x75,0x02,0x33,0xC9,
        0x5E,0x5B,0x85,0xC9,0x74,0x12,0x8B,0x01,0x8B,0x40,0x04,0xFF,
        0xD0,0x84,0xC0,0x75,0x07,0x8B,0x45,0xFC,0xC6,0x40,0x04,0x01,
        0x8B,0xE5,0x5D,0xC2,0x08,0
    };
    unsigned char* method=(unsigned char*)VirtualAlloc(NULL,sizeof(originalMethod),MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(method);
    FakeCheckVtable table={nullptr,(void*)FakeFileCheck};
    unsigned int parent[16]={0}, node[16]={0}, entry[16]={0}, sentinel[16]={0};
    FakeSSO key=GateSSO("p01_CD78.lua");
    FakeSSO nodeKey=GateSSO("saveData.lua");
    memcpy(node+4,&nodeKey,sizeof(nodeKey));
    parent[10]=(unsigned int)sentinel;
    node[10]=(unsigned int)entry;
    entry[0]=(unsigned int)&table;
    struct MethodCase { bool end; unsigned int compare, state, check, result; };
    const MethodCase cases[]={
        {true,0,2,0,0},{false,1,2,0,0},{false,0,1,0,0},
        {false,0,3,0,0},{false,0,2,1,0},{false,0,2,0,1}
    };
    for(int patched=0;patched<2;++patched) {
        memcpy(method,originalMethod,sizeof(originalMethod));
        assert(PatchCallN(method+0x12,(void*)FakeFind,5));
        assert(PatchCallN(method+0x25,(void*)FakeMethodCompare,5));
        if(patched) {
            assert(PatchFileBranches(method));
            assert(fcmpResumeHit==method+0x31);
            fileResume=method+0x55;
            assert(PatchJump(method+0x4E,(void*)ThunkFileCheck,5));
        }
        for(int running=0;running<2;++running) {
            action.phase=running?slnative::Running:slnative::Idle;
            actionTid=GetCurrentThreadId();
            gateClobberECX=true;
            for(const MethodCase& test:cases) {
                unsigned int request[4]={0};
                ResetFileCheck(); ResetFileBranch(); ResetKeyCompare();
                fakeFindNode=(unsigned int)(test.end?sentinel:node);
                fakeFindCalls=0; fakeFileCheckCalls=0;
                fakeFileCheckAL=test.check; methodCompareAL=test.compare;
                methodCompareCalls=0; entry[8]=test.state;
                CallFileMethod(method,request,parent,&key);
                bool reachesCheck=!test.end && !test.compare && test.state==2;
                assert(request[1]==test.result && fakeFindCalls==1);
                assert(methodCompareCalls==(test.end?0u:1u));
                assert(fakeFileCheckCalls==(reachesCheck?1:0));
                if(!test.end) {
                    assert(methodQuery==(unsigned int)&key);
                    assert(methodNodeKey==(unsigned int)(node+4));
                }
                if(patched && running) {
                    assert(keyCompareCount==(test.end?0:1) && !keyCompareUnmatched);
                    if(!test.end) {
                        assert(keyCompareRecords[0].complete && keyCompareRecords[0].al==test.compare);
                        assert(keyCompareRecords[0].query==(unsigned int)&key && keyCompareRecords[0].node==(unsigned int)node);
                        assert(keyCompareRecords[0].queryValue.readable && keyCompareRecords[0].nodeValue.readable);
                        assert(memcmp(keyCompareRecords[0].queryValue.bytes,key.buf,12)==0);
                        assert(memcmp(keyCompareRecords[0].nodeValue.bytes,nodeKey.buf,12)==0);
                    }
                    assert(fbRecords[0].arg1==(unsigned int)parent && fbRecords[0].arg2==(unsigned int)&key);
                    assert(fileCheckCount==(reachesCheck?1:0));
                    if(reachesCheck) {
                        assert(fileCheckRecords[0].parent==(unsigned int)parent && fileCheckRecords[0].key==(unsigned int)&key);
                        assert(fileCheckRecords[0].req4Before==0 && fileCheckRecords[0].req4After==0);
                        assert(fakeFileCheckThis==(unsigned int)entry);
                    }
                } else {
                    assert(fbCount==0 && fileCheckCount==0 && keyCompareCount==0);
                }
            }
        }
    }
    action.phase=slnative::Running;ResetKeyCompare();ResetFileCheck();ResetFileBranch();
    FakeHeap unreadable={(const char*)1,{0,0,0},1,16};
    memcpy(node+4,&unreadable,sizeof(unreadable));
    unsigned int request[4]={0};
    fakeFindNode=(unsigned int)node;fakeFindCalls=0;methodCompareCalls=0;
    methodCompareAL=1;fakeFileCheckCalls=0;entry[8]=2;
    CallFileMethod(method,request,parent,(void*)1);
    assert(fakeFindCalls==1 && methodCompareCalls==1 && fakeFileCheckCalls==0 && request[1]==0);
    assert(keyCompareCount==1 && keyCompareRecords[0].complete && keyCompareRecords[0].al==1);
    assert(!keyCompareRecords[0].queryValue.readable && !keyCompareRecords[0].nodeValue.readable);
    gateClobberECX=false;
    assert(VirtualFree(method,0,MEM_RELEASE));
    puts("PASS file-method integration: original instruction bytes with fake find/compare/check; patched and unpatched sentinel, key, state and check paths agree; Calling and idle paths; request+4 changes after the check snapshot; ECX clobber tolerated");
}
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
// ---- offline ABI sentinels for the three save-path observation sites ----
static int fakeSobsSetterCalls, fakeSobsBuilderCalls, fakeSobsResumeHits;
static int fakeSobsNested;
static int fakeSobsVariant;  // zero keeps the original ABI fixture unchanged
extern "C" void RunSobsBuilder();
static unsigned int fakeSobsSetterArg;
static unsigned int sobsEspBefore, sobsEspAfter;
static unsigned int sobsTestTilePtr, sobsTestBoardPtr, sobsTestXv, sobsTestYv;
static unsigned int sobsTestFramePtr, sobsTestOutPtr, sobsTestFlagV, sobsTestThisV;
static unsigned char sobsTileBuffer[0x2AE0+32];
static unsigned int sobsFrameBuffer[128];
static unsigned int sobsBoardBuffer[64];

extern "C" __declspec(naked) void FakeSobsSetter(){__asm{
    inc dword ptr [fakeSobsSetterCalls]
    mov eax,dword ptr [esp+4]
    mov dword ptr [fakeSobsSetterArg],eax
    mov eax,dword ptr [sobsTestTilePtr]
    mov dword ptr [eax+2AE0h],6
    mov eax,13579BDFh
    ret 4
}}
extern "C" __declspec(naked) void FakeSobsBuilder(){__asm{
    cmp dword ptr [fakeSobsNested],0
    je sobs_fake_not_nested
    mov dword ptr [fakeSobsNested],0
    call RunSobsBuilder
sobs_fake_not_nested:
    inc dword ptr [fakeSobsBuilderCalls]
    mov eax,dword ptr [esp+4]
    mov dword ptr [eax+10h],6
    mov dword ptr [eax+14h],0Fh
    mov dword ptr [eax],41414141h
    mov word ptr [eax+4],4242h
    cmp dword ptr [fakeSobsVariant],0
    je sobs_fake_original_text
    mov edx,dword ptr [fakeSobsVariant]
    add dl,40h
    mov byte ptr [eax],dl
    mov edx,dword ptr [fakeSobsVariant]
    and edx,3
    add edx,6
    mov dword ptr [eax+10h],edx
    mov dword ptr [eax+6],44444444h
sobs_fake_original_text:
    mov eax,1
    ret 8
}}
extern "C" __declspec(naked) void FakeSobsMapResume(){__asm{
    inc dword ptr [fakeSobsResumeHits]
    ret
}}
extern "C" __declspec(naked) void RunSobsSetter(){__asm{
    push ebx
    push esi
    push edi
    mov sobsEspBefore,esp
    push 6
    mov esi,dword ptr [sobsTestBoardPtr]
    mov ecx,dword ptr [sobsTestTilePtr]
    mov edi,dword ptr [sobsTestXv]
    mov ebx,dword ptr [sobsTestYv]
    call ThunkSetter
    mov sobsEspAfter,esp
    pop edi
    pop esi
    pop ebx
    ret
}}
extern "C" __declspec(naked) void RunSobsBuilder(){__asm{
    push ebx
    push esi
    push edi
    push ebp
    mov sobsEspBefore,esp
    mov ebp,dword ptr [sobsTestFramePtr]
    push dword ptr [sobsTestFlagV]
    push dword ptr [sobsTestOutPtr]
    mov ecx,dword ptr [sobsTestThisV]
    call ThunkBuilder
    mov sobsEspAfter,esp
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
}}
extern "C" __declspec(naked) void RunSobsMap(){__asm{
    push ebx
    push esi
    push edi
    push ebp
    mov sobsEspBefore,esp
    mov ebp,dword ptr [sobsTestFramePtr]
    mov esi,dword ptr [sobsTestTilePtr]
    call ThunkMapJoin
    mov sobsEspAfter,esp
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
}}
static void TestSaveObsAbi(){
    memset(sobsTileBuffer,0,sizeof(sobsTileBuffer));
    memset(sobsFrameBuffer,0,sizeof(sobsFrameBuffer));
    memset(sobsBoardBuffer,0,sizeof(sobsBoardBuffer));
    unsigned int tile=(unsigned int)(sobsTileBuffer+16);
    unsigned int frame=(unsigned int)(((unsigned char*)sobsFrameBuffer)+0x100);
    unsigned int board=(unsigned int)sobsBoardBuffer;
    sobsTestTilePtr=tile; sobsTestBoardPtr=board; sobsTestXv=1; sobsTestYv=3;
    sobsTestFramePtr=frame; sobsTestOutPtr=frame-0x28; sobsTestFlagV=1; sobsTestThisV=0x1234;
    sobsSetterFn=(void*)FakeSobsSetter;
    sobsBuilderFn=(void*)FakeSobsBuilder;
    sobsJoinResume=(unsigned char*)FakeSobsMapResume;
    sobsSetterCalls=0; sobsBuilderPre=0; sobsBuilderPost=0; sobsMapCalls=0;
    fakeSobsSetterCalls=0; fakeSobsBuilderCalls=0; fakeSobsResumeHits=0;
    *(unsigned int*)(tile+0x2AE0)=0;
    unsigned char fpBefore[16], fpAfter[16];
    memset(fpBefore,0x5A,sizeof(fpBefore));
    __asm { movups xmm0, xmmword ptr [fpBefore] }
    __asm { fld1 }
    RunSobsSetter();
    __asm { movups xmmword ptr [fpAfter],xmm0 }
    double x87Value=0;
    __asm { fstp qword ptr [x87Value] }
    assert(memcmp(fpBefore,fpAfter,16)==0 && x87Value==1.0);
    assert(fakeSobsSetterCalls==1 && sobsSetterCalls==1);
    assert(fakeSobsSetterArg==6 && sobsSetterArg==6);
    assert(sobsSetterPreValue==0 && sobsSetterPostValue==6);
    assert(sobsSetterRet==0x13579BDFu);
    assert(sobsSetterTile==tile && sobsSetterBoard==board && sobsSetterX==1 && sobsSetterY==3);
    assert(sobsEspAfter==sobsEspBefore);
    // Exercise actual recursive thunks, not only recorder depth bookkeeping.
    // Each invocation keeps its own site return address on its stack.
    sobsArm.armed=0;
    fakeSobsNested=1;
    int oldBuilderCalls=fakeSobsBuilderCalls;
    unsigned nestedEspBefore=0,nestedEspAfter=0;
    __asm { mov nestedEspBefore,esp }
    RunSobsBuilder();
    __asm { mov nestedEspAfter,esp }
    assert(fakeSobsBuilderCalls==oldBuilderCalls+2 && nestedEspAfter==nestedEspBefore);
    fakeSobsBuilderCalls=0; sobsBuilderPre=0; sobsBuilderPost=0;
    *(unsigned int*)(frame+8+0x10)=12;
    sobsArm.armed=0;
    RunSobsBuilder();
    assert(fakeSobsBuilderCalls==1 && sobsBuilderPre==1 && sobsBuilderPost==1);
    assert(sobsWindow[0].used==0);  // unarmed: nothing is recorded
    sobsArm.armed=1; sobsArm.armedAt=GetTickCount64(); sobsArm.ttlMs=600000;
    sobsArm.x=1; sobsArm.y=3; sobsArm.from=0; sobsArm.to=6;
    strcpy_s(sobsArm.obsNonce,"0123456789ABCDEF0123456789ABCDEF");
    RunSobsBuilder();
    assert(fakeSobsBuilderCalls==2 && sobsBuilderPre==2 && sobsBuilderPost==2);
    assert(sobsWindow[0].used && !sobsWindow[0].active);
    assert(sobsWindow[0].payloadBytes==6 && sobsWindow[0].payloadSha[0]);
    assert(sobsEspAfter==sobsEspBefore);
    *(unsigned int*)(frame-0x58)=2;
    *(unsigned int*)(frame-0x64)=3;
    *(unsigned char*)(frame-0x4D)=1;
    *(unsigned int*)(frame-0x48)=board;
    *(unsigned int*)(board+0x50)=(unsigned int)sobsBoardBuffer;
    *(unsigned int*)(tile+0x2AE0)=6;
    RunSobsMap();
    assert(fakeSobsResumeHits==1 && sobsMapCalls==1);
    assert(sobsMapX==2 && sobsMapY==3 && sobsMapInclude==1 && sobsMapTerrain==6);
    assert(sobsMapBoard==board && sobsMapRowSet==(unsigned int)sobsBoardBuffer);
    assert(sobsEspAfter==sobsEspBefore);
    sobsArm.armed=0;  // the pointer-only probe below must not consume a window binding
    sobsWriterPre=0; sobsWriterPost=0; sobsWriterAL=0xFF;
    SaveObsWriterPre(0x1111,0x2222,frame); SaveObsWriterPost(0x7F);
    assert(sobsWriterPre==1 && sobsWriterPost==1 && sobsWriterAL==0x7F);
    assert(sobsWriterPathPtr==0x1111 && sobsWriterContentPtr==0x2222);
    sobsMapCalls=0;
    SaveObsMapCell(0,0);
    assert(sobsMapX==0xFFFFFFFFu && sobsMapY==0xFFFFFFFFu && sobsMapInclude==0xFFu);
    // Byte gate: a synthetic image must be rejected until every site byte and rel32 agrees.
    unsigned char* image=(unsigned char*)VirtualAlloc(NULL,0x210000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(image);
    memset(image,0x90,0x210000);
    assert(!SaveObsVerify(image));
    struct { unsigned int va, target; } calls[3]={{0x165F58,0x1A1BA0},{0x20A0D2,0x20B230},{0x20A1D8,0x09F7C0}};
    for(int i=0;i<3;++i){ unsigned char* site=image+calls[i].va; site[0]=0xE8;
        int rel=(int)((image+calls[i].target)-(site+5)); memcpy(site+1,&rel,4); }
    unsigned char joinBytes[]={0xC7,0x45,0xFC,0xFF,0xFF,0xFF,0xFF};
    memcpy(image+0x1640E3,joinBytes,7);
    assert(SaveObsVerify(image));
    image[0x165F5A]^=0xFF;  // one wrong byte inside the setter CALL
    assert(!SaveObsVerify(image));
    image[0x165F5A]^=0xFF;  // restore the byte, then break only the rel32 target
    int badRel=(int)((image+0x1A1BA0)-(image+0x165F5D))+8;
    memcpy(image+0x165F59,&badRel,4);
    assert(!SaveObsVerify(image));
    VirtualFree(image,0,MEM_RELEASE);
    // Control gate: a well-formed 19-field line arms; wrong identity, field count or ttl is refused.
    session=std::string(24,'A');
    root=L"sobs_abi_scratch";
    CreateDirectoryW(root.c_str(),NULL);
    auto writeControl=[&](const std::string& text){
        FILE* file=NULL; _wfopen_s(&file,(root+L"\\native_p03_saveobs_control.txt").c_str(),L"w");
        if(file){fwrite(text.data(),1,text.size(),file);fclose(file);}
    };
    auto controlLine=[&](const std::string& sessionId,const std::string& ttl){
        return "P03SAVEOBS1|"+sessionId+"|"+std::to_string((unsigned long)GetCurrentProcessId())
            +"|"+std::string(32,'B')+"|"+std::string(32,'C')+"|0000000001|"
            +std::string(32,'D')+"|"+std::string(32,'E')+"|"+std::string(24,'A')+"_1|Alpha|"
            +std::string(64,'a')+"|"+std::string(64,'b')+"|"+std::string(64,'c')+"|"
            +std::string(64,'d')+"|1|3|0|6|"+ttl+"\n";
    };
    sobsControlHash.clear(); sobsArm.armed=0;
    writeControl(controlLine(session,"600000"));
    assert(SobsArmRead() && sobsArm.armed && sobsArm.x==1 && sobsArm.y==3
        && sobsArm.from==0 && sobsArm.to==6 && sobsArm.ttlMs==600000);
    sobsControlHash.clear(); sobsArm.armed=0;
    writeControl(controlLine(session,"0"));
    assert(!SobsArmRead() && !sobsArm.armed);
    sobsControlHash.clear(); sobsArm.armed=0;
    writeControl(controlLine(std::string(24,'F'),"600000"));
    assert(!SobsArmRead() && !sobsArm.armed);
    sobsControlHash.clear(); sobsArm.armed=0;
    std::string shortNonce=controlLine(session,"600000");
    shortNonce.replace(shortNonce.find(std::string(32,'C')),32,std::string(30,'C'));
    writeControl(shortNonce);
    assert(!SobsArmRead() && !sobsArm.armed);
    // Writer binding: the completed builder window on this thread binds once, keeps AL and hashes.
    unsigned char pathObject[64]; memset(pathObject,0,sizeof(pathObject));
    memcpy(pathObject,"saveData.lua",12);
    *(unsigned int*)(pathObject+0x10)=12; *(unsigned int*)(pathObject+0x14)=0x0F;
    unsigned char contentObject[64]; memset(contentObject,0,sizeof(contentObject));
    memcpy(contentObject,"AAAABB",6);
    *(unsigned int*)(contentObject+0x10)=6; *(unsigned int*)(contentObject+0x14)=0x0F;
    sobsArm.armed=1; sobsArm.armedAt=GetTickCount64(); sobsArm.ttlMs=600000;
    SobsWindow* built=NULL;
    for(int i=0;i<SOBS_MAX_WINDOWS;++i)
        if(sobsWindow[i].used && !sobsWindow[i].active) { built=&sobsWindow[i]; break; }
    if(!built) printf("writer-probe: no completed window (depth=%d active=%p used0=%d)\n",
                      sobsDepth,(void*)sobsActive,sobsWindow[0].used);
    assert(built);
    built->writerSeen=0; built->writerMatches=0;
    SaveObsWriterPre((unsigned)pathObject,(unsigned)contentObject,frame);
    if(!(built->writerSeen==1 && built->writerBytes==6))
        printf("writer-probe: seen=%d bytes=%u path=%s\n",built->writerSeen,
               built->writerBytes,built->writerPath);
    assert(built->writerSeen==1 && built->writerBytes==6);
    assert(strcmp(built->writerPath,"saveData.lua")==0);
    SaveObsWriterPost(1);
    assert(built->writerAL==1);
    assert(built->writerSha[0] && strcmp(built->payloadSha,built->writerSha)==0);
    assert(built->writerMatches==built->payloadOk);  // offline root is empty: payload not published
    // Consecutive saves must pair Post with the exact Pre, not the first used slot.
    SobsWindow* second=&sobsWindow[1];
    *second=*built; second->saveId=2; second->ebp=frame+4; second->writerSeen=0;
    SaveObsWriterPre((unsigned)pathObject,(unsigned)contentObject,frame+4);
    assert(sobsWriterActive==second && second->writerSeen==1);
    SaveObsWriterPost(0);
    assert(second->writerAL==0 && built->writerAL==1 && sobsWriterActive==NULL);
    std::string eventsBefore=Read(L"native_p03_saveobs_events.jsonl");
    SaveObsWriterPost(1);  // no matching Pre: cannot emit another copy of the old writer
    assert(Read(L"native_p03_saveobs_events.jsonl")==eventsBefore);
    // Capacity is a reported stop; no ring slot or old payload may be overwritten.
    for(int i=0;i<SOBS_MAX_WINDOWS;++i) sobsWindow[i].used=1;
    unsigned firstId=sobsWindow[0].saveId;
    assert(SobsNewWindow()==NULL && sobsLimited==1 && sobsWindow[0].saveId==firstId);
    sobsArm.armed=0;
    root.clear();
    // Nested builder: the inner call must not create a second window and marks the outer one.
    memset(sobsWindow,0,sizeof(sobsWindow));
    sobsDepth=0; sobsActive=NULL; sobsSaveId=0;
    sobsArm.armed=1; sobsArm.armedAt=GetTickCount64(); sobsArm.ttlMs=600000;
    sobsBuilderEbp=frame; sobsBuilderThis=0x1234; sobsBuilderOut=frame-0x28; sobsBuilderFlag=1;
    SaveObsBuilderPre();
    SobsWindow* outer=sobsActive;
    assert(outer && outer->active);
    SaveObsBuilderPre();
    assert(sobsActive==outer && sobsDepth==2);
    SaveObsBuilderPost();
    assert(sobsActive==outer && outer->nested==1 && outer->active);
    SaveObsBuilderPost();
    assert(!outer->active && sobsActive==NULL && sobsDepth==0);
    // Payload above the 1 MiB cap is published as truncated, never as a complete save text.
    memset(sobsWindow,0,sizeof(sobsWindow));
    sobsActive=NULL; sobsSaveId=0; sobsDepth=0;
    sobsBuilderEbp=frame;
    SaveObsBuilderPre();
    SobsWindow* capped=sobsActive;
    assert(capped);
    *(unsigned int*)(frame-0x28+0x10)=2u*1024u*1024u;
    *(unsigned int*)(frame-0x28+0x14)=0x0F;
    SaveObsBuilderPost();
    assert(capped->truncated==1 && capped->payloadOk==0);
    assert(capped->payloadBytes==2u*1024u*1024u);
    // Two boards in one save become two map occurrences, each capped at 64 cells.
    memset(sobsWindow,0,sizeof(sobsWindow));
    sobsActive=NULL; sobsSaveId=0; sobsDepth=0;
    sobsBuilderEbp=frame;
    SaveObsBuilderPre();
    SobsWindow* twoMaps=sobsActive;
    assert(twoMaps);
    *(unsigned int*)(frame-0x58)=0; *(unsigned int*)(frame-0x64)=0;
    *(unsigned char*)(frame-0x4D)=1; *(unsigned int*)(frame-0x48)=board;
    *(unsigned int*)(board+0x50)=(unsigned int)sobsBoardBuffer;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        *(unsigned int*)(frame-0x58)=x; *(unsigned int*)(frame-0x64)=y;
        SaveObsMapCell(frame,tile);
    }
    assert(twoMaps->maps==1 && twoMaps->map[0].cells==64);
    *(unsigned int*)(frame-0x48)=board+0x100;   // a different Board object
    *(unsigned int*)(frame-0x58)=0; *(unsigned int*)(frame-0x64)=0;
    SaveObsMapCell(frame,tile);
    assert(twoMaps->maps==2 && twoMaps->map[1].cells==1);
    SaveObsBuilderPost();
    // A1: 32 consecutive saves, then the 33rd must stop on capacity. Each save keeps its own
    // id, its own published file and its own writer binding; a repeated frame never reuses a
    // previous Post, and the retained 32 files are never overwritten.
    root=L"sobs_abi_scratch";  // payload publishing needs a writable observation root
    for(int i=1;i<=33;++i){
        wchar_t stale[96];
        swprintf_s(stale,L"sobs_abi_scratch\\native_p03_saveobs_payload_%d.lua",i);
        DeleteFileW(stale);
    }
    memset(sobsWindow,0,sizeof(sobsWindow));
    sobsActive=NULL; sobsSaveId=0; sobsDepth=0;
    sobsLimited=0; sobsDeficient=0;
    sobsWindowsUsed=0; sobsWindowsDropped=0;
    sobsEventsWritten=0; sobsEventsDropped=0;
    sobsArm.armed=1; sobsArm.armedAt=GetTickCount64(); sobsArm.ttlMs=600000;
    strcpy_s(sobsArm.obsNonce,"0123456789ABCDEF0123456789ABCDEF");
    sobsBuilderThis=0x1234; sobsBuilderFlag=1;
    for(int i=0;i<32;++i){
        int callsBefore=fakeSobsBuilderCalls;
        fakeSobsVariant=i+1;
        RunSobsBuilder();  // distinct short strings and lengths through the actual thunk
        assert(fakeSobsBuilderCalls==callsBefore+1);
        assert(sobsWindowsUsed==(i+1));
        SobsWindow* window=&sobsWindow[i];
        assert(window->used && !window->active && window->saveId==(unsigned int)(i+1));
        unsigned expectedLength=6+((i+1)&3);
        std::string expected="AAAABBDDDD";
        expected[0]=(char)(0x40+i+1); expected.resize(expectedLength);
        assert(window->payloadOk==1 && window->payloadBytes==expectedLength);
        memset(contentObject,0,sizeof(contentObject));
        memcpy(contentObject,expected.data(),expectedLength);
        *(unsigned int*)(contentObject+0x10)=expectedLength;
        *(unsigned int*)(contentObject+0x14)=0x0F;
        SaveObsWriterPre((unsigned)pathObject,(unsigned)contentObject,frame);
        assert(sobsWriterActive==window && window->writerSeen==1);
        unsigned expectedAL=(i==15)?0:1;
        SaveObsWriterPost(expectedAL);
        assert(window->writerAL==expectedAL && window->writerBytes==expectedLength);
        assert(strcmp(window->payloadSha,window->writerSha)==0);
        assert(std::string(window->payloadSha)==HashBuffer((const unsigned char*)expected.data(),expectedLength));
        if(i==14){
            for(int k=0;k<15;++k){
                wchar_t name[96];
                swprintf_s(name,L"sobs_abi_scratch\\native_p03_saveobs_payload_%d.lua",k+1);
                std::string data; assert(ReadFileBytes(name,data));
                assert(HashBuffer((const unsigned char*)data.data(),(unsigned int)data.size())
                       ==std::string(sobsWindow[k].payloadSha));
            }
        }
    }
    assert(sobsWindowsUsed==32 && sobsWindowsDropped==0);
    for(int i=0;i<32;++i){
        wchar_t name[96];
        swprintf_s(name,L"sobs_abi_scratch\\native_p03_saveobs_payload_%d.lua",i+1);
        std::string data; assert(ReadFileBytes(name,data));
        assert(HashBuffer((const unsigned char*)data.data(),(unsigned int)data.size())
               ==std::string(sobsWindow[i].payloadSha));
    }
    unsigned int keptBytes=sobsWindow[31].payloadBytes;
    std::string keptSha=sobsWindow[31].payloadSha;
    int callsBefore=fakeSobsBuilderCalls;
    RunSobsBuilder();  // the 33rd save: no window, no new payload, no overwrite
    assert(fakeSobsBuilderCalls==callsBefore+1);
    assert(sobsWindowsDropped==1 && sobsLimited==1 && sobsWindowsUsed==32);
    assert(sobsWindow[31].payloadBytes==keptBytes && keptSha==sobsWindow[31].payloadSha);
    for(int i=0;i<32;++i){
        wchar_t name[96];
        swprintf_s(name,L"sobs_abi_scratch\\native_p03_saveobs_payload_%d.lua",i+1);
        std::string retained; assert(ReadFileBytes(name,retained));
        assert(HashBuffer((const unsigned char*)retained.data(),(unsigned int)retained.size())
               ==std::string(sobsWindow[i].payloadSha));
    }
    assert(GetFileAttributesW(L"sobs_abi_scratch\\native_p03_saveobs_payload_33.lua")==INVALID_FILE_ATTRIBUTES);
    std::string eventText;
    assert(ReadFileBytes(root + L"\\native_p03_saveobs_events.jsonl", eventText));
    assert(eventText.find("\"reason\":\"payload_capacity\"")!=std::string::npos);
    assert(eventText.find("\"capacity\":32")!=std::string::npos);
    std::string statusText=SobsStatus("published","test");
    assert(statusText.find("\"observation_contract\":\"bounded32-v1\"")!=std::string::npos);
    assert(statusText.find("\"windows_used\":32")!=std::string::npos);
    assert(statusText.find("\"windows_dropped\":1")!=std::string::npos);
    assert(statusText.find("\"state\":\"partial\"")!=std::string::npos);
    assert(statusText.find("\"arm_counters\":{\"valid\":0")!=std::string::npos ||
           statusText.find("\"arm_counters\":{\"valid\":1")!=std::string::npos);
    long droppedBefore=sobsEventsDropped;
    sobsEvents=SOBS_MAX_EVENTS;
    assert(!SobsEvent("{\"t\":\"map\"}"));  // past the cap: reported, never silent
    assert(sobsEventsDropped==droppedBefore+1);
    // A second valid control cannot reset counters or attach old payloads to a new nonce.
    long usedBefore=sobsWindowsUsed;
    std::string nonceBefore=sobsArm.obsNonce, hashBefore=sobsControlHash;
    std::string nextControl=controlLine(session,"600000");
    nextControl.replace(nextControl.find(std::string(32,'C')),32,std::string(32,'F'));
    writeControl(nextControl);
    assert(!SobsArmRead() && !sobsArm.armed && sobsDeficient);
    assert(sobsWindowsUsed==usedBefore && sobsWindowsDropped==1);
    assert(nonceBefore==sobsArm.obsNonce && hashBefore==sobsControlHash);
    std::string rejected; assert(ReadFileBytes(root+L"\\native_p03_saveobs_status.txt",rejected));
    assert(rejected.find("rearm_rejected")!=std::string::npos);
    assert(!SobsArmRead());  // unchanged rejected control is not republished on every poll
    // A failed status publish must remain deficient after the file becomes writable again.
    std::wstring blockedStatus=root+L"\\native_p03_saveobs_status.txt.tmp";
    assert(CreateDirectoryW(blockedStatus.c_str(),NULL));
    sobsDeficient=0;
    long errorsBefore=sobsPublishErrors;
    SobsStatus("published","blocked_status");
    assert(sobsDeficient && sobsPublishErrors==errorsBefore+1);
    assert(RemoveDirectoryW(blockedStatus.c_str()));
    statusText=SobsStatus("published","status_recovered");
    assert(statusText.find("\"state\":\"partial\"")!=std::string::npos);
    fakeSobsVariant=0;
    sobsEvents=0;
    sobsArm.armed=0;
    puts("PASS save-path observation ABI: wrappers forward arguments/returns and keep ESP; writer Pre/Post bind the exact frame once; bounded32 keeps 32 payload files, the 33rd drops visibly; join mov and unreadable fields checked");
    puts("PASS gateA acceptance: 32 distinct payloads and lengths, mixed writer AL, all retained files survive overflow; re-arm rejected and failed status publish stays deficient");
}
#endif
int main(){
    unsigned int fields[8]={0};unsigned int before=0,after=0,value=0,ecxAfter=99;
    continueA=(void*)FakeTail;
    __asm{
        mov before,esp
        lea ecx,fields
        call TrampolineA
        mov value,eax
        mov ecxAfter,ecx
        mov after,esp
    }
    assert(before==after && value==0x13579BDF && ecxAfter==0 && testThis==(unsigned int)fields && visits==1);
    memcpy(argument,"p01_A123.lua",12);*(unsigned int*)(argument+16)=12;*(unsigned int*)(argument+20)=15;
    currentThis=(unsigned int)fields;saveFn=(void*)FakeSave;loadFn=(void*)FakeLoad;
    __asm{mov before,esp}
    NativeSave();
    __asm{mov after,esp}
    assert(before==after && testCalls==1 && testSize==12 && testCap==15 && testFlag==0 && testThis==currentThis);
    nativeAL=-1;testAL=1;NativeLoad();assert(nativeAL==1);
    nativeAL=-1;testAL=0;NativeLoad();assert(nativeAL==0 && testCalls==3);
    __asm{mov after,esp}
    assert(before==after);
    // T03 capture at 0x60A1D8: copy path/content before the original write.
    action.phase=slnative::Running; actionTid=GetCurrentThreadId(); action.request.kind="save";
    realWriteFn=(void*)FakeCaptureWrite;
    captureCount=0; captureAL=-1; capturePath.clear(); captureText.clear();
    fakeCaptureCalls=0; fakeCaptureAL=1;
    CallThunkCapture("p01_CD78.lua","abc");
    assert(fakeCaptureCalls==1 && captureCount==1);
    assert(capturePath=="p01_CD78.lua" && captureText=="abc" && captureAL==1);
    action.phase=slnative::Idle; captureCount=0; fakeCaptureCalls=0;
    CallThunkCapture("p01_CD78.lua","abc");
    assert(fakeCaptureCalls==1 && captureCount==0);
    // T03 prepare: write + readback + hash, write failure.
    realMalloc=(void*)FixtureMalloc; realFree=(void*)FixtureFree;
    for(size_t length : {0u,15u,16u,4094u,4095u,4096u,32094u}){
        GStringValue value;
        std::string text(length,'x');
        SetValue(&value,text);
        assert(FixtureValueValid(value));
        assert(value.raw[4]==length && memcmp(ValueData(&value),text.data(),length)==0);
        assert(ValueData(&value)[length]==0);
        FreeValue(&value);
        assert(!fixtureAllocation && !fixtureRaw);
    }
    realMalloc=(void*)malloc; realFree=(void*)free;
    realWriteFn=(void*)FakePrepareWrite; realOpenFn=(void*)FakeOpen;
    realSizeFn=(void*)FakeSize; realReadFn=(void*)FakeRead; realCloseFn=(void*)FakeClose;
    realRootFn=(void*)FakeRoot;realProfileFn=(void*)FakeProfile;
    realBackendFn=(void*)FakeBackend;realRelativeFn=(void*)FakeRelative;
    sourceProfile="Alpha";sourceReady=false;
    fakeProfilePrefix=std::string(fixtureUserRoot)+"profile_Alpha\\";
    origExistsFn=(void*)FakePrepareExists;fakeGateAL=0;
    fakePreparedContent.clear(); fakeWriteAL=1;
    fakeOpenCalls=fakeReadCalls=fakeCloseCalls=0;
    bool prepOk=PrepareSourceText("p01_CD78.lua",std::string(100,'x'));
    assert(prepOk && fakePreparedContent.size()==100);
    assert(fakeOpenCalls==1 && fakeReadCalls==1 && fakeCloseCalls==1);
    fakeGateAL=0;
    assert(PrepareSourceText("p01_CD78.lua",std::string(32094,'x')));
    assert(fakePreparedPath==fakeProfilePrefix+"p01_CD78.lua" && fakeOpenedPath==fakePreparedPath);
    assert(fakeQueriedPaths.back()==fakePreparedPath);
    fakeGateAL=0;
    fakeWriteAL=0x12345600;
    assert(!PrepareSourceText("p01_CD78.lua","abc"));
    fakeWriteAL=0x12345601;
    fakeGateAL=0;
    assert(PrepareSourceText("p01_CD78.lua","abc"));
    // Prepare-only transfer: same canonical target as future source-load preparation.
    DeleteFileA("p01_AB12.lua"); DeleteFileA("p01_CD78.lua");
    origExistsFn=(void*)FakePrepareExists;
    fakeGateCalls=0; fakeGateAL=0; fakeWriteAL=1; fakeReadUseOverride=false; fakeWriteWrong=false;
    fakePreparedContent.clear();
    PrepareEvidence prepEv;
    assert(PrepareTransfer("p01_AB12.lua",std::string(100,'x'),prepEv));
    assert(prepEv.writeAL==1 && prepEv.readSize==100 && prepEv.contentSize==100);
    assert(prepEv.readHash.size()==64 && prepEv.readHash==prepEv.contentHash && prepEv.ok);
    assert(prepEv.existsBefore==0 && prepEv.existsAfter==1);
    assert(fakeGateCalls==2);
    assert(prepEv.path==fakeProfilePrefix+"p01_AB12.lua" && prepEv.key=="profile_Alpha\\p01_AB12.lua" && prepEv.backend==1);
    assert(fakeQueriedPaths[fakeQueriedPaths.size()-2]==prepEv.path && fakeQueriedPaths.back()==prepEv.path);
    assert(fakePreparedPath==prepEv.path && fakeOpenedPath==prepEv.path);
    DeleteFileA("p01_AB12.lua");
    fakeGateAL=1; fakePreparedContent.clear();
    assert(!PrepareTransfer("p01_AB12.lua","abc",prepEv));
    assert(prepEv.existsBefore==1 && prepEv.writeAL==0 && fakePreparedContent.empty());
    fakeGateAL=0; fakeWriteAL=0; fakePreparedContent.clear();
    assert(!PrepareTransfer("p01_AB12.lua","abc",prepEv));
    assert(prepEv.writeAL==0 && !prepEv.ok);
    fakeWriteAL=1; fakeWriteWrong=true;
    assert(!PrepareTransfer("p01_AB12.lua",std::string(100,'x'),prepEv));
    assert(prepEv.writeAL==1 && prepEv.readSize==100 && prepEv.readHash!=prepEv.contentHash && !prepEv.ok);
    fakeWriteWrong=false; DeleteFileA("p01_AB12.lua");
    fakeGateAL=0; fakeWriteMakesEntry=false;
    assert(!PrepareTransfer("p01_AB12.lua",std::string(100,'x'),prepEv));
    assert(prepEv.writeAL==1 && prepEv.existsAfter==0 && !prepEv.ok);
    assert(GetFileAttributesA("p01_AB12.lua")!=INVALID_FILE_ATTRIBUTES); // physical file exists, backend invisible
    fakeGateAL=0; fakeWriteMakesEntry=true; fakeReadUseOverride=true;
    fakeReadOverride=std::string(100,'z');
    assert(!PrepareTransfer("p01_AB12.lua",std::string(100,'x'),prepEv));
    assert(prepEv.existsAfter==1 && prepEv.readHash!=prepEv.contentHash && !prepEv.ok);
    fakeReadUseOverride=false; DeleteFileA("p01_AB12.lua");
    // Reject profile, root classifier and relative-key mismatches before writing.
    for(int badPath=0;badPath<5;++badPath){
        fakeGateAL=0;fakePreparedPath.clear();fakeGateCalls=0;
        fakeProfilePrefix=std::string(fixtureUserRoot)+(badPath==0?"profile_Beta\\":"profile_Alpha\\");
        if(badPath==1)fakeProfilePrefix="profile_Alpha\\";
        fakeBackendAL=badPath==2?0x12345600:0x12345601;
        fakeRelativeWrong=badPath==3;
        std::string name=badPath==4?"profile_Alpha\\p01_AB12.lua":"p01_AB12.lua";
        assert(!PrepareTransfer(name,"abc",prepEv));
        assert(fakePreparedPath.empty() && fakeGateCalls==0 && prepEv.existsBefore==-1);
    }
    fakeBackendAL=1;fakeRelativeWrong=false;fakeProfilePrefix=std::string(fixtureUserRoot)+"profile_Alpha\\";
    fakeGateAL=0;sourceReady=true;
    sourceFileHash=HashBuffer((const unsigned char*)"abc",3);
    assert(PrepareSourceText("p01_AB12.lua","abc"));
    assert(fakePreparedPath==fakeProfilePrefix+"p01_AB12.lua");
    fakeGateAL=0;sourceFileHash=std::string(64,'0');fakePreparedPath.clear();
    assert(!PrepareTransfer("p01_AB12.lua","wrong source",prepEv) && fakePreparedPath.empty());
    sourceReady=false;
    puts("PASS G1 path: native profile prefix + short name; root classification and relative key; prep/source-load share write/query/read path; 32094 bytes; occupied name and physical-only file rejected; wrong profile/root/key/source rejected before write");
    // Request kind parsing and prepare-only arm field rules.
    std::string prepReq=std::string("0123456789ABCDEF01234567")+"|0000000003|prep\n";
    slprobe::Record prepRecord;
    assert(prepReq.size()==41 && slprobe::ParseRequest(prepReq,prepRecord));
    assert(prepRecord.kind=="prep" && prepRecord.seq==3 && IsPrepareKind(prepRecord.kind) && !IsTriggerKind(prepRecord.kind));
    std::string saveReq=std::string("0123456789ABCDEF01234567")+"|0000000004|save\n";
    assert(slprobe::ParseRequest(saveReq,prepRecord) && IsTriggerKind(prepRecord.kind) && !IsPrepareKind(prepRecord.kind));
    std::string badReq=std::string("0123456789ABCDEF01234567")+"|0000000003|prepx\n";
    assert(!slprobe::ParseRequest(badReq,prepRecord));
    std::vector<std::string> prepFields{"S","0000000001","prep","p01_AB12.lua","-",std::string(64,'a')};
    assert(PrepArmFields(prepFields));
    prepFields[4]="C:\\x"; assert(!PrepArmFields(prepFields));
    prepFields[4]="-"; prepFields[5]="short"; assert(!PrepArmFields(prepFields));
    // PublishPrepare line format carries the controller contract for both prep and load.
    session=std::string("0123456789ABCDEF01234567"); action.last=7; action.name="p01_AB12.lua";
    action.request.kind="prep";
    fakeGateAL=0; fakeWriteAL=1; fakeReadUseOverride=false;
    assert(PrepareTransfer("p01_AB12.lua",std::string(100,'x'),prepEv));
    auto prepLine=Split(FormatPrepareLine(prepEv));
    assert(prepLine.size()==12 && prepLine[0]==session && prepLine[1]=="0000000007" && prepLine[2]=="prep");
    assert(prepLine[3]=="p01_AB12.lua" && prepLine[4]=="1" && prepLine[5]=="100" && prepLine[6]=="100");
    assert(prepLine[7]=="0" && prepLine[8]=="1" && prepLine[9]==prepLine[10] && prepLine[9].size()==64 && prepLine[11]=="1");
    DeleteFileA("p01_AB12.lua");
    // A source-load publishes the same evidence under its own kind and seq.
    fakeGateAL=0; fakeWriteAL=1; fakePreparedContent.clear();
    assert(PrepareTransfer("p01_AB12.lua",std::string(32094,'x'),prepEv));
    action.request.kind="load";
    auto loadLine=Split(FormatPrepareLine(prepEv));
    assert(loadLine.size()==12 && loadLine[1]=="0000000007" && loadLine[2]=="load");
    assert(loadLine[5]=="32094" && loadLine[6]=="32094" && loadLine[11]=="1");
    DeleteFileA("p01_AB12.lua");
    action.request.kind="save";
    // Capture completeness: one successful write of this transfer name; stale files removed.
    root=L"."; action.last=9; action.name="p01_CD78.lua";
    capturePath="C:\\Users\\x\\profile_Alpha\\p01_CD78.lua";
    captureText=std::string(32094,'c'); captureCount=1; captureAL=1;
    PublishCapture();
    assert(capturePublished);
    std::string metaText; slprobe::ReadRecord(L"native_capture_meta.txt",metaText);
    auto metaFields=Split(metaText);
    assert(metaFields.size()==4 && metaFields[1]=="0000000009" && metaFields[2]=="1" && metaFields[3]=="1");
    assert(GetFileAttributesA("native_capture.lua")!=INVALID_FILE_ATTRIBUTES);
    // A failed publish must not pair readable old text with fresh success metadata.
    HANDLE heldCapture=CreateFileA("native_capture.lua",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    assert(heldCapture!=INVALID_HANDLE_VALUE);
    action.last=10;captureText=std::string(32094,'z');
    PublishCapture();
    assert(!capturePublished);
    assert(GetFileAttributesA("native_capture_meta.txt")==INVALID_FILE_ATTRIBUTES);
    CloseHandle(heldCapture);
    action.last=9;captureText=std::string(32094,'c');PublishCapture();
    assert(capturePublished);
    captureCount=2; PublishCapture();
    assert(!capturePublished);
    assert(GetFileAttributesA("native_capture.lua")==INVALID_FILE_ATTRIBUTES);
    assert(GetFileAttributesA("native_capture_meta.txt")==INVALID_FILE_ATTRIBUTES);
    captureCount=1; capturePath="p01_ZZZZ.lua"; PublishCapture();
    assert(!capturePublished && GetFileAttributesA("native_capture.lua")==INVALID_FILE_ATTRIBUTES);
    captureText.clear(); capturePath.clear(); captureCount=0; captureAL=-1;
    DeleteFileA("native_probe.log");
    root.clear(); action.phase=slnative::Idle;
    // Exists-gate wrapper: both CALL sites observed, original callee called once.
    action.phase=slnative::Running; actionTid=GetCurrentThreadId(); action.last=1;
    origExistsFn=(void*)FakeExists;
    FakeSSO gateMain=GateSSO("p01_CD78.lua");
    fakeGateCalls=0; fakeGateAL=1; ResetGates();
    CallThunkMain(&gateMain,0);
    assert(gateESPBefore==gateESPAfter && fakeGateCalls==1);
    assert(fakeGateECX==(unsigned int)&gateMain && (fakeGateEDX&0xFF)==0);
    assert((gateRet&0xFF)==1 && gateRecords[0].written && gateRecords[0].complete);
    assert(strcmp(gateRecords[0].text,"p01_CD78.lua")==0 && gateRecords[0].dl==0 && gateRecords[0].al==1);
    assert(!gateRecords[1].written);
    FakeSSO gateEmpty=GateSSO("");
    fakeGateCalls=0; fakeGateAL=0; ResetGates();
    CallThunkBackup(&gateEmpty,0);
    assert(fakeGateCalls==1 && (gateRet&0xFF)==0);
    assert(gateRecords[1].written && gateRecords[1].readable && gateRecords[1].complete);
    assert(gateRecords[1].text[0]==0 && gateRecords[1].len==0 && !gateRecords[0].written);
    std::string heapText(76,'p');
    FakeHeap gateHeap={heapText.data(),{0,0,0},(unsigned int)heapText.size(),(unsigned int)heapText.capacity()};
    fakeGateCalls=0; fakeGateAL=1; ResetGates();
    CallThunkMain(&gateHeap,0);
    assert(fakeGateCalls==1 && gateRecords[0].written && gateRecords[0].heap);
    assert(gateRecords[0].readable && !gateRecords[0].truncated && gateRecords[0].complete);
    assert(gateRecords[0].len==76 && strcmp(gateRecords[0].text,heapText.c_str())==0);
    std::string longText(300,'q');
    FakeHeap gateLong={longText.data(),{0,0,0},(unsigned int)longText.size(),(unsigned int)longText.capacity()};
    fakeGateCalls=0; fakeGateAL=0; ResetGates();
    CallThunkBackup(&gateLong,0);
    assert(fakeGateCalls==1 && gateRecords[1].heap && gateRecords[1].readable && gateRecords[1].truncated);
    assert(gateRecords[1].len==300 && strlen(gateRecords[1].text)==256 && (gateRet&0xFF)==0);
    FakeHeap gateNull={nullptr,{0,0,0},10,31};
    fakeGateCalls=0; fakeGateAL=1; ResetGates();
    CallThunkMain(&gateNull,1);
    assert(fakeGateCalls==1 && gateRecords[0].written && !gateRecords[0].readable && !gateRecords[0].complete);
    assert(gateRecords[0].dl==1);
    FakeHeap gateEmptyHeap={nullptr,{0,0,0},0,31};
    fakeGateCalls=0; fakeGateAL=1; ResetGates();
    CallThunkMain(&gateEmptyHeap,0);
    assert(fakeGateCalls==1 && gateRecords[0].heap && gateRecords[0].readable && gateRecords[0].text[0]==0);
    action.phase=slnative::Idle;
    fakeGateCalls=0; ResetGates();
    CallThunkMain(&gateMain,0);
    assert(fakeGateCalls==1 && !gateRecords[0].written && !gateRecords[1].written);
    action.phase=slnative::Running; actionTid=GetCurrentThreadId();
    origExistsFn=(void*)FakeExistsNested; nestCalls=0; fakeGateAL=1; ResetGates();
    CallThunkMain(&gateMain,0);
    assert(nestCalls==2 && gateNested==1 && gateRecords[0].written);
    assert(strcmp(gateRecords[0].text,"p01_CD78.lua")==0);
    // Tree-lookup wrapper: fake map/key, read-only record, original lookup once.
    FakeMap findMap={(void*)0x11111111};
    FakeNode findNode; FakeEntry findEntry; findNode.entry=&findEntry; findEntry.state=2;
    FakeSSO findKeyObj=GateSSO("profile_Alpha");
    findTrampoline=(void*)FakeFind; fakeFindCalls=0; fakeFindNode=(unsigned int)&findNode;
    action.phase=slnative::Running; actionTid=GetCurrentThreadId(); action.last=1;
    ResetFind();
    CallThunkFind(&findMap,&findKeyObj);
    assert(fakeFindCalls==1 && fakeFindKey==(unsigned int)&findKeyObj);
    assert(findCount==1 && findRecords[0].written && !findRecords[0].isEnd);
    assert(strcmp(findRecords[0].text,"profile_Alpha")==0);
    assert(findRecords[0].entry==(unsigned int)&findEntry && findRecords[0].state==2);
    assert(inFind==0);
    fakeFindCalls=0; fakeFindNode=(unsigned int)findMap.end; ResetFind();
    CallThunkFind(&findMap,&findKeyObj);
    assert(fakeFindCalls==1 && findCount==1 && findRecords[0].isEnd && findRecords[0].state==0);
    std::string findHeapText(32,'k');
    FakeHeap findHeapObj={findHeapText.data(),{0,0,0},(unsigned int)findHeapText.size(),(unsigned int)findHeapText.capacity()};
    fakeFindCalls=0; fakeFindNode=(unsigned int)&findNode; ResetFind();
    CallThunkFind(&findMap,&findHeapObj);
    assert(findCount==1 && findRecords[0].heap && findRecords[0].readable);
    assert(strcmp(findRecords[0].text,findHeapText.c_str())==0);
    fakeFindCalls=0; ResetFind();
    CallThunkFind(&findMap,(void*)1);
    assert(fakeFindCalls==1 && findCount==1 && !findRecords[0].readable);
    action.phase=slnative::Idle;
    findKey=0xDEADBEEF; findCallerRet=0xCAFEBABE;
    fakeFindCalls=0; ResetFind();
    CallThunkFind(&findMap,&findKeyObj);
    assert(fakeFindCalls==1 && findCount==0 && inFind==0);
    assert(findKey==0xDEADBEEF && findCallerRet==0xCAFEBABE);
    action.phase=slnative::Running; actionTid=GetCurrentThreadId();
    FakeSSO nestedText=GateSSO("p01_CD78.lua");
    nestedKey=(unsigned int)&nestedText; nestedMap=(unsigned int)&findMap; nestedNode=(unsigned int)&findNode;
    findTrampoline=(void*)FakeFindNested; fakeNestedCalls=0; ResetFind();
    CallThunkFind(&findMap,&findKeyObj);
    assert(fakeNestedCalls==2 && findCount==1 && inFind==0);
    assert(fakeFindKey==(unsigned int)&findKeyObj && strcmp(findRecords[0].text,"profile_Alpha")==0);
    findTrampoline=(void*)FakeFind; fakeFindNode=(unsigned int)&findNode; fakeFindCalls=0; ResetFind();
    for(int i=0;i<17;++i) CallThunkFind(&findMap,&findKeyObj);
    assert(fakeFindCalls==17 && findCount==17 && findOverflow==1 && findRecords[15].written);
    // Entry-check wrapper: fake entry/vtable, check called once, al forwarded.
    FakeCheckVtable checkVtable={nullptr,(void*)FakeCheck};
    FakeCheckEntry checkEntry={&checkVtable};
    fakeCheckCalls=0; fakeCheckThis=0; fakeCheckAL=1;
    action.phase=slnative::Running; actionTid=GetCurrentThreadId(); action.last=1;
    ResetEntryCheck();
    CallThunkEntryCheck(&checkEntry);
    CallThunkEntryCheck(&checkEntry);
    assert(fakeCheckCalls==2 && fakeCheckThis==(unsigned int)&checkEntry);
    assert(entryCheckCount==2 && entryCheckRecords[0].written && entryCheckRecords[1].written);
    assert(entryCheckRecords[0].al==1 && entryCheckRecords[1].al==1);
    assert(entryCheckRecords[0].entry==(unsigned int)&checkEntry);
    assert(entryCheckRecords[0].vtable==(unsigned int)&checkVtable);
    assert(entryCheckRecords[0].checkFn==(unsigned int)FakeCheck);
    action.phase=slnative::Idle;
    fakeCheckCalls=0; ResetEntryCheck();
    CallThunkEntryCheck(&checkEntry);
    assert(fakeCheckCalls==1 && entryCheckCount==0);
    // File-entry-check wrapper: fake request frame, entry/vtable/check, request+4 before/after.
    FakeCheckVtable testFileVtable={nullptr,(void*)FakeFileCheck};
    FakeCheckEntry testFileEntry={&testFileVtable};
    unsigned int requestWords[4]={0x008214D8,0xAB,0,0};
    fileResume=(void*)FakeFileResume;
    action.phase=slnative::Running; actionTid=GetCurrentThreadId(); action.last=1;
    fakeFileCheckCalls=0; fakeFileCheckThis=0; fakeFileCheckAL=1; gateClobberECX=true;
    ResetFileCheck();
    CallThunkFileCheck(&testFileEntry,&requestWords,0x11111111,0x22222222);
    assert(fakeFileCheckCalls==1 && fakeFileCheckThis==(unsigned int)&testFileEntry);
    assert(fileCheckCount==1 && fileCheckRecords[0].written);
    assert(fileCheckRecords[0].request==(unsigned int)&requestWords);
    assert(fileCheckRecords[0].entry==(unsigned int)&testFileEntry);
    assert(fileCheckRecords[0].vtable==(unsigned int)&testFileVtable);
    assert(fileCheckRecords[0].checkFn==(unsigned int)FakeFileCheck);
    assert(fileCheckRecords[0].al==1);
    assert(fileCheckRecords[0].req4Before==0xAB && fileCheckRecords[0].req4After==0xAB);
    assert(fileCheckRecords[0].parent==0x11111111 && fileCheckRecords[0].key==0x22222222);
    assert(fileESPAfter+4==fileESPBefore);
    requestWords[1]=0; fakeFileCheckCalls=0; fakeFileCheckAL=0; ResetFileCheck();
    CallThunkFileCheck(&testFileEntry,&requestWords,0,0);
    assert(fakeFileCheckCalls==1 && fileCheckCount==1);
    assert(fileCheckRecords[0].al==0 && fileCheckRecords[0].req4Before==0 && fileCheckRecords[0].req4After==0);
    assert(requestWords[1]==1);
    action.phase=slnative::Idle;
    fakeFileCheckCalls=0; ResetFileCheck();
    CallThunkFileCheck(&testFileEntry,&requestWords,0,0);
    assert(fakeFileCheckCalls==1 && fileCheckCount==0);
    action.phase=slnative::Running; actionTid=GetCurrentThreadId();
    fakeFileCheckCalls=0; ResetFileCheck();
    for(int i=0;i<9;++i) CallThunkFileCheck(&testFileEntry,&requestWords,0,0);
    assert(fakeFileCheckCalls==9 && fileCheckCount==9 && fileCheckOverflow==1 && fileCheckRecords[7].written);
    gateClobberECX=false;
    // File-entry branch wrapper: 0x46D4D0 entry, cmp, key, state.
    feResume=(void*)FakeEntryResume;
    fcmpResumeHit=(void*)FakeCmpResumeHit;
    fcmpResumeMiss=(void*)FakeCmpResumeMiss;
    fkeyResume=(void*)FakeKeyResume;
    fstateResume=(void*)FakeStateResume;
    action.phase=slnative::Running; actionTid=GetCurrentThreadId(); action.last=1;
    ResetFileBranch();
    CallThunkFileEntry(&requestWords,0x11111111,0x22222222);
    assert(fbCount==1 && fbRecords[0].written && fbRecords[0].kind==0);
    assert(fbRecords[0].request==(unsigned int)&requestWords);
    assert(fbRecords[0].arg1==0x11111111 && fbRecords[0].arg2==0x22222222);
    assert(feESPAfter+12==feESPBefore);
    ResetFileBranch();
    CallThunkFileCmp(0x1000,0x1000);
    assert(fbCount==1 && fbRecords[0].kind==1 && fbRecords[0].hit==1);
    assert(fbRecords[0].node==0x1000 && fbRecords[0].sentinel==0x1000);
    assert(fcmpESPAfter+4==fcmpESPBefore);
    ResetFileBranch();
    CallThunkFileCmp(0x1000,0x2000);
    assert(fbCount==1 && fbRecords[0].kind==1 && fbRecords[0].hit==0);
    ResetFileBranch();
    CallThunkFileKey(0x1000,0,0x2000);
    assert(fbCount==1 && fbRecords[0].kind==2 && fbRecords[0].al==0);
    assert(fkeyESIAfter==0x1000);
    ResetFileBranch();
    CallThunkFileKey(0x1000,1,0x2000);
    assert(fbCount==1 && fbRecords[0].kind==2 && fbRecords[0].al==1);
    assert(fkeyESIAfter==0x2000);
    assert(fkeyESPAfter+4==fkeyESPBefore);
    fakeNode[10]=(unsigned int)fakeEntry;
    fakeEntry[8]=2;
    ResetFileBranch();
    CallThunkFileState((unsigned int)fakeNode);
    assert(fbCount==1 && fbRecords[0].kind==3);
    assert(fbRecords[0].entry==(unsigned int)fakeEntry && fbRecords[0].state==2);
    assert(fstateEAX==2 && fstateECX==(unsigned int)fakeEntry);
    assert(fstateESPAfter+4==fstateESPBefore);
    action.phase=slnative::Idle;
    ResetFileBranch();
    CallThunkFileEntry(&requestWords,0,0);
    assert(fbCount==0);
    action.phase=slnative::Running;
    ResetFileBranch();
    for(int i=0;i<17;++i) CallThunkFileEntry(&requestWords,0,0);
    assert(fbCount==17 && fbOverflow==1 && fbRecords[15].written);
    TestPatchedFileMethod();
    TestKeySnapshots();TestKeyPairing();
#ifdef ITB_SL_SAVEOBS_DIAGNOSTIC
    TestSaveObsAbi();
#endif
#ifdef ITB_SL_COLD_DIAGNOSTIC
    assert(ColdOnceAllowed("load","p01_CD78.lua",coldHash,0));
    assert(!ColdOnceAllowed("save","p01_CD78.lua",coldHash,0));
    assert(!ColdOnceAllowed("load","p01_CD79.lua",coldHash,0));
    assert(!ColdOnceAllowed("load","p01_CD78.lua",std::string(64,'0'),0));
    assert(!ColdOnceAllowed("load","p01_CD78.lua",coldHash,1));
#endif
    puts("PASS actual x86 assembly: update-tail wrapper preserves stack/return/this; save/load use fake callees with ret28/ret24 and fresh AL; exists-gate thunks call the original once, keep ECX/DL/AL, read bounded SSO and heap strings, and mark truncation, non-calling and nested cases; tree-lookup thunk passes non-calling and nested calls through without touching return addresses or globals; file-entry-check thunk calls entry vtable+4 once with ret 0, forwards AL, reads request words before/after, records request+4 changes, and keeps the stack balanced; file-entry branch wrappers record 0x46D4D0 entry/cmp/key/state, forward flags and registers, and keep the stack balanced; capture/prepare adapters copy path/content before the original write, forward write AL, and read back hash through fake handlers; prepare-only transfer checks entry occupancy before writing, forwards write AL, records readback size/hash and formats the controller result line");return 0;
}
#endif

