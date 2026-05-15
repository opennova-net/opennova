#ifndef OPENNOVA_OBJECT_THREE_DI_POLICY_H
#define OPENNOVA_OBJECT_THREE_DI_POLICY_H

#include <stdint.h>

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define OBJECT_THREE_DI_POLICY_EXPORT __declspec(dllexport)
#  else
#    define OBJECT_THREE_DI_POLICY_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define OBJECT_THREE_DI_POLICY_EXPORT __attribute__((visibility("default")))
#  else
#    define OBJECT_THREE_DI_POLICY_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Object3diPolicyStatus {
    OBJECT_3DI_POLICY_OK = 0,
    OBJECT_3DI_POLICY_INVALID_ARGUMENT = -1,
    OBJECT_3DI_POLICY_READ_FAILED = -2,
    OBJECT_3DI_POLICY_IO_FAILED = -3,
    OBJECT_3DI_POLICY_COMPARE_FAILED = 1,
    OBJECT_3DI_POLICY_VALIDATION_FAILED = 2,
} Object3diPolicyStatus;

enum {
    OBJECT_3DI_COMPARE_RELAX_GEOMETRY = 1u << 0,
};

OBJECT_THREE_DI_POLICY_EXPORT Object3diPolicyStatus
object_3di_compare_files(const char *expected_path,
                         const char *actual_path,
                         uint32_t flags,
                         const char *report_path);

OBJECT_THREE_DI_POLICY_EXPORT Object3diPolicyStatus
object_3di_validate_geometry_chunks(const char *path,
                                    uint32_t flags,
                                    const char *report_path);

#ifdef __cplusplus
}
#endif

#endif // OPENNOVA_OBJECT_THREE_DI_POLICY_H
