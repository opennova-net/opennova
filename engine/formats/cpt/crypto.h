#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova {

void rotate_encrypt(uint8_t *buf, size_t len);
void rotate_decrypt(uint8_t *buf, size_t len);

} // namespace opennova
