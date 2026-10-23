#pragma once
#include <windows.h>
#include <stdio.h>
#include <string>

// One producer, one outstanding request. ASCII records; UTF-16 filesystem paths.
namespace slprobe {
struct Record {
    std::string session, kind;
    unsigned long seq = 0;
};

inline bool ParseRequest(const std::string& text, Record& out) {
    if (text.size() != 41 || text[24] != '|' || text[35] != '|' || text[40] != '\n') return false;
    for (size_t i = 0; i < 24; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'A' && text[i] <= 'F'))) return false;
    unsigned long long seq = 0;
    for (size_t i = 25; i < 35; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        seq = seq * 10 + text[i] - '0';
    }
    if (!seq || seq > 2147483647) return false;
    out.session = text.substr(0, 24);
    out.seq = (unsigned long)seq;
    out.kind = text.substr(36, 4);
    return out.kind == "save" || out.kind == "load" || out.kind == "prep";
}

inline bool ReadRecord(const std::wstring& path, std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buf[4096]; DWORD got = 0;
    bool ok = ReadFile(file, buf, sizeof(buf), &got, NULL) != 0 && got < sizeof(buf);
    CloseHandle(file);
    if (ok) text.assign(buf, got);
    return ok;
}

inline bool Publish(const std::wstring& path, const std::string& text) {
    std::wstring tmp = path + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(file, text.data(), (DWORD)text.size(), &wrote, NULL) != 0 && wrote == text.size();
    CloseHandle(file);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

class RequestChannel {
    std::wstring root_;
    std::string session_, pendingAck_;
    unsigned long last_ = 0, count_ = 0;
public:
    bool Start(const std::wstring& root) {
        if (root.size() < 3 || root[1] != L':' || (root[2] != L'\\' && root[2] != L'/')) return false;
        DWORD attr = GetFileAttributesW(root.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
        root_ = root;
        GUID id;
        if (CoCreateGuid(&id) != S_OK) return false;
        char token[25];
        // 96 bits of a new GUID; not a secret or an authentication token.
        sprintf_s(token, "%08lX%04X%04X%02X%02X%02X%02X", id.Data1, id.Data2, id.Data3,
            id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3]);
        session_ = token; last_ = count_ = 0; pendingAck_.clear();
        if (!Publish(Path(L"ack.txt"), session_ + "|0000000000|ready|dry_run\n")) return false;
        return Publish(Path(L"ready.txt"), session_ + "\n");
    }
    std::wstring Path(const wchar_t* name) const { return root_ + L"\\" + name; }
    const std::string& Session() const { return session_; }
    unsigned long Count() const { return count_; }
    const char* Poll() {
        if (session_.empty()) return "not_ready";
        if (!pendingAck_.empty()) {
            if (!Publish(Path(L"ack.txt"), pendingAck_)) return "ack_pending";
            pendingAck_.clear();
            return "ack_retried";
        }
        std::string text;
        if (!ReadRecord(Path(L"trigger.txt"), text)) return "unavailable";
        Record request;
        if (!ParseRequest(text, request)) return "invalid";
        if (request.session != session_ || request.seq <= last_) return "stale";
        // This is the only handler in this phase: no game objects, hooks or native calls.
        last_ = request.seq;
        ++count_;
        char tail[80];
        sprintf_s(tail, "|%010lu|%s|dry_run\n", request.seq, request.kind.c_str());
        pendingAck_ = session_ + tail;
        if (!Publish(Path(L"ack.txt"), pendingAck_)) return "consumed_ack_pending";
        pendingAck_.clear();
        return "consumed";
    }
};
}
