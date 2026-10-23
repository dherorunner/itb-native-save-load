// P04 v2: Lua calls only enqueue. The update-tail OnA owns engine operations.
#include "p04_service.h"
#include <stddef.h>
#include <stdint.h>

// SDL2's public x86 ABI, restricted to the two fixed Escape events we need.
// No SDL library is loaded here and this bridge does not change keyboard state.
struct P04SdlKeysym { int32_t scancode,sym;uint16_t mod;uint32_t unused; };
struct P04SdlKeyboardEvent {
    uint32_t type,timestamp,windowID;
    uint8_t state,repeat,padding2,padding3;
    P04SdlKeysym keysym;
};
union P04SdlEvent { uint32_t type;P04SdlKeyboardEvent key;uint8_t padding[56]; };
struct P04SdlVersion { uint8_t major,minor,patch; };
static_assert(sizeof(P04SdlKeysym)==16,"SDL2 keysym ABI");
static_assert(sizeof(P04SdlKeyboardEvent)==32&&sizeof(P04SdlEvent)==56,"SDL2 event ABI");
static_assert(offsetof(P04SdlKeyboardEvent,type)==0&&offsetof(P04SdlKeyboardEvent,timestamp)==4&&
    offsetof(P04SdlKeyboardEvent,windowID)==8&&offsetof(P04SdlKeyboardEvent,state)==12&&
    offsetof(P04SdlKeyboardEvent,repeat)==13&&offsetof(P04SdlKeyboardEvent,keysym)==16,"SDL2 keyboard offsets");
static_assert(offsetof(P04SdlKeysym,sym)==4&&offsetof(P04SdlKeysym,mod)==8&&
    offsetof(P04SdlKeysym,unused)==12,"SDL2 keysym offsets");
#ifdef ITB_SL_P04_OFFLINE_TEST
// Assigned only by the NOHOOK executable; never resolve a real SDL in tests.
static void* (*p04TestSdlResolve)(const char*)=nullptr;
#endif
static void* P04SdlResolve(const char* name) {
#ifdef ITB_SL_P04_OFFLINE_TEST
    return p04TestSdlResolve?p04TestSdlResolve(name):nullptr;
#else
    HMODULE sdl=GetModuleHandleW(L"SDL2.dll");
    return sdl?(void*)GetProcAddress(sdl,name):nullptr;
#endif
}

struct P04Lua {
    void (*createtable)(void*,int,int);
    void (*pushcclosure)(void*,int (*)(void*),int);
    void (*setfield)(void*,int,const char*);
    void (*pushstring)(void*,const char*);
    void (*pushlstring)(void*,const char*,size_t);
    void (*pushinteger)(void*,int);
    void (*pushboolean)(void*,int);
    void (*pushnil)(void*);
    void (*pushvalue)(void*,int);
    const char* (*tolstring)(void*,int,size_t*);
    int (*gettop)(void*);
    void (*settop)(void*,int);
    int (*type)(void*,int);
    int (*toboolean)(void*,int);
    int (*pcall)(void*,int,int,int);
    int (*ref)(void*,int);
    void (*unref)(void*,int,int);
    void (*rawgeti)(void*,int,int);
};
static P04Lua* p04Lua=nullptr;
static void* p04State=nullptr;
static int p04GuardRef=-2;
static volatile LONG p04WorkerStarted=0,p04Ready=0;
static std::string p04Reason="not_initialized";
static bool p04PermitSet=false,p04VerifyRequested=false;
static unsigned long p04PermitSeq=0;
static ULONGLONG p04PermitIssued=0;
static unsigned long p04PermitTtl=0;
static std::string p04PermitKind,p04PermitBattle,p04PermitProfile;
static std::string p04ActionProfile,p04ActionBattle,p04ReceiptHash;
static std::wstring p04Receipt;
static bool p04Failed=false;

