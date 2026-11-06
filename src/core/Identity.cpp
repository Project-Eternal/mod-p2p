#include "Identity.h"

namespace p2p
{
    Account Account::Generate()
    {
        Account a;
        a.signing = crypto::NewSigningKey();
        a.agreement = crypto::AgreementKeyOf(a.signing.priv);
        return a;
    }

    Account Account::FromPrivate(Key32 const& priv)
    {
        Account a;
        a.signing = crypto::SigningKeyFrom(priv);
        a.agreement = crypto::AgreementKeyOf(priv);
        return a;
    }

    Id128 Certificate::IdOf(Key32 const& accountKey, Id128 const& nonce)
    {
        Writer w;
        w.Raw("p2p character", 13).Key(accountKey).Id(nonce);
        Key32 h = crypto::Sha256(w.data);
        Id128 id{};
        memcpy(id.data(), h.data(), 16);
        return id;
    }

    static Bytes Signed(Certificate const& c)
    {
        Writer w;
        w.Raw("p2p certificate", 15).Id(c.id).Key(c.accountKey).Key(c.agreeKey).Id(c.nonce);
        return w.data;
    }

    Certificate Certificate::Make(Account const& account, Id128 const& nonce)
    {
        Certificate c;
        c.accountKey = account.signing.pub;
        c.agreeKey = account.agreement.pub;
        c.nonce = nonce;
        c.id = IdOf(c.accountKey, nonce);
        c.signature = crypto::Sign(account.signing.priv, Signed(c));
        return c;
    }

    bool Certificate::Verify() const
    {
        return IdOf(accountKey, nonce) == id && crypto::Verify(accountKey, Signed(*this), signature);
    }

    void Certificate::Write(Writer& w) const
    {
        w.Id(id).Key(accountKey).Key(agreeKey).Id(nonce).Blob(signature);
    }

    bool Certificate::Read(Reader& r, Certificate& out)
    {
        out.id = r.Id();
        out.accountKey = r.Key();
        out.agreeKey = r.Key();
        out.nonce = r.Id();
        out.signature = r.Blob(64);
        return r.ok();
    }
}
