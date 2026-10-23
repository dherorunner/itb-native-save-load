// P04 minimal file service implementation. No game headers, no hooks, no threads.
#include "p04_service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace p04svc {
namespace {

std::wstring g_root;
bool g_configured = false;
unsigned g_faults = 0;

bool TakeFault(unsigned flag) {
    if ((g_faults & flag) == 0) return false;
    g_faults &= ~flag;
    return true;
}

void SetStatus(Status* status, int code, const std::wstring& detail) {
    if (status != nullptr) {
        status->code = code;
        status->detail = detail;
    }
}

int MapWinError(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return PermissionDenied;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return AlreadyExists;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return TooLarge;
        default:
            return IoError;
    }
}

std::wstring FullPath(const std::wstring& normalized) {
    if (g_root.empty()) return std::wstring();
    return g_root + L"\\" + normalized;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool SafeComponents(const std::wstring& path) {
    // Reject reparse points at every existing component, including the root.
    size_t begin=3;
    while(begin<=path.size()) {
        size_t end=path.find_first_of(L"\\/",begin);
        if(end==std::wstring::npos)end=path.size();
        std::wstring part=path.substr(begin,end-begin);
        if(part.empty() || part==L"." || part==L".." || part.back()==L'.' || part.back()==L' ')return false;
        DWORD attr=GetFileAttributesW(path.substr(0,end).c_str());
        if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        if(end==path.size())break;
        begin=end+1;
    }
    return true;
}

bool EnsureAbsoluteDirectory(const std::wstring& path, Status* status) {
    if (path.size() < 3 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
        SetStatus(status, BadPath, L"路径不是绝对盘符路径");
        return false;
    }
    if(!SafeComponents(path)){SetStatus(status,BadPath,L"拒绝重解析或不规范目录组件");return false;}
    std::wstring current = path.substr(0, 2);
    size_t index = 2;
    while (index < path.size()) {
        while (index < path.size() && (path[index] == L'\\' || path[index] == L'/')) ++index;
        if (index >= path.size()) break;
        size_t end = index;
        while (end < path.size() && path[end] != L'\\' && path[end] != L'/') ++end;
        current += L"\\" + path.substr(index, end - index);
        if (!DirectoryExists(current)) {
            if (CreateDirectoryW(current.c_str(), nullptr) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
                SetStatus(status, MapWinError(GetLastError()), L"创建目录失败: " + current);
                return false;
            }
        }
        index = end;
    }
    if (!DirectoryExists(path)) {
        SetStatus(status, IoError, L"目录不可用: " + path);
        return false;
    }
    return true;
}

std::wstring ParentOf(const std::wstring& normalized) {
    const size_t slash = normalized.find_last_of(L'\\');
    if (slash == std::wstring::npos) return std::wstring();
    return normalized.substr(0, slash);
}

bool ReadWhole(const std::wstring& full, size_t max_bytes, std::string* bytes, Status* status) {
    HANDLE handle = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        SetStatus(status, MapWinError(error), L"打开失败: " + full);
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        SetStatus(status, MapWinError(error), L"读取大小失败");
        return false;
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
        CloseHandle(handle);
        SetStatus(status, TooLarge, L"文件超过读取上限");
        return false;
    }
    std::string data;
    data.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(handle, &data[offset], chunk, &read, nullptr) || read == 0) {
            const DWORD error = GetLastError();
            CloseHandle(handle);
            SetStatus(status, MapWinError(error), L"读取失败");
            return false;
        }
        offset += read;
    }
    CloseHandle(handle);
    if (bytes != nullptr) *bytes = std::move(data);
    return true;
}

bool WriteNewFile(const std::wstring& full, const std::string& bytes, Status* status) {
    HANDLE handle = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        SetStatus(status, MapWinError(error), L"独占创建失败: " + full);
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            const DWORD error = GetLastError();
            CloseHandle(handle);
            DeleteFileW(full.c_str());
            SetStatus(status, MapWinError(error), L"写入失败");
            return false;
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        DeleteFileW(full.c_str());
        SetStatus(status, MapWinError(error), L"刷新失败");
        return false;
    }
    CloseHandle(handle);
    return true;
}

}  // namespace