static bool P04ResolveLua() {
    if(p04Lua)return true;
    HMODULE runtime=GetModuleHandleW(L"lua5.1.dll");if(!runtime)return false;
    static P04Lua lua{};
#define P04_LUA(field,symbol) lua.field=(decltype(lua.field))GetProcAddress(runtime,symbol);if(!lua.field)return false
    P04_LUA(createtable,"lua_createtable");P04_LUA(pushcclosure,"lua_pushcclosure");
    P04_LUA(setfield,"lua_setfield");P04_LUA(pushstring,"lua_pushstring");
    P04_LUA(pushlstring,"lua_pushlstring");P04_LUA(pushinteger,"lua_pushinteger");
    P04_LUA(pushboolean,"lua_pushboolean");P04_LUA(pushnil,"lua_pushnil");
    P04_LUA(pushvalue,"lua_pushvalue");P04_LUA(tolstring,"lua_tolstring");
    P04_LUA(gettop,"lua_gettop");P04_LUA(settop,"lua_settop");P04_LUA(type,"lua_type");
    P04_LUA(toboolean,"lua_toboolean");P04_LUA(pcall,"lua_pcall");
    P04_LUA(ref,"luaL_ref");P04_LUA(unref,"luaL_unref");P04_LUA(rawgeti,"lua_rawgeti");
#undef P04_LUA
    p04Lua=&lua;return true;
}
static std::string P04Arg(void* L,int index) {
    size_t n=0;const char* value=p04Lua->tolstring(L,index,&n);
    return value?std::string(value,n):std::string();
}
static int P04Err(void* L,const char* code) {p04Lua->pushnil(L);p04Lua->pushstring(L,code);return 2;}
static const char* P04Code(int code) {
    switch(code){
    case p04svc::NotFound:return "not_found";case p04svc::AlreadyExists:return "already_exists";
    case p04svc::PermissionDenied:return "permission_denied";case p04svc::TooLarge:return "too_large";
    case p04svc::BadPath:return "bad_path";case p04svc::NotConfigured:return "not_configured";
    case p04svc::VerifyFailed:return "verify_failed";case p04svc::CommitUncertain:return "commit_uncertain";
    default:return "io_error";
    }
}
static bool P04ProfileOk(const std::string& p) {
    if(p.empty()||p.size()>32)return false;
    for(char c:p)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
    return true;
}
static bool P04Seq(void* L,int arg,unsigned long* value) {
    std::string s=P04Arg(L,arg);if(s.empty()||s.size()>10)return false;
    unsigned long long n=0;for(char c:s){if(c<'0'||c>'9')return false;n=n*10+c-'0';}
    if(n==0||n>2147483647)return false;*value=(unsigned long)n;return true;
}
static bool P04Hash(const std::string& hash) {
    if(hash.size()!=64)return false;
    for(char c:hash)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}
static bool P04Busy(){return action.phase==slnative::Queued||action.phase==slnative::Running||action.phase==slnative::Returned;}

