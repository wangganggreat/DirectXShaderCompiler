///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// dxcfilesystem.cpp                                                         //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Provides helper file system for dxcompiler.                               //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include "dxc/Support/WinIncludes.h"
#include "dxc/HLSL/DxilContainer.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/FileIOHelper.h"
#include "dxc/dxcapi.h"
#include "llvm/Support/raw_ostream.h"
#include "dxcutil.h"

#include "dxc/Support/dxcfilesystem.h"
#include "dxc/Support/Unicode.h"
#include "clang/Frontend/CompilerInstance.h"

using namespace llvm;
using namespace hlsl;

// DxcArgsFileSystem
namespace {

#if defined(_MSC_VER)
#include <io.h>
#ifndef STDOUT_FILENO
# define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
# define STDERR_FILENO 2
#endif
#endif

#ifdef DBG

// This should be improved with global enabled mask rather than a compile-time mask.
#define DXTRACE_MASK_ENABLED  0
#define DXTRACE_MASK_APIFS    1
#define DXTRACE_ENABLED(subsystem) (DXTRACE_MASK_ENABLED & subsystem)

// DXTRACE_FMT formats a debugger trace message if DXTRACE_MASK allows it.
#define DXTRACE_FMT(subsystem, fmt, ...) do { \
  if (DXTRACE_ENABLED(subsystem)) OutputDebugFormatA(fmt, __VA_ARGS__); \
} while (0)
/// DXTRACE_FMT_APIFS is used by the API-based virtual filesystem.
#define DXTRACE_FMT_APIFS(fmt, ...) DXTRACE_FMT(DXTRACE_MASK_APIFS, fmt, __VA_ARGS__)

#else

#define DXTRACE_FMT_APIFS(...)

#endif // DBG


enum class HandleKind {
  Special = 0,
  File = 1,
  FileDir = 2,
  SearchDir = 3
};
enum class SpecialValue {
  Unknown = 0,
  StdOut = 1,
  StdErr = 2,
  Source = 3,
  Output = 4
};
struct HandleBits {
  unsigned Offset : 8;
  unsigned Length : 8;
  unsigned Kind : 4;
};
struct DxcArgsHandle {
  DxcArgsHandle(HANDLE h) : Handle(h) {}
  DxcArgsHandle(unsigned fileIndex)
    : Bits{ fileIndex, 0, (unsigned)HandleKind::File } {}
  DxcArgsHandle(HandleKind HK, unsigned fileIndex, unsigned dirLength)
    : Bits{ fileIndex, dirLength, (unsigned)HK} {}
  DxcArgsHandle(SpecialValue V)
      : Bits{(unsigned)V, 0, (unsigned)HandleKind::Special} {}
  union {
    HANDLE Handle;
    HandleBits Bits;
  };
  bool operator==(const DxcArgsHandle &Other) { return Handle == Other.Handle; }
  HandleKind GetKind() const { return (HandleKind)Bits.Kind; }
  bool IsFileKind() const { return GetKind() == HandleKind::File; }
  bool IsSpecialUnknown() const { return Handle == 0; }
  bool IsDirHandle() const {
    return GetKind() == HandleKind::FileDir || GetKind() == HandleKind::SearchDir;
  }
  bool IsStdHandle() const {
    return GetKind() == HandleKind::Special &&
           (GetSpecialValue() == SpecialValue::StdErr ||
            GetSpecialValue() == SpecialValue::StdOut);
  }
  unsigned GetFileIndex() const {
    DXASSERT_NOMSG(IsFileKind());
    return Bits.Offset;
  }
  SpecialValue GetSpecialValue() const {
    DXASSERT_NOMSG(GetKind() == HandleKind::Special);
    return (SpecialValue)Bits.Offset;
  }
  unsigned Length() const { return Bits.Length; }
};

static_assert(sizeof(DxcArgsHandle) == sizeof(HANDLE), "else can't transparently typecast");

