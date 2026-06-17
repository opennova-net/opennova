# OpenBSD bcrypt vendored from pyca/bcrypt 3.2.0's _csrc tree.
# License: ISC (see comment headers in bcrypt.c, blf.c, timingsafe_bcmp.c).
#
# We only build the verify-side bits: bcrypt_hashpass + Blowfish +
# timingsafe_bcmp. bcrypt_gensalt is intentionally absent — it requires
# arc4random_buf which doesn't exist on Windows. Hash generation for new
# users is delegated to the standalone tools/gen-bcrypt helper or done
# server-side via libs/novacrypto's bcrypt::hashpw wrapper which uses
# std::random_device for the salt. (See libs/novacrypto/bcrypt.cpp.)

add_library(opennova_bcrypt STATIC
    bcrypt.c
    blf.c
    timingsafe_bcmp.c
)

target_include_directories(opennova_bcrypt
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# Disable the bcrypt_gensalt forward decl — we don't include it. The
# decl is harmless but keeps the source unmodified.
if(MSVC)
    target_compile_options(opennova_bcrypt PRIVATE
        /wd4101  # unreferenced local variable
        /wd4244  # conversion narrowing
        /wd4267  # size_t -> u_int8_t narrowing
    )
endif()