static DWORD P04WorkerInit() {
#ifndef ITB_SL_P04_OFFLINE_TEST
    wchar_t exe[MAX_PATH*4]={};
    if(!GetModuleFileNameW(nullptr,exe,MAX_PATH*4)||Hash(exe)!=exeHash){p04Reason="unsupported_exe";p04Ready=-1;return 1;}
    if(!Hook()){p04Reason="hook_setup_failed";p04Ready=-1;return 1;}
#endif
    p04Reason.clear();InterlockedExchange(&p04Ready,1);return 0;
}
static bool P04StartWorker() {
    if(InterlockedCompareExchange(&p04WorkerStarted,1,0)!=0)return p04Ready==1;
#ifdef ITB_SL_P04_OFFLINE_TEST
    return P04WorkerInit()==0;
#else
    HANDLE thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
    if(!thread){p04Reason="worker_start_failed";p04Ready=-1;return false;}CloseHandle(thread);
    ULONGLONG deadline=GetTickCount64()+5000;
    while(p04Ready==0&&GetTickCount64()<deadline)Sleep(10);
    if(p04Ready==0)p04Reason="worker_not_ready";
    return p04Ready==1;
#endif
}
static bool P04ConfigureStore() {
    if(!p04svc::Configured()) {
        wchar_t base[MAX_PATH*4]={};
        DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH*4);
        if(!n||n>=MAX_PATH*4){p04Reason="no_localappdata";return false;}
        p04svc::Status status{};
        if(!p04svc::Configure(std::wstring(base)+L"\\ITB-SL\\P04",&status)){p04Reason=P04Code(status.code);return false;}
    }
    if(session.empty()) {
        p04svc::Status status{};std::string boot;
        if(!p04svc::RandomHex(12,&boot,&status)){p04Reason="epoch_failed";return false;}
        for(char& c:boot)if(c>='a'&&c<='f')c-=32;
        std::wstring relative=L"ipc\\"+std::wstring(boot.begin(),boot.end());
        if(!p04svc::EnsureDirectory(relative,&status)){p04Reason=P04Code(status.code);return false;}
        root=p04svc::Root()+L"\\"+relative;session=boot;
    }
    return true;
}
static bool P04PermitValid(unsigned long seq,const std::string& kind,const std::string&) {
    return p04Ready==1&&p04PermitSet&&seq==p04PermitSeq&&kind==p04PermitKind&&
        p04PermitProfile==p04Profile&&!p04PermitBattle.empty()&&
        GetTickCount64()-p04PermitIssued<=p04PermitTtl;
}
static void P04Revoke(unsigned long seq){if(seq==p04PermitSeq)p04PermitSet=false;}
static void P04RejectNoCall(unsigned long,const char*,const char* reason) {
    strncpy_s(p04RejectReason,reason?reason:"rejected",_TRUNCATE);
}
static bool P04Reject(const char* reason){P04RejectNoCall(action.last,action.request.kind.c_str(),reason);return false;}
static bool P04BeforeCall() {
    if(!P04PermitValid(action.last,action.request.kind,action.name)||p04ActionProfile!=p04Profile||
       p04ActionBattle!=p04PermitBattle)return P04Reject("permit_invalid");
    if(!p04State||p04GuardRef<0)return P04Reject("guard_missing");
    int top=p04Lua->gettop(p04State);
    p04Lua->rawgeti(p04State,-10000,p04GuardRef);
    p04Lua->pushinteger(p04State,(int)action.last);
    p04Lua->pushstring(p04State,action.request.kind.c_str());
    p04Lua->pushstring(p04State,p04ActionBattle.c_str());
    p04Lua->pushstring(p04State,p04ActionProfile.c_str());
    int error=p04Lua->pcall(p04State,4,1,0);
    bool permit=!error&&p04Lua->type(p04State,-1)==1&&p04Lua->toboolean(p04State,-1)==1;
    p04Lua->settop(p04State,top);
    if(!permit)return P04Reject(error?"guard_error":"guard_rejected");
    // Callback may revoke or change profile; recheck after it returns.
    if(!P04PermitValid(action.last,action.request.kind,action.name)||p04ActionProfile!=p04Profile)return P04Reject("permit_revoked");
    if(GetTickCount64()-queuedAt>5000)return P04Reject("queue_expired");
    sourceProfile=p04ActionProfile;
    PrepareEvidence ev;ResetEvidence(ev);
    if(!ResolvePreparePath(action.name,ev))return P04Reject("engine_path_mismatch");
    target=Utf8ToWide(ev.path);
    DWORD attributes=GetFileAttributesW(target.c_str());
    DWORD errorCode=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
    if(attributes!=INVALID_FILE_ATTRIBUTES)return P04Reject("name_occupied");
    if(errorCode!=ERROR_FILE_NOT_FOUND&&errorCode!=ERROR_PATH_NOT_FOUND)return P04Reject("target_unreadable");
    if(action.request.kind=="save") {
        ev.existsBefore=ExistsValue(ev.path);
        PublishPrepare(ev);
        if(ev.existsBefore!=0)return P04Reject("name_occupied");
    } else {
        if(!sourceReady||!p04svc::IsInsideRoot(sourceFileW)||!p04svc::IsInsideRoot(p04Receipt)||
           Hash(sourceFileW)!=sourceFileHash||Hash(p04Receipt)!=p04ReceiptHash)return P04Reject("source_changed");
    }
    p04PermitSet=false;
    return true;
}