const DxcArgsHandle UnknownHandle(SpecialValue::Unknown);
const DxcArgsHandle StdOutHandle(SpecialValue::StdOut);
const DxcArgsHandle StdErrHandle(SpecialValue::StdErr);
const DxcArgsHandle OutputHandle(SpecialValue::Output);

/// Max number of included files (1:1 to their directories) or search directories.
/// If programs include more than a handful, DxcArgsFileSystem will need to do better than linear scans.
/// If this is fired, ERROR_OUT_OF_STRUCTURES will be returned by an attempt to open a file.
static const size_t MaxIncludedFiles = 1000;

bool IsAbsoluteOrCurDirRelativeW(LPCWSTR Path) {
  if (!Path || !Path[0]) return FALSE;
  // Current dir-relative path.
  if (Path[0] == L'.') {
    return Path[1] == L'\0' || Path[1] == L'/' || Path[1] == L'\\';
  }
  // Disk designator, then absolute path.
  if (Path[1] == L':' && Path[2] == L'\\') {
    return TRUE;
  }
  // UNC name
  if (Path[0] == L'\\') {
    return Path[1] == L'\\';
  }

  //
  // NOTE: there are a number of cases we don't handle, as they don't play well with the simple
  // file system abstraction we use:
  // - current directory on disk designator (eg, D:file.ext), requires per-disk current dir
  // - parent paths relative to current directory (eg, ..\\file.ext)
  //
  // The current-directory support is available to help in-memory handlers. On-disk handlers
  // will typically have absolute paths to begin with.
  //
  return FALSE;
}

void MakeAbsoluteOrCurDirRelativeW(LPCWSTR &Path, std::wstring &PathStorage) {
  if (IsAbsoluteOrCurDirRelativeW(Path)) {
    return;
  }
  else {
    PathStorage = L"./";
    PathStorage += Path;
    Path = PathStorage.c_str();
  }
}

}

namespace dxcutil {
/// File system based on API arguments. Support being added incrementally.
///
/// DxcArgsFileSystem emulates a file system to clang/llvm based on API
/// arguments. It can block certain functionality (like picking up the current
/// directory), while adding other (like supporting an app's in-memory
/// files through an IDxcIncludeHandler).
///
/// stdin/stdout/stderr are registered especially (given that they have a
/// special role in llvm::ins/outs/errs and are defaults to various operations,
/// it's not unexpected). The direct user of DxcArgsFileSystem can also register
/// streams to capture output for specific files.
///
/// Support for IDxcIncludeHandler is somewhat tricky because the API is very
/// minimal, to allow simple implementations, but that puts this class in the
/// position of brokering between llvm/clang existing files (which probe for
/// files and directories in various patterns), and this simpler handler.
/// The current approach is to minimize changes in llvm/clang and work around
/// the absence of directory support in IDxcIncludeHandler by assuming all
/// included paths already exist (the handler may reject those paths later on),
/// and always querying for a file before its parent directory (so we can
/// disambiguate between one or the other).
class DxcArgsFileSystemImpl : public DxcArgsFileSystem {
private:
  CComPtr<IDxcBlob> m_pSource;
  LPCWSTR m_pSourceName;
  std::wstring m_pAbsSourceName; // absolute (or '.'-relative) source name
  CComPtr<IStream> m_pSourceStream;
  CComPtr<IStream> m_pOutputStream;
  CComPtr<AbstractMemoryStream> m_pStdOutStream;
  CComPtr<AbstractMemoryStream> m_pStdErrStream;
  LPCWSTR m_pOutputStreamName;
  std::wstring m_pAbsOutputStreamName;
  CComPtr<IDxcIncludeHandler> m_includeLoader;
  std::vector<std::wstring> m_searchEntries;
  bool m_bDisplayIncludeProcess;

  // Some constraints of the current design: opening the same file twice
  // will return the same handle/structure, and thus the same file pointer.
  struct IncludedFile {
    CComPtr<IDxcBlob> Blob;
    CComPtr<IStream> BlobStream;
    std::wstring Name;
    IncludedFile(std::wstring &&name, IDxcBlob *pBlob, IStream *pStream)
      : Name(name), Blob(pBlob), BlobStream(pStream) { }
  };
  llvm::SmallVector<IncludedFile, 4> m_includedFiles;