bool Configure(const std::wstring& absolute_root, Status* status) {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetFullPathNameW(absolute_root.c_str(), MAX_PATH * 4, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH * 4) {
        SetStatus(status, BadPath, L"根路径无法规范化");
        return false;
    }
    std::wstring normalized(buffer, length);
    while (normalized.size() > 3 && (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }
    if (!EnsureAbsoluteDirectory(normalized, status)) return false;
    if (g_configured) {
        if (normalized != g_root) {
            SetStatus(status, AlreadyExists, L"本进程已绑定其它外置根");
            return false;
        }
        return true;
    }
    g_root = normalized;
    g_configured = true;
    return true;
}

bool Configured() { return g_configured; }

const std::wstring& Root() { return g_root; }

bool NormalizeRelative(const std::wstring& relative, std::wstring* normalized, Status* status) {
    if (relative.empty()) {
        SetStatus(status, BadPath, L"相对路径为空");
        return false;
    }
    if (relative[0] == L'\\' || relative[0] == L'/') {
        SetStatus(status, BadPath, L"拒绝绝对路径");
        return false;
    }
    std::wstring out;
    std::wstring component;
    bool ended_with_separator = false;
    const auto flush = [&]() -> bool {
        if (component.empty()) {
            SetStatus(status, BadPath, L"拒绝空路径组件");
            return false;
        }
        if (component == L"." || component == L".." || component.back()==L'.' || component.back()==L' ') {
            SetStatus(status, BadPath, L"拒绝 . 或 .. 组件");
            return false;
        }
        if (component.find_first_of(L":<>\"|?*") != std::wstring::npos) {
            SetStatus(status, BadPath, L"拒绝盘符或备用流");
            return false;
        }
        if (component.size() > 128) {
            SetStatus(status, BadPath, L"路径组件过长");
            return false;
        }
        if (!out.empty()) out += L'\\';
        out += component;
        component.clear();
        ended_with_separator = false;
        return true;
    };
    for (const wchar_t ch : relative) {
        if (ch == L'\\' || ch == L'/') {
            if (ended_with_separator) {
                SetStatus(status, BadPath, L"拒绝连续分隔符");
                return false;
            }
            if (!flush()) return false;
            ended_with_separator = true;
        } else if (ch < 0x20) {
            SetStatus(status, BadPath, L"路径含控制字符");
            return false;
        } else {
            ended_with_separator = false;
            component += ch;
        }
    }
    if (ended_with_separator) {
        SetStatus(status, BadPath, L"拒绝结尾分隔符");
        return false;
    }
    if (!flush()) return false;
    if (out.empty()) {
        SetStatus(status, BadPath, L"相对路径没有有效组件");
        return false;
    }
    if (normalized != nullptr) *normalized = out;
    return true;
}

bool IsInsideRoot(const std::wstring& path) {
    if (!g_configured || path.size() <= g_root.size()) return false;
    if (_wcsnicmp(path.c_str(), g_root.c_str(), g_root.size()) != 0) return false;
    if(path[g_root.size()] != L'\\')return false;
    std::wstring normalized;
    if(!NormalizeRelative(path.substr(g_root.size()+1),&normalized,nullptr))return false;
    return SafeComponents(path);
}

bool EnsureDirectory(const std::wstring& relative, Status* status) {
    if (!g_configured) {
        SetStatus(status, NotConfigured, L"文件服务未初始化");
        return false;
    }
    std::wstring normalized;
    if (!NormalizeRelative(relative, &normalized, status)) return false;
    return EnsureAbsoluteDirectory(FullPath(normalized), status);
}

bool Read(const std::wstring& relative, size_t max_bytes, std::string* bytes, Status* status) {
    if (!g_configured) {
        SetStatus(status, NotConfigured, L"文件服务未初始化");
        return false;
    }
    std::wstring normalized;
    if (!NormalizeRelative(relative, &normalized, status)) return false;
    const std::wstring full = FullPath(normalized);
    if (!IsInsideRoot(full)) {
        SetStatus(status, BadPath, L"路径越界");
        return false;
    }
    return ReadWhole(full, max_bytes, bytes, status);
}

bool WriteExclusive(const std::wstring& relative, const std::string& bytes, Status* status) {
    if (!g_configured) {
        SetStatus(status, NotConfigured, L"文件服务未初始化");
        return false;
    }
    std::wstring normalized;
    if (!NormalizeRelative(relative, &normalized, status)) return false;
    const std::wstring parent = ParentOf(normalized);
    if (!parent.empty() && !EnsureDirectory(parent, status)) return false;
    const std::wstring full = FullPath(normalized);
    if (!IsInsideRoot(full)) {
        SetStatus(status, BadPath, L"路径越界");
        return false;
    }
    if (TakeFault(FaultExclusiveWrite)) {
        SetStatus(status, IoError, L"测试注入的独占写失败");
        return false;
    }
    return WriteNewFile(full, bytes, status);
}

bool WriteAtomic(const std::wstring& relative, const std::string& bytes, Status* status) {
    if (!g_configured) {
        SetStatus(status, NotConfigured, L"文件服务未初始化");
        return false;
    }
    std::wstring normalized;
    if (!NormalizeRelative(relative, &normalized, status)) return false;
    const std::wstring parent = ParentOf(normalized);
    if (!parent.empty() && !EnsureDirectory(parent, status)) return false;
    const std::wstring target = FullPath(normalized);
    if (!IsInsideRoot(target)) {
        SetStatus(status, BadPath, L"路径越界");
        return false;
    }
    std::string suffix;
    if (!RandomHex(8, &suffix, status)) return false;
    const std::wstring temp = target + L".tmp-" + std::wstring(suffix.begin(), suffix.end());
    if (!WriteNewFile(temp, bytes, status)) return false;
    std::string readback;
    Status verify{};
    if (!ReadWhole(temp, bytes.size() + 1, &readback, &verify) || readback != bytes) {
        DeleteFileW(temp.c_str());
        SetStatus(status, VerifyFailed, L"临时文件读回不一致");
        return false;
    }
    if (TakeFault(FaultAtomicMove)) {
        DeleteFileW(temp.c_str());
        SetStatus(status, IoError, L"测试注入的指针替换失败");
        return false;
    }
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temp.c_str());
        SetStatus(status, MapWinError(error), L"替换目标失败");
        return false;
    }
    readback.clear();
    if (TakeFault(FaultPostCommitRead) || !ReadWhole(target, bytes.size() + 1, &readback, &verify) || readback != bytes) {
        SetStatus(status, CommitUncertain, L"提交后读回不一致，目标可能已经提交");
        return false;
    }
    return true;
}

