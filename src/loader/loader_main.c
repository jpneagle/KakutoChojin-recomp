/* kt_loader.exe: a placeholder image fixed at 0x10000, the base every XBE is
 * linked for. Its oversized .bss makes the loader own the address range the
 * title needs; kt_host.dll then overwrites the whole image with the XBE, so
 * nothing here may run after KtMain is entered (it never returns). */
#include <windows.h>

__declspec(dllimport) void __stdcall KtMain(void);

/* 0x10000 + headers/code + this ~= 16 MB of address space. */
static volatile unsigned char g_xbox_image_reserve[0x00F00000];

void __cdecl LoaderEntry(void) {
    g_xbox_image_reserve[0] = 0;
    KtMain();
}