  static bool IsDirOf(LPCWSTR lpDir, size_t dirLen, const std::wstring &fileName) {
    if (fileName.size() <= dirLen) return false;
    if (0 != wcsncmp(lpDir, fileName.data(), dirLen)) return false;

    // Prefix matches, c:\\ to c:\\foo.hlsl or ./bar to ./bar/file.hlsl
    // Ensure there are no additional characters, don't match ./ba if ./bar.hlsl exists
    if (lpDir[dirLen - 1] == '\\' || lpDir[dirLen - 1] == '/') {
      // The file name was already terminated in a separator.
      return true;
    }

    return fileName.data()[dirLen] == '\\' || fileName.data()[dirLen] == '/';
  }

  static bool IsDirPrefixOrSame(LPCWSTR lpDir, size_t dirLen, const std::wstring &path) {
    if (0 == wcscmp(lpDir, path.c_str())) return true;
    return IsDirOf(lpDir, dirLen, path);
  }

  HANDLE TryFindDirHandle(LPCWSTR lpDir) const {
    size_t dirLen = wcslen(lpDir);
    for (size_t i = 0; i < m_includedFiles.size(); ++i) {
      const std::wstring &fileName = m_includedFiles[i].Name;
      if (IsDirOf(lpDir, dirLen, fileName)) {
        return DxcArgsHandle(HandleKind::FileDir, i, dirLen).Handle;
      }
    }
    for (size_t i = 0; i < m_searchEntries.size(); ++i) {
      if (IsDirPrefixOrSame(lpDir, dirLen, m_searchEntries[i])) {
        return DxcArgsHandle(HandleKind::SearchDir, i, dirLen).Handle;
      }
    }
    return INVALID_HANDLE_VALUE;
  }
  DWORD TryFindOrOpen(LPCWSTR lpFileName, size_t &index) {
    for (size_t i = 0; i < m_includedFiles.size(); ++i) {
      if (0 == wcscmp(lpFileName, m_includedFiles[i].Name.data())) {
        index = i;
        return ERROR_SUCCESS;
      }
    }

    if (m_includeLoader.p != nullptr) {
      if (m_includedFiles.size() == MaxIncludedFiles) {
        return ERROR_OUT_OF_STRUCTURES;
      }

      CComPtr<::IDxcBlob> fileBlob;
      HRESULT hr = m_includeLoader->LoadSource(lpFileName, &fileBlob);
      if (FAILED(hr)) {
        return ERROR_UNHANDLED_EXCEPTION;
      }
      if (fileBlob.p != nullptr) {
        CComPtr<IDxcBlobEncoding> fileBlobEncoded;
        if (FAILED(hlsl::DxcGetBlobAsUtf8(fileBlob, &fileBlobEncoded))) {
          return ERROR_UNHANDLED_EXCEPTION;
        }
        CComPtr<IStream> fileStream;
        if (FAILED(hlsl::CreateReadOnlyBlobStream(fileBlobEncoded, &fileStream))) {
          return ERROR_UNHANDLED_EXCEPTION;
        }
        m_includedFiles.emplace_back(std::wstring(lpFileName), fileBlobEncoded, fileStream);
        index = m_includedFiles.size() - 1;

        if (m_bDisplayIncludeProcess) {
          std::string openFileStr;
          raw_string_ostream s(openFileStr);
          std::string fileName = Unicode::UTF16ToUTF8StringOrThrow(lpFileName);
          s << "Opening file [" << fileName << "], stack top [" << (index-1)
            << "]\n";
          s.flush();
          ULONG cbWritten;
          IFT(m_pStdErrStream->Write(openFileStr.c_str(), openFileStr.size(),
                                 &cbWritten));
        }
        return ERROR_SUCCESS;
      }
    }
    return ERROR_NOT_FOUND;
  }
  static HANDLE IncludedFileIndexToHandle(size_t index) {
    return DxcArgsHandle(index).Handle;
  }
  bool IsKnownHandle(HANDLE h) const {
    return !DxcArgsHandle(h).IsSpecialUnknown();
  }
  IncludedFile &HandleToIncludedFile(HANDLE handle) {
    DxcArgsHandle argsHandle(handle);
    DXASSERT_NOMSG(argsHandle.GetFileIndex() < m_includedFiles.size());
    return m_includedFiles[argsHandle.GetFileIndex()];
  }

public:
  DxcArgsFileSystemImpl(_In_ IDxcBlob *pSource, LPCWSTR pSourceName, _In_opt_ IDxcIncludeHandler* pHandler)
      : m_pSource(pSource), m_pSourceName(pSourceName), m_includeLoader(pHandler), m_bDisplayIncludeProcess(false),
        m_pOutputStreamName(nullptr) {
    MakeAbsoluteOrCurDirRelativeW(m_pSourceName, m_pAbsSourceName);
    IFT(CreateReadOnlyBlobStream(m_pSource, &m_pSourceStream));
    m_includedFiles.push_back(IncludedFile(std::wstring(m_pSourceName), m_pSource, m_pSourceStream));
  }
  void EnableDisplayIncludeProcess() override {
    m_bDisplayIncludeProcess = true;
  }
  void WriteStdErrToStream(raw_string_ostream &s) override {
    s.write((char*)m_pStdErrStream->GetPtr(), m_pStdErrStream->GetPtrSize());
    s.flush();
  }
  HRESULT CreateStdStreams(_In_ IMalloc* pMalloc) override {
    DXASSERT(m_pStdOutStream == nullptr, "else already created");
    CreateMemoryStream(pMalloc, &m_pStdOutStream);
    CreateMemoryStream(pMalloc, &m_pStdErrStream);
    if (m_pStdOutStream == nullptr || m_pStdErrStream == nullptr) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }

