// WinFsUtils.hpp - Windows 文件系统与编码工具函数（header-only）
#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <winsock2.h>
#include <windows.h>
#include <direct.h>
#include <io.h>

#pragma comment(lib, "shlwapi.lib")

namespace winfs {

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

inline std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

inline std::string utf8ToAnsi(const std::string& utf8) {
    if (utf8.empty()) return "";
    std::wstring ws = utf8ToWide(utf8);
    if (ws.empty()) return utf8;
    int len = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return utf8;
    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

inline std::string resolvePath(const std::string& path) {
    wchar_t resolved[MAX_PATH];
    std::wstring wpath = utf8ToWide(path);
    if (GetFullPathNameW(wpath.c_str(), MAX_PATH, resolved, nullptr))
        return wideToUtf8(resolved);
    return path;
}

inline bool dirExists(const std::string& path) {
    DWORD attrs = GetFileAttributesW(utf8ToWide(path).c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool fileExists(const std::string& path) {
    DWORD attrs = GetFileAttributesW(utf8ToWide(path).c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool mkdirp(const std::string& path) {
    if (dirExists(path)) return true;
    std::string current;
    for (size_t i = 0; i < path.size(); i++) {
        current += path[i];
        if ((path[i] == '/' || path[i] == '\\') && current.size() > 1) {
            CreateDirectoryW(utf8ToWide(current).c_str(), nullptr);
        }
    }
    CreateDirectoryW(utf8ToWide(current).c_str(), nullptr);
    return dirExists(path);
}

struct DirEntry {
    std::string name;
    bool isDir;
    unsigned long fileSize;
    time_t modTime;
};

inline std::vector<DirEntry> listDirEntries(const std::string& path) {
    std::vector<DirEntry> entries;
    std::wstring searchPath = utf8ToWide(path) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return entries;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
        DirEntry e;
        e.name = wideToUtf8(fd.cFileName);
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.fileSize = fd.nFileSizeLow;
        ULARGE_INTEGER ull;
        ull.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        ull.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        e.modTime = (time_t)(ull.QuadPart / 10000000ULL - 11644473600ULL);
        entries.push_back(e);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return entries;
}

inline long long getDirSizeRecursive(const std::string& path) {
    long long size = 0;
    for (auto& e : listDirEntries(path)) {
        if (e.isDir) size += getDirSizeRecursive(path + "/" + e.name);
        else size += e.fileSize;
    }
    return size;
}

inline int countFilesRecursive(const std::string& path) {
    int count = 0;
    for (auto& e : listDirEntries(path)) {
        if (e.isDir) count += countFilesRecursive(path + "/" + e.name);
        else count++;
    }
    return count;
}

inline bool removeDirRecursive(const std::string& path) {
    bool ok = true;
    for (auto& e : listDirEntries(path)) {
        std::string fp = path + "/" + e.name;
        if (e.isDir) { if (!removeDirRecursive(fp)) ok = false; }
        else { if (!DeleteFileW(utf8ToWide(fp).c_str())) ok = false; }
    }
    if (!RemoveDirectoryW(utf8ToWide(path).c_str())) ok = false;
    return ok;
}

inline std::vector<std::string> listSubdirs(const std::string& path) {
    std::vector<std::string> dirs;
    for (auto& e : listDirEntries(path))
        if (e.isDir) dirs.push_back(e.name);
    return dirs;
}

inline std::string readFileToString(const std::string& path) {
    std::ifstream f(utf8ToAnsi(path), std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline std::string urlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '%' && i + 2 < str.size()) {
            char hex[3] = { str[i + 1], str[i + 2], 0 };
            result += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

} // namespace winfs
