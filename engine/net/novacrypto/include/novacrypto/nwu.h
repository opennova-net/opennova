#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace opennova {

// NWU cipher — the 4-phase obfuscation used on selected NAPI control
// messages (gate probes, ping probes). Keyed on an ASCII string.
//
// Witnessed byte-exact in jodemo.exe:
//   Crypto_DecryptBuffer@0x5e5230  (decrypt entry)
//   PFF_EncryptBuffer@0x5e5300     (encrypt entry, despite kong's 'PFF_' prefix)
//   Crypto_ComputeSeed@0x5e5160
//   CLCG_Init@0x5f3170             (LCG magic constant 78665521 = 0x04B05731)
//   Buffer_AddWithKeyString@0x5e5080   / SubtractWithKeyString@0x5e50f0
//   Buffer_ReverseInPlace@0x5e4fc0
//   Buffer_AddWithProgression@0x5e5000 / SubtractWithProgression@0x5e5040
//   Buffer_ScrambleWithLCG@0x62a8b0
//   Crypto_ApplyPRNG@0x5e51f0
//
// Re-grilled byte-exact in retail Jointops.exe (2026-06-11, grill wave 3) —
// demo and retail ciphers are identical:
//   NapiNP_EncryptBuffer@0x6187b0   (ADD chain == our nwu_decrypt)
//   NapiNP_DecryptBuffer@0x618880   (SUB chain == our nwu_encrypt)
//   NapiNP_ComputeKeySeed@0x618430  (== nwu_compute_seed; null->3252, sum(i+key[i]^2)+len+50)
//   NapiPRNG_Init@0x62e430          (LCG mult 78665521; struct {state@0,mult@4,counter@8})
//   Crypto_AddWithKey@0x6182d0      (buf[i] += key[i % klen])
//   Crypto_AddProgressive@0x618250  (buf[i] += seed+i; seed += step)
//   Crypto_AddLCG@0x6183b0          (state = u16(mult*state+1); buf[i] += state)
//   NapiNP_ReverseBuffer@0x618210
// Name-swap confirmed at the byte level: retail *EncryptBuffer* applies the
// ADD chain, which is our nwu_decrypt (see reference_nwu_names_swapped).
//
// Key string used for the gate ping probes (sub_5F5420):
inline constexpr const char *NWU_GATE_KEY = "GATEAPI";

// LCG multiplier lifted from CLCG_Init@0x5f3170 / NapiPRNG_Init@0x62e430.
// 78665521 == 0x04B05731; only the low 16 bits (0x5731) participate in the
// per-step multiply. (Prior comments mislabeled this as 0x04B02631.)
inline constexpr uint32_t NWU_LCG_MAGIC = 78665521u; // 0x04B05731

// Seed-derivation step (Crypto_ComputeSeed): accumulate (i + key[i]^2) plus
// len + 50. Returns 3252 when the key is null (original's sentinel default).
uint32_t nwu_compute_seed(std::string_view key);

// Encrypt / decrypt in place. Returns the number of bytes processed (== len
// on success, 0 on invalid input). Calling `nwu_decrypt` after `nwu_encrypt`
// with the same key restores the original bytes.
size_t nwu_encrypt(uint8_t *buf, size_t len, std::string_view key);
size_t nwu_decrypt(uint8_t *buf, size_t len, std::string_view key);

} // namespace opennova
