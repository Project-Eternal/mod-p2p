/*
 * Who a player is on the network. An account has one key pair (Ed25519, to sign) and one agreement key (X25519, derived from it). A character
 * has a permanent 128-bit identifier that is the hash of its account's public key and a random value, plus a certificate: the account's
 * signature over (identifier, keys, random value). Anybody can verify a certificate without asking anybody; nobody else can make one for
 * an identifier that hashes that account's key.
 */
#pragma once
#include "Crypto.h"
#include <string>

namespace p2p
{
    struct Account
    {
        crypto::KeyPair signing, agreement;

        static Account Generate();
        static Account FromPrivate(Key32 const& signingPriv);
        bool Valid() const { return signing.pub != Key32{}; }
    };

    struct Certificate
    {
        Id128 id{};          // the character's network identifier
        Key32 accountKey{};  // the account's public (signing) key
        Key32 agreeKey{};    // the account's public agreement key
        Id128 nonce{};       // the random value that was hashed into the identifier
        Bytes signature;     // by the account, over everything above

        static Certificate Make(Account const& account, Id128 const& nonce);
        static Id128 IdOf(Key32 const& accountKey, Id128 const& nonce);
        bool Verify() const;
        void Write(Writer& w) const;
        static bool Read(Reader& r, Certificate& out);   // does not verify
        Bytes Encode() const { Writer w; Write(w); return w.data; }
        static bool Decode(Bytes const& b, Certificate& out)
        {
            Reader r(b);
            return Read(r, out) && r.end();
        }
    };

    // The account part of a character's identifier is the same for all characters of an account
    inline bool SameAccount(Certificate const& a, Certificate const& b) { return a.accountKey == b.accountKey; }
}
