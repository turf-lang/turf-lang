// BundledLinkWindowsShims.cpp -- Windows only, built only with TURF_EMBED_LLD.
//
// MSYS2's static lld archives (liblldCOFF.a, liblldMinGW.a) are built the way
// MSYS2 builds everything: against shared libraries, with every call into them
// compiled as an indirect call through an import-table slot, `__imp_<name>`. A
// *static* link finds no such slot, so the embedded linker's link failed with
// 17 undefined `__imp_` symbols, in two groups, found by building turfc.exe
// against MSYS2's mingw-w64-ucrt-x86_64-lld 22.1.8-3 (REL-008 stage 4).
//
// 1. libxml2, 16 functions, referenced by LLVM's Windows manifest merger,
//    which lld's COFF driver links for its /manifest handling. turfc never
//    asks lld for a manifest, so these are never called; linking libxml2 for
//    them would add a DLL to the archive (libxml2-16.dll, which pulls in
//    libiconv, zlib and liblzma), exactly what REL-008 removes. They are WEAK, so
//    if a real import library is ever linked, it wins. A call, were one ever to
//    happen, stops with a message instead of jumping through a null pointer.
// 2. llvm::demangle(std::string_view), a real function of LLVM's own static
//    archive, referenced the same indirect way (lld demangles symbol names in
//    its diagnostics). Its slot points at the real function.
#include "llvm/Demangle/Demangle.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {
[[noreturn]] void TurfXmlUnavailable() {
  std::fputs("turfc: internal error: the embedded linker tried to merge a "
             "Windows manifest, which this build does not support "
             "(libxml2 is not linked)\n",
             stderr);
  std::abort();
}
} // namespace

#define TURF_XML_SLOT(Name)                                                    \
  extern "C" __attribute__((weak)) void *__imp_##Name =                        \
      reinterpret_cast<void *>(&TurfXmlUnavailable)

TURF_XML_SLOT(xmlAddChild);
TURF_XML_SLOT(xmlCopyNamespace);
TURF_XML_SLOT(xmlDocDumpFormatMemoryEnc);
TURF_XML_SLOT(xmlDocGetRootElement);
TURF_XML_SLOT(xmlDocSetRootElement);
TURF_XML_SLOT(xmlFree);
TURF_XML_SLOT(xmlFreeDoc);
TURF_XML_SLOT(xmlFreeNode);
TURF_XML_SLOT(xmlFreeNs);
TURF_XML_SLOT(xmlNewDoc);
TURF_XML_SLOT(xmlNewNs);
TURF_XML_SLOT(xmlNewProp);
TURF_XML_SLOT(xmlReadMemory);
TURF_XML_SLOT(xmlSetGenericErrorFunc);
TURF_XML_SLOT(xmlStrdup);
TURF_XML_SLOT(xmlUnlinkNode);

// `llvm::demangle[abi:cxx11](std::basic_string_view<char>)`
extern "C" void *__imp__ZN4llvm8demangleB5cxx11ESt17basic_string_viewIcSt11char_traitsIcEE =
    reinterpret_cast<void *>(
        static_cast<std::string (*)(std::string_view)>(&llvm::demangle));
