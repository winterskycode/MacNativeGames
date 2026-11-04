/* Registries in a named data section: Mach-O reads them with getsectiondata; ELF (Android) uses the linker's
   __start_/__stop_ symbols (the section name must be a C identifier; `retain` keeps entries under --gc-sections). */
#pragma once
#ifdef __ANDROID__
#define TOTK_SECTION(name) used, retain, section(#name)
#define TOTK_SECTION_DATA(name, size_ptr) ({ extern const char __start_##name[] __attribute__((weak)), __stop_##name[] __attribute__((weak)); \
    *(size_ptr) = (unsigned long)(__stop_##name - __start_##name); (const void *)__start_##name; })
#else
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#define TOTK_SECTION(name) used, section("__DATA," #name)
#define TOTK_SECTION_DATA(name, size_ptr) ((const void *)getsectiondata(&_mh_execute_header, "__DATA", #name, (size_ptr)))
#endif