static int P04LuaAvailable(void* L){p04Lua->pushboolean(L,p04Ready==1&&p04svc::Configured());return 1;}
static int P04LuaReason(void* L){p04Lua->pushstring(L,p04Reason.c_str());return 1;}
static int P04LuaExeHash(void* L){p04Lua->pushstring(L,exeHash);return 1;}
static int P04LuaEpoch(void* L){p04Lua->pushstring(L,session.c_str());return 1;}
static int P04LuaRoot(void* L){std::string s=p04svc::Utf8(p04svc::Root());p04Lua->pushlstring(L,s.data(),s.size());return 1;}
static int P04LuaInit(void* L) {
    std::string profile=P04Arg(L,1),exe=P04Arg(L,2);
    if(!P04ProfileOk(profile))return P04Err(L,"bad_profile");
    if(exe!=exeHash)return P04Err(L,"unsupported_exe");
    if(p04State&&p04State!=L)return P04Err(L,"lua_state_changed");
    if(P04Busy()&&profile!=p04Profile){p04PermitSet=false;return P04Err(L,"busy");}
    if(!P04ConfigureStore()||!P04StartWorker())return P04Err(L,p04Reason.c_str());
    if(profile!=p04Profile)p04PermitSet=false;
    p04State=L;p04Profile=profile;p04Lua->pushboolean(L,1);return 1;
}
static int P04LuaRequestEscapeClose(void* L) {
    if(p04Lua->gettop(L)!=0)return P04Err(L,"no_arguments");
    if(L!=p04State)return P04Err(L,"lua_state_changed");
    if(p04Ready!=1||!p04svc::Configured()||p04Profile.empty())return P04Err(L,"not_initialized");
    if(P04Busy())return P04Err(L,"busy");
    auto version=(void (__cdecl *)(P04SdlVersion*))P04SdlResolve("SDL_GetVersion");
    auto focus=(void* (__cdecl *)())P04SdlResolve("SDL_GetKeyboardFocus");
    auto windowID=(uint32_t (__cdecl *)(void*))P04SdlResolve("SDL_GetWindowID");
    auto push=(int (__cdecl *)(P04SdlEvent*))P04SdlResolve("SDL_PushEvent");
    if(!version||!focus||!windowID||!push)return P04Err(L,"sdl_exports_missing");
    P04SdlVersion v{};version(&v);
    if(v.major!=2)return P04Err(L,"sdl_version_unsupported");
    void* window=focus();if(!window)return P04Err(L,"sdl_focus_missing");
    uint32_t id=windowID(window);if(!id)return P04Err(L,"sdl_window_missing");
    P04SdlEvent event{};event.key.type=0x300;event.key.windowID=id;event.key.state=1;
    event.key.keysym.scancode=41;event.key.keysym.sym=27;
    int result=push(&event);
    if(result!=1)return P04Err(L,result==0?"sdl_keydown_filtered":"sdl_keydown_failed");
    event=P04SdlEvent{};event.key.type=0x301;event.key.windowID=id;
    event.key.keysym.scancode=41;event.key.keysym.sym=27;
    result=push(&event);
    if(result!=1)return P04Err(L,result==0?"sdl_partial_keyup_filtered":"sdl_partial_keyup_failed");
    p04Lua->pushboolean(L,1);return 1;
}
static bool P04Relative(void* L,int arg,std::wstring* path){return p04svc::Wide(P04Arg(L,arg),path);}
static int P04LuaRead(void* L) {
    std::wstring path;if(!P04Relative(L,1,&path))return P04Err(L,"bad_utf8");
    std::string limit=P04Arg(L,2);size_t n=limit.empty()?4*1024*1024:(size_t)strtoul(limit.c_str(),nullptr,10);
    if(n>16*1024*1024)return P04Err(L,"too_large");
    p04svc::Status status{};std::string bytes;
    if(!p04svc::Read(path,n,&bytes,&status))return P04Err(L,P04Code(status.code));
    p04Lua->pushlstring(L,bytes.data(),bytes.size());return 1;
}
static int P04LuaWriteExclusive(void* L) {
    std::wstring path;if(!P04Relative(L,1,&path))return P04Err(L,"bad_utf8");
    p04svc::Status status{};
    if(!p04svc::WriteExclusive(path,P04Arg(L,2),&status))return P04Err(L,P04Code(status.code));
    p04Lua->pushboolean(L,1);return 1;
}
static int P04LuaWriteAtomic(void* L) {
    std::wstring path;if(!P04Relative(L,1,&path))return P04Err(L,"bad_utf8");
    p04svc::Status status{};
    if(!p04svc::WriteAtomic(path,P04Arg(L,2),&status))return P04Err(L,P04Code(status.code));
    p04Lua->pushboolean(L,1);return 1;
}
static int P04LuaExists(void* L) {
    std::wstring path;if(!P04Relative(L,1,&path))return P04Err(L,"bad_utf8");
    p04svc::Status status{};bool exists=false;
    if(!p04svc::Exists(path,&exists,&status))return P04Err(L,P04Code(status.code));
    p04Lua->pushboolean(L,exists);return 1;
}
static int P04LuaRemove(void* L) {
    std::wstring path;if(!P04Relative(L,1,&path))return P04Err(L,"bad_utf8");
    p04svc::Status status{};
    if(!p04svc::Remove(path,&status)&&status.code!=p04svc::NotFound)return P04Err(L,P04Code(status.code));
    p04Lua->pushboolean(L,1);return 1;
}
static int P04LuaList(void* L) {
    std::wstring path;if(!P04Relative(L,1,&path))return P04Err(L,"bad_utf8");
    p04svc::Status status{};std::string names;
    if(!p04svc::ListFiles(path,&names,&status))return P04Err(L,P04Code(status.code));
    p04Lua->pushlstring(L,names.data(),names.size());return 1;
}
static int P04LuaSha256(void* L) {
    p04svc::Status status{};std::string hash;
    if(!p04svc::Sha256Hex(P04Arg(L,1),&hash,&status))return P04Err(L,P04Code(status.code));
    p04Lua->pushstring(L,hash.c_str());return 1;
}
static int P04LuaRandomHex(void* L) {
    p04svc::Status status{};std::string hash,arg=P04Arg(L,1);
    if(!p04svc::RandomHex(arg.empty()?8:(unsigned)strtoul(arg.c_str(),nullptr,10),&hash,&status))return P04Err(L,P04Code(status.code));
    p04Lua->pushstring(L,hash.c_str());return 1;
}
static int P04LuaSetGuard(void* L) {
    if(L!=p04State||p04Lua->type(L,1)!=6)return P04Err(L,"bad_guard");
    if(p04GuardRef>=0)p04Lua->unref(L,-10000,p04GuardRef);
    p04Lua->pushvalue(L,1);p04GuardRef=p04Lua->ref(L,-10000);p04Lua->pushboolean(L,1);return 1;
}
static int P04LuaNextSeq(void* L){p04Lua->pushinteger(L,(int)(action.last+1));return 1;}
static int P04LuaPermit(void* L) {
    unsigned long seq=0,ttl=0;std::string kind=P04Arg(L,2),battle=P04Arg(L,3);
    if(!P04Seq(L,1,&seq)||!P04Seq(L,4,&ttl)||ttl>8000||seq!=action.last+1||
       (kind!="save"&&kind!="load")||battle.empty()||battle.size()>1024||p04Ready!=1||p04GuardRef<0||P04Busy())return P04Err(L,"bad_permit");
    p04PermitSeq=seq;p04PermitKind=kind;p04PermitBattle=battle;p04PermitProfile=p04Profile;
    p04PermitTtl=ttl;p04PermitIssued=GetTickCount64();p04PermitSet=true;
    p04Lua->pushboolean(L,1);return 1;
}
static int P04LuaRevoke(void* L){unsigned long seq=0;if(!P04Seq(L,1,&seq))return P04Err(L,"bad_seq");P04Revoke(seq);p04Lua->pushboolean(L,1);return 1;}
static int P04Queue(void* L,const char* kind) {
    unsigned long seq=0;std::string name=P04Arg(L,2);
    if(!P04Seq(L,1,&seq)||!slnative::ValidNameP04(name))return P04Err(L,"bad_request");
    if(!P04PermitValid(seq,kind,name))return P04Err(L,"no_permit");
    if(P04Busy()||seq!=action.last+1)return P04Err(L,"busy");
    std::wstring source,receipt;std::string sourceHash,receiptHash;
    if(strcmp(kind,"load")==0) {
        if(!p04svc::Wide(P04Arg(L,3),&source)||!p04svc::Wide(P04Arg(L,5),&receipt))return P04Err(L,"outside_store");
        for(wchar_t& c:source)if(c==L'/')c=L'\\';
        for(wchar_t& c:receipt)if(c==L'/')c=L'\\';
        if(!p04svc::IsInsideRoot(source)||!p04svc::IsInsideRoot(receipt))return P04Err(L,"outside_store");
        std::wstring profilePrefix=p04svc::Root()+L"\\profiles\\profile_"+std::wstring(p04Profile.begin(),p04Profile.end())+L"\\";
        if(source.rfind(profilePrefix,0)!=0||receipt.rfind(profilePrefix,0)!=0)return P04Err(L,"profile_path_mismatch");
        sourceHash=P04Arg(L,4);receiptHash=P04Arg(L,6);
        if(!P04Hash(sourceHash)||!P04Hash(receiptHash)||Hash(source)!=sourceHash||Hash(receipt)!=receiptHash)return P04Err(L,"source_hash_mismatch");
    }
    sourceReady=strcmp(kind,"load")==0;sourceFileW=source;sourceFileHash=sourceHash;sourceProfile=p04Profile;
    p04Receipt=receipt;p04ReceiptHash=receiptHash;
    p04ActionProfile=p04Profile;p04ActionBattle=p04PermitBattle;p04NativeCalled=0;p04PrepareOk=false;
    p04VerifyRequested=false;p04Failed=false;p04RejectReason[0]=0;
    ResetCapture();target.clear();expectedHash=sourceHash;
    memset(argument,0,24);memcpy(argument,name.data(),name.size());
    *(unsigned int*)(argument+16)=(unsigned int)name.size();*(unsigned int*)(argument+20)=15;
    slprobe::Record request;request.session=session;request.seq=seq;request.kind=kind;queuedAt=GetTickCount64();
    if(!action.Accept(request,name))return P04Err(L,"queue_rejected");
    SetAck("queued");p04Lua->pushboolean(L,1);return 1;
}
static int P04LuaSave(void* L){return P04Queue(L,"save");}
static int P04LuaLoad(void* L){return P04Queue(L,"load");}
static int P04LuaVerify(void* L) {
    unsigned long seq=0;std::string kind=P04Arg(L,2);
    if(!P04Seq(L,1,&seq)||seq!=action.last||kind!=action.request.kind||action.phase!=slnative::Returned||p04Failed)return P04Err(L,"not_returned");
    p04VerifyRequested=true;p04Lua->pushboolean(L,1);return 1;
}
static bool P04CaptureValid() {
    return action.request.kind=="save"&&(action.phase==slnative::Returned||action.phase==slnative::Verified)&&
        p04NativeCalled==1&&capturePublished&&CaptureComplete()&&captureAL==1&&
        _stricmp(capturePath.c_str(),Utf8(target).c_str())==0;
}
static int P04LuaCapture(void* L) {
    unsigned long seq=0;
    if(!P04Seq(L,1,&seq)||seq!=action.last||!P04CaptureValid())return P04Err(L,"capture_unavailable");
    p04Lua->pushlstring(L,captureText.data(),captureText.size());return 1;
}
static void P04Field(void* L,const char* key,const std::string& value){p04Lua->pushlstring(L,value.data(),value.size());p04Lua->setfield(L,-2,key);}
static void P04FieldInt(void* L,const char* key,int value){p04Lua->pushinteger(L,value);p04Lua->setfield(L,-2,key);}
static void P04FieldBool(void* L,const char* key,bool value){p04Lua->pushboolean(L,value);p04Lua->setfield(L,-2,key);}
static int P04LuaPoll(void* L) {
    if(action.phase==slnative::Queued&&GetTickCount64()-queuedAt>5000&&action.Begin()) {P04RejectNoCall(action.last,action.request.kind.c_str(),"queue_expired");action.Reject(-3);}
    if(p04VerifyRequested&&action.phase==slnative::Returned) {
        p04VerifyRequested=false;
        bool good=p04NativeCalled==1&&(action.request.kind=="save"?P04CaptureValid()&&action.al==-1:p04PrepareOk&&action.al==1);
        if(good){action.Verify(action.last);SetAck("verified");}
        else {p04Failed=true;strncpy_s(p04RejectReason,"verification_failed",_TRUNCATE);}
    }
    const char* state="idle";
    switch(action.phase){case slnative::Queued:state="queued";break;case slnative::Running:state="queued";break;
    case slnative::Returned:state="returned";break;case slnative::Verified:state="verified";break;case slnative::Rejected:state="rejected";break;}
    if(action.phase==slnative::Returned&&((action.request.kind=="save"&&!P04CaptureValid())||
       (action.request.kind=="load"&&action.al!=1)))p04Failed=true;
    if(p04Failed)state="failed";
    p04Lua->createtable(L,0,20);
    P04Field(L,"epoch",session);P04FieldInt(L,"seq",(int)action.last);P04Field(L,"kind",action.request.kind);
    P04Field(L,"name",action.name);P04Field(L,"profile",p04ActionProfile);P04Field(L,"exe",exeHash);P04Field(L,"state",state);
    P04FieldBool(L,"native_called",p04NativeCalled==1);P04FieldInt(L,"al",action.al);
    P04Field(L,"reason",p04RejectReason[0]?p04RejectReason:(p04Failed?"native_result_invalid":""));
    bool captured=p04NativeCalled==1&&action.request.kind=="save";
    P04FieldInt(L,"count",captured?(int)captureCount:0);
    P04FieldInt(L,"write_al",captured?(int)captureAL:0);
    P04Field(L,"capture_sha",captured&&!captureText.empty()?HashBuffer((const unsigned char*)captureText.data(),(unsigned int)captureText.size()):"");
    P04FieldInt(L,"capture_size",captured?(int)captureText.size():0);
    P04Field(L,"capture_path",captured?capturePath:"");
    if(action.request.kind=="load")P04FieldBool(L,"prepare_ok",p04PrepareOk);
    return 1;
}
static const struct {const char* name;int (*fn)(void*);} p04Functions[]={
    {"available",P04LuaAvailable},{"reason",P04LuaReason},{"exe_hash",P04LuaExeHash},{"epoch",P04LuaEpoch},{"root",P04LuaRoot},
    {"init",P04LuaInit},{"read",P04LuaRead},{"write_exclusive",P04LuaWriteExclusive},{"write_atomic",P04LuaWriteAtomic},
    {"exists",P04LuaExists},{"remove",P04LuaRemove},{"list",P04LuaList},{"sha256",P04LuaSha256},{"random_hex",P04LuaRandomHex},
    {"set_guard",P04LuaSetGuard},{"next_seq",P04LuaNextSeq},{"permit",P04LuaPermit},{"revoke",P04LuaRevoke},
    {"save",P04LuaSave},{"load",P04LuaLoad},{"verify",P04LuaVerify},{"capture",P04LuaCapture},{"poll",P04LuaPoll},
    {"request_escape_close",P04LuaRequestEscapeClose},
};
static int P04OpenImpl(void* L) {
    if(!P04ResolveLua())return 0;
    p04Lua->createtable(L,0,28);
    for(const auto& item:p04Functions){p04Lua->pushcclosure(L,item.fn,0);p04Lua->setfield(L,-2,item.name);}
    P04Field(L,"protocol","p04-v2");P04FieldInt(L,"bridge_abi",2);
    P04Field(L,"ui_bridge","p04-escape-v1");return 1;
}
extern "C" __declspec(dllexport) int P04Open(void* L){return P04OpenImpl(L);}