  void GetStreamForFD(int fd, IStream** ppResult) {
    return GetStreamForHandle(HandleFromFD(fd), ppResult);
  }
  void GetStreamForHandle(HANDLE handle, IStream** ppResult) {
    CComPtr<IStream> stream;
    DxcArgsHandle argsHandle(handle);
    if (argsHandle == OutputHandle) {
      stream = m_pOutputStream;
    }
    else if (argsHandle == StdOutHandle) {
      stream = m_pStdOutStream;
    }
    else if (argsHandle == StdErrHandle) {
      stream = m_pStdErrStream;
    }
    else if (argsHandle.GetKind() == HandleKind::File) {
      stream = HandleToIncludedFile(handle).BlobStream;
    }
    *ppResult = stream.Detach();
  }

  void GetStdOutpuHandleStream(IStream **ppResultStream) override {
    return GetStreamForHandle(StdOutHandle.Handle, ppResultStream);
  }

  void SetupForCompilerInstance(clang::CompilerInstance &compiler) override {
    DXASSERT(m_searchEntries.size() == 0, "else compiler instance being set twice");
    // Turn these into UTF-16 to avoid converting later, and ensure they
    // are fully-qualified or relative to the current directory.
    const std::vector<clang::HeaderSearchOptions::Entry> &entries =
      compiler.getHeaderSearchOpts().UserEntries;
    if (entries.size() > MaxIncludedFiles) {
      throw hlsl::Exception(HRESULT_FROM_WIN32(ERROR_OUT_OF_STRUCTURES));
    }
    for (unsigned i = 0, e = entries.size(); i != e; ++i) {
      const clang::HeaderSearchOptions::Entry &E = entries[i];
      if (IsAbsoluteOrCurDirRelative(E.Path.c_str())) {
        m_searchEntries.emplace_back(Unicode::UTF8ToUTF16StringOrThrow(E.Path.c_str()));
      }
      else {
        std::wstring ws(L"./");
        ws += Unicode::UTF8ToUTF16StringOrThrow(E.Path.c_str());
        m_searchEntries.emplace_back(std::move(ws));
      }
    }
  }

