// P04 minimal file service: an external-root, no-game-dependency store helper.
// This module is deliberately separate from native_roundtrip.cpp so the same bytes
// can be compiled into the product DLL and into an offline test executable.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace p04svc {

enum Code {
    OK = 0,
    NotFound = 1,
    AlreadyExists = 2,
    PermissionDenied = 3,
    TooLarge = 4,
    BadPath = 5,
    IoError = 6,
    VerifyFailed = 7,
    NotConfigured = 8,
    CommitUncertain = 9,
};

struct Status {
    int code = OK;
    std::wstring detail;
    bool ok() const { return code == OK; }
};

// Configures the single allowed root for this process. The root must be absolute;
// it is normalized and created if missing. Calling again with a different root fails.
bool Configure(const std::wstring& absolute_root, Status* status);

// True once Configure succeeded.
bool Configured();

// Normalized absolute root (empty before Configure).
const std::wstring& Root();

// Accepts only relative paths made of 1..n components. Rejects empty components,
// '.', '..', drive letters, leading slashes and embedded separators inside names.
bool NormalizeRelative(const std::wstring& relative, std::wstring* normalized, Status* status);

// Ensures a relative directory exists (creating intermediate components).
bool EnsureDirectory(const std::wstring& relative, Status* status);

// Reads a whole file. Missing and unreadable are distinct codes.
bool Read(const std::wstring& relative, size_t max_bytes, std::string* bytes, Status* status);

// Writes a new file and never replaces an existing one.
bool WriteExclusive(const std::wstring& relative, const std::string& bytes, Status* status);

// Writes to a same-directory temporary file, flushes it, then replaces the target.
// On any failure before the replace the previous target bytes stay untouched.
bool WriteAtomic(const std::wstring& relative, const std::string& bytes, Status* status);

bool Exists(const std::wstring& relative, bool* exists, Status* status);

// Removes one regular file under the root. Missing is reported as NotFound.
bool Remove(const std::wstring& relative, Status* status);

// Lists immediate file names of a relative directory (deterministic sort).
bool ListFiles(const std::wstring& relative, std::string* newline_separated, Status* status);

// Lowercase hex SHA-256 of the given bytes.
bool Sha256Hex(const std::string& bytes, std::string* hex, Status* status);

// Lowercase hex from a cryptographically strong system RNG.
bool RandomHex(unsigned byte_count, std::string* hex, Status* status);

// UTF-8 conversion used for Lua-visible diagnostics.
std::string Utf8(const std::wstring& text);
bool Wide(const std::string& text, std::wstring* out);

// True when `path` resolves inside the configured root (component-wise check).
bool IsInsideRoot(const std::wstring& path);

// Test hook: a deterministic fault point used by the offline tests. Values are
// bit flags applied to the next matching operation, then cleared.
enum FaultFlag : unsigned {
    FaultNone = 0,
    FaultAtomicMove = 1u << 0,
    FaultExclusiveWrite = 1u << 1,
    FaultPostCommitRead = 1u << 2,
};
void SetFaultsForTest(unsigned flags);
unsigned FaultsForTest();

}  // namespace p04svc
