// Shared C-ABI export annotation for the opennova libraries.
//
// Pure preprocessor and C-safe: this header is included from headers that are
// consumed as plain C (ctypes-facing FFI headers), so it must not declare
// anything. Each library keeps its historical <DOMAIN>_EXPORT macro by
// aliasing it to OPENNOVA_API, so declarations and the exported symbol
// surface are unchanged.

#ifndef OPENNOVA_IO_EXPORT_H
#define OPENNOVA_IO_EXPORT_H

#ifndef OPENNOVA_API
#  ifdef _WIN32
#    ifdef OPENNOVA_SHARED_EXPORTS
#      define OPENNOVA_API __declspec(dllexport)
#    else
#      define OPENNOVA_API
#    endif
#  else
#    ifdef OPENNOVA_SHARED_EXPORTS
#      define OPENNOVA_API __attribute__((visibility("default")))
#    else
#      define OPENNOVA_API
#    endif
#  endif
#endif

#endif // OPENNOVA_IO_EXPORT_H
