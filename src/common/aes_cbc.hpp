// AES-128-CBC payload cipher for the "hybrid" mode of Bambu printers, i.e.
// when the printer is NOT in LAN-only mode: MQTT payloads on the
// device/<serial>/request and device/<serial>/report topics are then
// AES-128-CBC encrypted with a key derived from the owner's LAN access code
// (public protocol: bambu_mqtt_control, "MQTT encryption"). In LAN-only mode
// the same topics carry plaintext.
//
// This is interoperability, not circumvention: the key material is the access
// code the owner reads on their own printer screen, nothing extracted from
// Bambu's closed-source software.
#pragma once

#include <optional>
#include <string>

namespace bbl_lan {

// Key AND iv, identical: the ASCII access code right-padded with '0' to 16
// characters (codes are normally 8 decimal digits); longer codes truncated.
std::string bambu_cipher_key(const std::string& access_code);

// AES-128-CBC + PKCS7 over key16 (iv = key16), result base64 (one line).
std::string aes128_cbc_encrypt_b64(const std::string& plain, const std::string& key16);

// nullopt unless b64 is valid base64, decrypts with valid PKCS7 padding and
// yields printable JSON-looking text (starts with '{' or '['): that extra
// check lets callers auto-detect the mode without being fooled by random
// base64 payloads.
std::optional<std::string> aes128_cbc_decrypt_b64(const std::string& b64, const std::string& key16);

} // namespace bbl_lan
