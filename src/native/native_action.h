#pragma once
#include "request_channel.h"

// Bounded experiment only. Idle -> queued -> running -> returned -> verified.
// A returned call is deliberately still busy until the operator verifies its result.
namespace slnative {
enum { Idle = 0, Queued = 1, Running = 2, Returned = 3, Verified = 4, Rejected = 5 };
inline bool ValidName(const std::string& name) {
    if (name.size() != 12 || name.substr(0,4) != "p01_" || name.substr(8) != ".lua") return false;
    for (int i=4;i<8;++i) if (!((name[i]>='0' && name[i]<='9') || (name[i]>='A' && name[i]<='F'))) return false;
    return true;
}
inline bool ValidNameP04(const std::string& name) {
    if (name.size() != 12 || name.substr(0,4) != "p04_" || name.substr(8) != ".lua") return false;
    for (int i=4;i<8;++i) if (!((name[i]>='0' && name[i]<='9') || (name[i]>='A' && name[i]<='F'))) return false;
    return true;
}
struct Action {
    volatile LONG phase = Idle;
    unsigned long last = 0;
    slprobe::Record request;
    std::string name;
    LONG al = -1;
    bool Accept(const slprobe::Record& next, const std::string& file) {
#ifdef ITB_SL_P04_SERVICE
        const bool validName=ValidNameP04(file);
#else
        const bool validName=ValidName(file);
#endif
        if ((phase != Idle && phase != Verified && phase != Rejected) || next.seq != last+1 || !validName) return false;
        request=next; name=file; last=next.seq; al=-1;
        InterlockedExchange(&phase,Queued); return true;
    }
    bool Begin() { return InterlockedCompareExchange(&phase,Running,Queued)==Queued; }
    void Finish(LONG result) { al=result; InterlockedExchange(&phase,Returned); }
    bool Reject(LONG reason) {
        LONG previous = InterlockedCompareExchange(&phase, Rejected, Running);
        if (previous != Running) return false;
        al = reason;
        return true;
    }
    bool Verify(unsigned long seq) {
        if (seq != last) return false;
        return InterlockedCompareExchange(&phase,Verified,Returned)==Returned;
    }
};
}
