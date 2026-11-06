#include "Crypto.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <memory>

namespace p2p
{
    std::string Hex(uint8_t const* data, size_t size)
    {
        static char const digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(size * 2);
        for (size_t i = 0; i < size; ++i)
        {
            out.push_back(digits[data[i] >> 4]);
            out.push_back(digits[data[i] & 15]);
        }
        return out;
    }

    bool FromHex(std::string const& text, Bytes& out)
    {
        out.clear();
        if (text.size() % 2)
            return false;
        auto value = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        for (size_t i = 0; i < text.size(); i += 2)
        {
            int hi = value(text[i]), lo = value(text[i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            out.push_back(static_cast<uint8_t>(hi * 16 + lo));
        }
        return true;
    }
}

namespace p2p::crypto
{
    namespace
    {
        struct KeyDeleter { void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); } };
        using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
        struct CtxDeleter { void operator()(EVP_PKEY_CTX* c) const { EVP_PKEY_CTX_free(c); } };
        using Ctx = std::unique_ptr<EVP_PKEY_CTX, CtxDeleter>;
        struct MdDeleter { void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); } };
        using MdCtx = std::unique_ptr<EVP_MD_CTX, MdDeleter>;
        struct CipherDeleter { void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); } };
        using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherDeleter>;

        KeyPair NewKey(int type)
        {
            KeyPair pair;
            Ctx ctx(EVP_PKEY_CTX_new_id(type, nullptr));
            EVP_PKEY* raw = nullptr;
            if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1 || EVP_PKEY_keygen(ctx.get(), &raw) != 1)
                return pair;
            Key key(raw);
            size_t n = 32;
            EVP_PKEY_get_raw_private_key(key.get(), pair.priv.data(), &n);
            n = 32;
            EVP_PKEY_get_raw_public_key(key.get(), pair.pub.data(), &n);
            return pair;
        }

        KeyPair KeyFromPrivate(int type, Key32 const& priv)
        {
            KeyPair pair;
            pair.priv = priv;
            Key key(EVP_PKEY_new_raw_private_key(type, nullptr, priv.data(), 32));
            size_t n = 32;
            if (key)
                EVP_PKEY_get_raw_public_key(key.get(), pair.pub.data(), &n);
            return pair;
        }
    }

    void Random(uint8_t* out, size_t n) { RAND_bytes(out, static_cast<int>(n)); }
    Bytes Random(size_t n) { Bytes b(n); Random(b.data(), n); return b; }
    Id128 RandomId() { Id128 id{}; Random(id.data(), 16); return id; }

    Key32 Sha256(uint8_t const* data, size_t n)
    {
        Key32 out{};
        ::SHA256(data, n, out.data());
        return out;
    }

    Key32 Hmac(Key32 const& key, uint8_t const* data, size_t n)
    {
        Key32 out{};
        unsigned int size = 32;
        HMAC(EVP_sha256(), key.data(), 32, data, n, out.data(), &size);
        return out;
    }

    bool ConstantTimeEqual(uint8_t const* a, uint8_t const* b, size_t n)
    {
        uint8_t diff = 0;
        for (size_t i = 0; i < n; ++i)
            diff |= a[i] ^ b[i];
        return diff == 0;
    }

    Bytes Hkdf(uint8_t const* secret, size_t secretSize, Bytes const& salt, std::string const& info, size_t length)
    {
        Bytes out(length);
        Ctx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
        size_t n = length;
        if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) != 1 ||
            EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), salt.data(), static_cast<int>(salt.size())) != 1 ||
            EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), secret, static_cast<int>(secretSize)) != 1 ||
            EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), reinterpret_cast<uint8_t const*>(info.data()), static_cast<int>(info.size())) != 1 ||
            EVP_PKEY_derive(ctx.get(), out.data(), &n) != 1)
            return Bytes(length, 0);
        return out;
    }

    Key32 Pbkdf2(std::string const& password, Bytes const& salt, int iterations)
    {
        Key32 out{};
        if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()), iterations, EVP_sha256(), 32, out.data()) != 1)
            return Key32{};
        return out;
    }

    KeyPair NewSigningKey() { return NewKey(EVP_PKEY_ED25519); }
    KeyPair SigningKeyFrom(Key32 const& priv) { return KeyFromPrivate(EVP_PKEY_ED25519, priv); }

    Bytes Sign(Key32 const& priv, Bytes const& message)
    {
        Key key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv.data(), 32));
        MdCtx md(EVP_MD_CTX_new());
        Bytes sig(64);
        size_t n = 64;
        if (!key || !md || EVP_DigestSignInit(md.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
            EVP_DigestSign(md.get(), sig.data(), &n, message.data(), message.size()) != 1)
            return {};
        return sig;
    }

    bool Verify(Key32 const& pub, Bytes const& message, uint8_t const* signature, size_t signatureSize)
    {
        if (signatureSize != 64)
            return false;
        Key key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), 32));
        MdCtx md(EVP_MD_CTX_new());
        return key && md && EVP_DigestVerifyInit(md.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
               EVP_DigestVerify(md.get(), signature, signatureSize, message.data(), message.size()) == 1;
    }

    KeyPair NewAgreementKey() { return NewKey(EVP_PKEY_X25519); }

    KeyPair AgreementKeyOf(Key32 const& signingPriv)
    {
        Bytes seed = Hkdf(signingPriv.data(), 32, {}, "p2p agreement key", 32);
        Key32 priv{};
        memcpy(priv.data(), seed.data(), 32);
        return KeyFromPrivate(EVP_PKEY_X25519, priv);
    }

    bool Agree(Key32 const& priv, Key32 const& peerPub, Key32& secret)
    {
        Key mine(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), 32));
        Key theirs(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPub.data(), 32));
        if (!mine || !theirs)
            return false;
        Ctx ctx(EVP_PKEY_CTX_new(mine.get(), nullptr));
        size_t n = 32;
        if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_derive_set_peer(ctx.get(), theirs.get()) != 1 ||
            EVP_PKEY_derive(ctx.get(), secret.data(), &n) != 1)
            return false;
        return n == 32;
    }

    Bytes Seal(Key32 const& key, uint8_t const nonce[12], Bytes const& aad, Bytes const& plain)
    {
        CipherCtx ctx(EVP_CIPHER_CTX_new());
        Bytes out(plain.size() + 16);
        int n = 0, total = 0;
        if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, key.data(), nonce) != 1)
            return {};
        if (!aad.empty() && EVP_EncryptUpdate(ctx.get(), nullptr, &n, aad.data(), static_cast<int>(aad.size())) != 1)
            return {};
        if (!plain.empty())
        {
            if (EVP_EncryptUpdate(ctx.get(), out.data(), &n, plain.data(), static_cast<int>(plain.size())) != 1)
                return {};
            total = n;
        }
        if (EVP_EncryptFinal_ex(ctx.get(), out.data() + total, &n) != 1)
            return {};
        total += n;
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, 16, out.data() + total) != 1)
            return {};
        out.resize(total + 16);
        return out;
    }

    bool Open(Key32 const& key, uint8_t const nonce[12], Bytes const& aad, uint8_t const* sealed, size_t size, Bytes& plain)
    {
        if (size < 16)
            return false;
        CipherCtx ctx(EVP_CIPHER_CTX_new());
        size_t body = size - 16;
        plain.assign(body, 0);
        int n = 0, total = 0;
        if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, key.data(), nonce) != 1)
            return false;
        if (!aad.empty() && EVP_DecryptUpdate(ctx.get(), nullptr, &n, aad.data(), static_cast<int>(aad.size())) != 1)
            return false;
        if (body)
        {
            if (EVP_DecryptUpdate(ctx.get(), plain.data(), &n, sealed, static_cast<int>(body)) != 1)
                return false;
            total = n;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, 16, const_cast<uint8_t*>(sealed + body)) != 1)
            return false;
        if (EVP_DecryptFinal_ex(ctx.get(), plain.data() + total, &n) != 1)
        {
            plain.clear();
            return false;
        }
        return true;
    }
}