  HRESULT RegisterOutputStream(LPCWSTR pName, IStream *pStream) override {
    DXASSERT(m_pOutputStream.p == nullptr, "else multiple outputs registered");
    m_pOutputStream = pStream;
    m_pOutputStreamName = pName;
    MakeAbsoluteOrCurDirRelativeW(m_pOutputStreamName, m_pAbsOutputStreamName);
    return S_OK;
  }

  __override ~DxcArgsFileSystemImpl() { };
  __override BOOL FindNextFileW(
    _In_   HANDLE hFindFile,
    _Out_  LPWIN32_FIND_DATAW lpFindFileData) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }

  __override HANDLE FindFirstFileW(
    _In_   LPCWSTR lpFileName,
    _Out_  LPWIN32_FIND_DATAW lpFindFileData) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }

  __override void FindClose(HANDLE findHandle) throw() {
    __debugbreak();
  }

  __override HANDLE CreateFileW(
    _In_      LPCWSTR lpFileName,
    _In_      DWORD dwDesiredAccess,
    _In_      DWORD dwShareMode,
    _In_      DWORD dwCreationDisposition,
    _In_      DWORD dwFlagsAndAttributes) throw() {
    DXTRACE_FMT_APIFS("DxcArgsFileSystem::CreateFileW %S\n", lpFileName);
    DWORD findError;
    {
      std::wstring FileNameStore; // The destructor might release and set LastError to success.
      MakeAbsoluteOrCurDirRelativeW(lpFileName, FileNameStore);

      // Check for a match to the output file.
      if (m_pOutputStreamName != nullptr &&
        0 == wcscmp(lpFileName, m_pOutputStreamName)) {
        return OutputHandle.Handle;
      }

      HANDLE dirHandle = TryFindDirHandle(lpFileName);
      if (dirHandle != INVALID_HANDLE_VALUE) {
        return dirHandle;
      }

      size_t includedIndex;
      findError = TryFindOrOpen(lpFileName, includedIndex);
      if (findError == ERROR_SUCCESS) {
        return IncludedFileIndexToHandle(includedIndex);
      }
    }

    SetLastError(findError);
    return INVALID_HANDLE_VALUE;
  }

  __override BOOL SetFileTime(_In_ HANDLE hFile,
    _In_opt_  const FILETIME *lpCreationTime,
    _In_opt_  const FILETIME *lpLastAccessTime,
    _In_opt_  const FILETIME *lpLastWriteTime) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }

  __override BOOL GetFileInformationByHandle(_In_ HANDLE hFile, _Out_ LPBY_HANDLE_FILE_INFORMATION lpFileInformation) throw() {
    DxcArgsHandle argsHandle(hFile);
    ZeroMemory(lpFileInformation, sizeof(*lpFileInformation));
    lpFileInformation->nFileIndexLow = (DWORD)(uintptr_t)hFile;
    if (argsHandle.IsFileKind()) {
      IncludedFile &file = HandleToIncludedFile(hFile);
      lpFileInformation->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      lpFileInformation->nFileSizeLow = file.Blob->GetBufferSize();
      return TRUE;
    }
    if (argsHandle == OutputHandle) {
      lpFileInformation->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      STATSTG stat;
      HRESULT hr = m_pOutputStream->Stat(&stat, STATFLAG_NONAME);
      if (FAILED(hr)) {
        SetLastError(ERROR_IO_DEVICE);
        return FALSE;
      }
      lpFileInformation->nFileSizeLow = stat.cbSize.LowPart;
      return TRUE;
    }
    else if (argsHandle.IsDirHandle()) {
      lpFileInformation->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
      lpFileInformation->nFileIndexHigh = 1;
      return TRUE;
    }

    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }

  __override DWORD GetFileType(_In_ HANDLE hFile) throw() {
    DxcArgsHandle argsHandle(hFile);
    if (argsHandle.IsStdHandle()) {
      return FILE_TYPE_CHAR;
    }
    // Every other known handle is of type disk.
    if (!argsHandle.IsSpecialUnknown()) {
      return FILE_TYPE_DISK;
    }

    SetLastError(ERROR_NOT_FOUND);
    return FILE_TYPE_UNKNOWN;
  }

  __override BOOL CreateHardLinkW(_In_ LPCWSTR lpFileName, _In_ LPCWSTR lpExistingFileName) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override BOOL MoveFileExW(_In_ LPCWSTR lpExistingFileName, _In_opt_ LPCWSTR lpNewFileName, _In_ DWORD dwFlags) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override DWORD GetFileAttributesW(_In_ LPCWSTR lpFileName) throw() {
    DXTRACE_FMT_APIFS("DxcArgsFileSystem::GetFileAttributesW %S\n", lpFileName);
    DWORD findError;
    {
      std::wstring FileNameStore; // The destructor might release and set LastError to success.
      MakeAbsoluteOrCurDirRelativeW(lpFileName, FileNameStore);
      size_t sourceNameLen = wcslen(m_pSourceName);
      size_t fileNameLen = wcslen(lpFileName);

      // Check for a match to the source.
      if (fileNameLen == sourceNameLen) {
        if (0 == wcsncmp(m_pSourceName, lpFileName, fileNameLen)) {
          return FILE_ATTRIBUTE_NORMAL;
        }
      }

      // Check for a perfect match to the output.
      if (m_pOutputStreamName != nullptr &&
        0 == wcscmp(m_pOutputStreamName, lpFileName)) {
        return FILE_ATTRIBUTE_NORMAL;
      }

      if (TryFindDirHandle(lpFileName) != INVALID_HANDLE_VALUE) {
        return FILE_ATTRIBUTE_DIRECTORY;
      }

      size_t includedIndex;
      findError = TryFindOrOpen(lpFileName, includedIndex);
      if (findError == ERROR_SUCCESS) {
        return FILE_ATTRIBUTE_NORMAL;
      }
    }

    SetLastError(findError);
    return INVALID_FILE_ATTRIBUTES;
  }

  __override BOOL CloseHandle(_In_ HANDLE hObject) throw() {
    // Not actually closing handle. Would allow improper usage, but simplifies
    // query/open/usage patterns.
    if (IsKnownHandle(hObject)) {
      return TRUE;
    }

    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  __override BOOL DeleteFileW(_In_ LPCWSTR lpFileName) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override BOOL RemoveDirectoryW(_In_ LPCWSTR lpFileName) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override BOOL CreateDirectoryW(_In_ LPCWSTR lpPathName) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  _Success_(return != 0 && return < nBufferLength)
    __override DWORD GetCurrentDirectoryW(_In_ DWORD nBufferLength, _Out_writes_to_opt_(nBufferLength, return +1) LPWSTR lpBuffer) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  _Success_(return != 0 && return < nSize)
    __override DWORD GetMainModuleFileNameW(__out_ecount_part(nSize, return +1) LPWSTR lpFilename, DWORD nSize) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override DWORD GetTempPathW(DWORD nBufferLength, _Out_writes_to_opt_(nBufferLength, return +1) LPWSTR lpBuffer) {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override BOOLEAN CreateSymbolicLinkW(_In_ LPCWSTR lpSymlinkFileName, _In_ LPCWSTR lpTargetFileName, DWORD dwFlags) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override bool SupportsCreateSymbolicLink() throw() {
    return false;
  }
  __override BOOL ReadFile(_In_ HANDLE hFile, _Out_bytecap_(nNumberOfBytesToRead) LPVOID lpBuffer, _In_ DWORD nNumberOfBytesToRead, _Out_opt_ LPDWORD lpNumberOfBytesRead) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }
  __override HANDLE CreateFileMappingW(
    _In_      HANDLE hFile,
    _In_      DWORD flProtect,
    _In_      DWORD dwMaximumSizeHigh,
    _In_      DWORD dwMaximumSizeLow) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return INVALID_HANDLE_VALUE;
  }
  __override LPVOID MapViewOfFile(
    _In_  HANDLE hFileMappingObject,
    _In_  DWORD dwDesiredAccess,
    _In_  DWORD dwFileOffsetHigh,
    _In_  DWORD dwFileOffsetLow,
    _In_  SIZE_T dwNumberOfBytesToMap) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return nullptr;
  }
  __override BOOL UnmapViewOfFile(_In_ LPCVOID lpBaseAddress) throw() {
    SetLastError(ERROR_NOT_CAPABLE);
    return FALSE;
  }

  // Console APIs.
  __override bool FileDescriptorIsDisplayed(int fd) throw() {
    return false;
  }
  __override unsigned GetColumnCount(DWORD nStdHandle) throw() {
    return 80;
  }
  __override unsigned GetConsoleOutputTextAttributes() throw() {
    return 0;
  }
  __override void SetConsoleOutputTextAttributes(unsigned) throw() {
    __debugbreak();
  }
  __override void ResetConsoleOutputTextAttributes() throw() {
    __debugbreak();
  }

  // CRT APIs - handles and file numbers can be mapped directly.
  HANDLE HandleFromFD(int fd) const {
    if (fd == STDOUT_FILENO) return StdOutHandle.Handle;
    if (fd == STDERR_FILENO) return StdErrHandle.Handle;
    return (HANDLE)(uintptr_t)(fd);
  }
  __override int open_osfhandle(intptr_t osfhandle, int flags) throw() {
    DxcArgsHandle H((HANDLE)osfhandle);
    if (H == StdOutHandle.Handle) return STDOUT_FILENO;
    if (H == StdErrHandle.Handle) return STDERR_FILENO;
    return (int)(intptr_t)H.Handle;
  }
  __override intptr_t get_osfhandle(int fd) throw() {
    return (intptr_t)HandleFromFD(fd);
  }
  __override int close(int fd) throw() {
    return 0;
  }
  __override long lseek(int fd, long offset, int origin) throw() {
    CComPtr<IStream> stream;
    GetStreamForFD(fd, &stream);
    if (stream == nullptr) {
      errno = EBADF;
      return -1;
    }

    LARGE_INTEGER li;
    li.LowPart = offset;
    li.HighPart = 0;
    ULARGE_INTEGER newOffset;
    HRESULT hr = stream->Seek(li, origin, &newOffset);
    if (FAILED(hr)) {
      errno = EINVAL;
      return -1;
    }

    return newOffset.LowPart;
  }
  __override int setmode(int fd, int mode) throw() {
    return 0;
  }
  __override errno_t resize_file(_In_ LPCWSTR path, uint64_t size) throw() {
    return 0;
  }
  __override int Read(int fd, _Out_bytecap_(count) void* buffer, unsigned int count) throw() {
    CComPtr<IStream> stream;
    GetStreamForFD(fd, &stream);
    if (stream == nullptr) {
      errno = EBADF;
      return -1;
    }

    ULONG cbRead;
    HRESULT hr = stream->Read(buffer, count, &cbRead);
    if (FAILED(hr)) {
      errno = EIO;
      return -1;
    }

    return (int)cbRead;
  }
  __override int Write(int fd, _In_bytecount_(count) const void* buffer, unsigned int count) throw() {
    CComPtr<IStream> stream;
    GetStreamForFD(fd, &stream);
    if (stream == nullptr) {
      errno = EBADF;
      return -1;
    }

#ifdef _DEBUG
    if (fd == STDERR_FILENO) {
        char* copyWithNull = new char[count+1];
        strncpy(copyWithNull, (char*)buffer, count);
        copyWithNull[count] = '\0';
        OutputDebugStringA(copyWithNull);
        delete[] copyWithNull;
    }
#endif

    ULONG written;
    HRESULT hr = stream->Write(buffer, count, &written);
    if (FAILED(hr)) {
      errno = EIO;
      return -1;
    }

    return (int)written;
  }
};
}

namespace dxcutil {

DxcArgsFileSystem *
CreateDxcArgsFileSystem(
    _In_ IDxcBlob *pSource, _In_ LPCWSTR pSourceName,
    _In_opt_ IDxcIncludeHandler *pIncludeHandler) {
  return new DxcArgsFileSystemImpl(pSource, pSourceName, pIncludeHandler);
}

} // namespace dxcutil