#include "aes_cbc.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <memory>

namespace bbl_lan {
namespace {

constexpr int kKeyLen = 16;

// RFC 4648 base64 without line breaks. Returns nullopt on any invalid
// character or bad length (we only ever decode our own/printer base64, so no
// tolerance for missing padding).
std::optional<std::string> b64_decode(const std::string& in)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string s = in;
    while (!s.empty() && s.back() == '=')
        s.pop_back();
    if (in.size() % 4 != 0 || s.empty())
        return std::nullopt;
    // An '=' may only appear as trailing padding.
    if (const size_t pad = in.find('='); pad != std::string::npos && in.find_first_not_of('=', pad) != std::string::npos)
        return std::nullopt;
    std::string out;
    out.reserve(s.size() * 3 / 4 + 3);
    uint32_t acc   = 0;
    int      bits  = 0;
    for (const char c : s) {
        const int v = val(c);
        if (v < 0)
            return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

bool looks_like_json_text(const std::string& s)
{
    if (s.empty() || (s.front() != '{' && s.front() != '['))
        return false;
    return std::none_of(s.begin(), s.end(), [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u < 0x20 && u != '\t' && u != '\n' && u != '\r';
    });
}

} // namespace

std::string bambu_cipher_key(const std::string& access_code)
{
    std::string key = access_code.substr(0, kKeyLen);
    key.resize(kKeyLen, '0');
    return key;
}

std::string aes128_cbc_encrypt_b64(const std::string& plain, const std::string& key16)
{
    if (key16.size() != kKeyLen)
        return {};
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx)
        return {};
    const auto* key = reinterpret_cast<const unsigned char*>(key16.data());
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, key) != 1)
        return {};
    std::string out(plain.size() + EVP_MAX_BLOCK_LENGTH, '\0');
    int         n = 0, total = 0;
    if (EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(&out[0]), &n,
                          reinterpret_cast<const unsigned char*>(plain.data()), static_cast<int>(plain.size())) != 1)
        return {};
    total = n;
    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&out[0]) + total, &n) != 1)
        return {};
    total += n;
    out.resize(static_cast<size_t>(total));

    std::string b64(static_cast<size_t>((out.size() + 2) / 3) * 4 + 1, '\0');
    const int   wrote = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&b64[0]),
                                        reinterpret_cast<const unsigned char*>(out.data()), static_cast<int>(out.size()));
    if (wrote <= 0)
        return {};
    b64.resize(static_cast<size_t>(wrote));
    return b64;
}

std::optional<std::string> aes128_cbc_decrypt_b64(const std::string& b64, const std::string& key16)
{
    if (key16.size() != kKeyLen || b64.size() % 4 != 0)
        return std::nullopt;
    const auto raw = b64_decode(b64);
    if (!raw || raw->empty() || raw->size() % 16 != 0)
        return std::nullopt;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx)
        return std::nullopt;
    const auto* key = reinterpret_cast<const unsigned char*>(key16.data());
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, key) != 1)
        return std::nullopt;
    std::string out(raw->size() + EVP_MAX_BLOCK_LENGTH, '\0');
    int         n = 0, total = 0;
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(&out[0]), &n,
                          reinterpret_cast<const unsigned char*>(raw->data()), static_cast<int>(raw->size())) != 1)
        return std::nullopt;
    total = n;
    // Wrong key almost always fails here (bad PKCS7 padding).
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&out[0]) + total, &n) != 1)
        return std::nullopt;
    total += n;
    out.resize(static_cast<size_t>(total));
    if (!looks_like_json_text(out))
        return std::nullopt;
    return out;
}

} // namespace bbl_lan
