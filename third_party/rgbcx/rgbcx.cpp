// The one translation unit holding rgbcx's implementation (third_party/rgbcx/CMakeLists.txt).
// The implementation calls memset, memcpy, fabs and fabsf without including their headers; MSVC's
// standard headers bring them in transitively, GCC's and Clang's do not, so they are included here
// first and the vendored header is left as it is.
#include <cmath>
#include <cstring>

#define RGBCX_IMPLEMENTATION
#include <rgbcx.h>