bool Exists(const std::wstring& relative, bool* exists, Status* status) {
    if (!g_configured) {
        SetStatus(status, NotConfigured, L"文件服务未初始化");
        return false;
    }
    std::wstring normalized;
    if (!NormalizeRelative(relative, &normalized, status)) return false;
    const std::wstring full = FullPath(normalized);
    if (!IsInsideRoot(full)) {
        SetStatus(status, BadPath, L"路径越界");
        return false;
    }
    const DWORD attributes = GetFileAttributesW(full.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (exists != nullptr) *exists = true;
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        if (exists != nullptr) *exists = false;
        return true;
    }
    SetStatus(status, MapWinError(error), L"查询存在性失败");
    return false;
}

bool Remove(const std::wstring& relative, Status* status) {
    if (!g_configured) {
        SetStatus(status, NotConfigured, L"文件服务未初始化");
        return false;
    }
    std::wstring normalized;
    if (!NormalizeRelative(relative, &normalized, status)) return false;
    const std::wstring full = FullPath(normalized);
    if (!IsInsideRoot(full)) {
        SetStatus(status, BadPath, L"路径越界");
        return false;
    }
    if (DeleteFileW(full.c_str()) != 0) return true;
    const DWORD error = GetLastError();
    SetStatus(status, MapWinError(error), L"删除失败");
    return false;
}

bool ListFiles(const std::wstring& relative, std::string* newline_separated, Status* status) {
    if (!g_configured) {
        SetStatus(status, NotConfigured, L"文件服务未初始化");
        return false;
    }
    std::wstring normalized;
    if (!NormalizeRelative(relative, &normalized, status)) return false;
    const std::wstring full = FullPath(normalized);
    if (!IsInsideRoot(full)) {
        SetStatus(status, BadPath, L"路径越界");
        return false;
    }
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = full + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        SetStatus(status, error == ERROR_FILE_NOT_FOUND ? NotFound : MapWinError(error), L"列目录失败");
        return false;
    }
    std::vector<std::wstring> names;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        names.push_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(names.begin(), names.end());
    std::string joined;
    for (const std::wstring& name : names) {
        if (!joined.empty()) joined += '\n';
        joined += Utf8(name);
    }
    if (newline_separated != nullptr) *newline_separated = std::move(joined);
    return true;
}

bool Sha256Hex(const std::string& bytes, std::string* hex, Status* status) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    unsigned char digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        SetStatus(status, IoError, L"SHA-256 算法不可用");
        return false;
    }
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0
        && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                          static_cast<ULONG>(bytes.size()), 0) >= 0
        && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (const unsigned char value : digest) {
            out += kHex[value >> 4];
            out += kHex[value & 0x0f];
        }
        if (hex != nullptr) *hex = std::move(out);
        ok = true;
    } else {
        SetStatus(status, IoError, L"SHA-256 计算失败");
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool RandomHex(unsigned byte_count, std::string* hex, Status* status) {
    if (byte_count == 0 || byte_count > 64) {
        SetStatus(status, BadPath, L"随机字节数超出范围");
        return false;
    }
    unsigned char buffer[64] = {};
    if (BCryptGenRandom(nullptr, buffer, byte_count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        SetStatus(status, IoError, L"系统随机数不可用");
        return false;
    }
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(byte_count * 2);
    for (unsigned i = 0; i < byte_count; ++i) {
        out += kHex[buffer[i] >> 4];
        out += kHex[buffer[i] & 0x0f];
    }
    if (hex != nullptr) *hex = std::move(out);
    return true;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], needed,
                        nullptr, nullptr);
    return out;
}

bool Wide(const std::string& text,std::wstring* out) {
    if(text.find('\0')!=std::string::npos)return false;
    if(text.empty()){out->clear();return true;}
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),nullptr,0);
    if(n<=0)return false;
    out->assign((size_t)n,L'\0');
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),&(*out)[0],n)==n;
}

void SetFaultsForTest(unsigned flags) { g_faults = flags; }

unsigned FaultsForTest() { return g_faults; }

}  // namespace p04svc
