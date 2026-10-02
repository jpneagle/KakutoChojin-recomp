// Windows/Xbox base types for host code.
//
// The Xbox APIs are declared with Windows types (DWORD, LONG, LARGE_INTEGER,
// WINAPI...). On Windows they come from <windows.h>; elsewhere this header
// defines them with the same sizes as on the Xbox (LONG is 32 bits). Portable
// host code uses only what is defined here; code that needs the Win32 API
// includes <windows.h> itself and is built on Windows only.
//
// KT_PORTABLE_CHECK uses the non-Windows definitions on Windows too, so the
// portable sources can be syntax-checked with MSVC (tools/portable_check.bat).
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(KT_PORTABLE_CHECK)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef LONG NTSTATUS;
#define KT_WIN32_API 1  // the Win32 API is available

#else

#define KT_WIN32_API 0

typedef void VOID;
typedef char CHAR;
typedef uint8_t UCHAR, BYTE, BOOLEAN;
typedef int16_t SHORT;
typedef uint16_t USHORT, WORD;
typedef char16_t WCHAR;
typedef int32_t INT, LONG, BOOL;
typedef uint32_t UINT, ULONG, DWORD, ACCESS_MASK;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef int32_t HRESULT;
typedef int32_t NTSTATUS;
typedef void* PVOID;
typedef LONG* PLONG;
typedef ULONG* PULONG;
typedef DWORD* LPDWORD;
typedef size_t SIZE_T;

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

struct RECT {
    LONG left, top, right, bottom;
};
struct POINT {
    LONG x, y;
};
struct GUID {
    uint32_t Data1;
    uint16_t Data2, Data3;
    uint8_t Data4[8];
};

// Calling conventions only matter on 32-bit x86, which is Windows-only here.
#define WINAPI
#define NTAPI
#define APIENTRY
#ifndef _MSC_VER
#define __stdcall
#define __fastcall
#define __cdecl
#endif

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define S_OK ((HRESULT)0)
#define E_ABORT ((HRESULT)0x80004004)
#define E_INVALIDARG ((HRESULT)0x80070057)

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400

#define ERROR_SUCCESS 0L
#define ERROR_DEVICE_NOT_CONNECTED 1167L

#endif
