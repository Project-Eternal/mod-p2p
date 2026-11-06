/*
 * The cryptography of mod-p2p, all from OpenSSL 3 (the library the server already links): Ed25519 signatures, X25519 key agreement,
 * HKDF-SHA256, ChaCha20-Poly1305 and HMAC-SHA256. Nothing else in the module (and nothing in a host) touches key material.
 */
#pragma once
#include "Codec.h"

namespace p2p::crypto
{
    struct KeyPair { Key32 priv{}, pub{}; };

    void Random(uint8_t* out, size_t n);
    Bytes Random(size_t n);
    Id128 RandomId();

    Key32 Sha256(uint8_t const* data, size_t n);
    inline Key32 Sha256(Bytes const& b) { return Sha256(b.data(), b.size()); }

    Key32 Hmac(Key32 const& key, uint8_t const* data, size_t n);
    inline Key32 Hmac(Key32 const& key, Bytes const& b) { return Hmac(key, b.data(), b.size()); }
    bool ConstantTimeEqual(uint8_t const* a, uint8_t const* b, size_t n);

    // HKDF-SHA256 (RFC 5869): the output keying material of 'length' bytes
    Bytes Hkdf(uint8_t const* secret, size_t secretSize, Bytes const& salt, std::string const& info, size_t length);
    inline Key32 Hkdf32(Bytes const& secret, Bytes const& salt, std::string const& info)
    {
        Bytes out = Hkdf(secret.data(), secret.size(), salt, info, 32);
        Key32 k{};
        memcpy(k.data(), out.data(), 32);
        return k;
    }

    // PBKDF2-HMAC-SHA256 (deliberately slow: the shard key of a private shard is made of the identifier with it)
    Key32 Pbkdf2(std::string const& password, Bytes const& salt, int iterations);

    KeyPair NewSigningKey();
    KeyPair SigningKeyFrom(Key32 const& priv);               // the public half from the private one
    Bytes Sign(Key32 const& priv, Bytes const& message);   // 64 bytes
    bool Verify(Key32 const& pub, Bytes const& message, uint8_t const* signature, size_t signatureSize);
    inline bool Verify(Key32 const& pub, Bytes const& message, Bytes const& signature) { return Verify(pub, message, signature.data(), signature.size()); }

    KeyPair NewAgreementKey();
    // the account's long-term agreement key (X25519), derived from its signing key; its public half travels in the certificates
    KeyPair AgreementKeyOf(Key32 const& signingPriv);
    bool Agree(Key32 const& priv, Key32 const& peerPub, Key32& secret);   // false for a bad (low order) peer key

    // ChaCha20-Poly1305: the nonce is 12 bytes; the output is the ciphertext followed by the 16-byte tag
    Bytes Seal(Key32 const& key, uint8_t const nonce[12], Bytes const& aad, Bytes const& plain);
    bool Open(Key32 const& key, uint8_t const nonce[12], Bytes const& aad, uint8_t const* sealed, size_t size, Bytes& plain);
}
