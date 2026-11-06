#include "Check.h"
#include "../src/core/Link.h"
#include "../src/core/Rooms.h"
using namespace p2p;

int main()
{
    // HKDF-SHA256, RFC 5869 test case 1
    {
        Bytes ikm(22, 0x0b), salt, okm;
        for (int i = 0; i <= 12; ++i) salt.push_back(i);
        Bytes out = crypto::Hkdf(ikm.data(), ikm.size(), salt, std::string("\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9", 10), 42);
        CHECK(Hex(out) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    }
    // sign, verify, tamper
    {
        auto k = crypto::NewSigningKey();
        Bytes m = ToBytes("hello");
        Bytes s = crypto::Sign(k.priv, m);
        CHECK(s.size() == 64);
        CHECK(crypto::Verify(k.pub, m, s));
        Bytes m2 = ToBytes("hellp");
        CHECK(!crypto::Verify(k.pub, m2, s));
        s[3] ^= 1;
        CHECK(!crypto::Verify(k.pub, m, s));
        CHECK(crypto::SigningKeyFrom(k.priv).pub == k.pub);
    }
    // agreement and AEAD
    {
        auto a = crypto::NewAgreementKey(), b = crypto::NewAgreementKey();
        Key32 s1{}, s2{};
        CHECK(crypto::Agree(a.priv, b.pub, s1));
        CHECK(crypto::Agree(b.priv, a.pub, s2));
        CHECK(s1 == s2);
        uint8_t nonce[12] = {1, 2, 3};
        Bytes sealed = crypto::Seal(s1, nonce, ToBytes("aad"), ToBytes("secret text"));
        CHECK(sealed.size() == 11 + 16);
        Bytes plain;
        CHECK(crypto::Open(s2, nonce, ToBytes("aad"), sealed.data(), sealed.size(), plain));
        CHECK(ToString(plain) == "secret text");
        CHECK(!crypto::Open(s2, nonce, ToBytes("aae"), sealed.data(), sealed.size(), plain));
        sealed[0] ^= 1;
        CHECK(!crypto::Open(s2, nonce, ToBytes("aad"), sealed.data(), sealed.size(), plain));
        // empty plaintext
        sealed = crypto::Seal(s1, nonce, {}, {});
        CHECK(sealed.size() == 16 && crypto::Open(s2, nonce, {}, sealed.data(), sealed.size(), plain) && plain.empty());
    }
    // identity: the identifier is bound to the account key
    {
        Account a = Account::Generate(), b = Account::Generate();
        Id128 nonce = crypto::RandomId();
        Certificate c = Certificate::Make(a, nonce);
        CHECK(c.Verify());
        CHECK(c.id == Certificate::IdOf(a.signing.pub, nonce));
        Certificate forged = c;   // another account claims the same identifier
        forged.accountKey = b.signing.pub;
        forged.agreeKey = b.agreement.pub;
        forged.signature = crypto::Sign(b.signing.priv, ToBytes("x"));
        CHECK(!forged.Verify());
        Certificate other = Certificate::Make(b, nonce);   // same nonce, other account: another identifier
        CHECK(other.id != c.id && other.Verify());
        Certificate tampered = c;
        tampered.nonce[0] ^= 1;
        CHECK(!tampered.Verify());
        Certificate back;
        CHECK(Certificate::Decode(c.Encode(), back) && back.Verify() && back.id == c.id);
        CHECK(!Certificate::Decode(Bytes(10), back));
        // the account is restored from its private key
        Account again = Account::FromPrivate(a.signing.priv);
        CHECK(again.signing.pub == a.signing.pub && again.agreement.pub == a.agreement.pub);
    }
    // rooms: private names hide the secret, depend on the realm; the proof key differs from the name
    {
        Id128 party = crypto::RandomId();
        Room r1 = PrivateRoom(1, "party", PartySecret(party)), r1b = PrivateRoom(1, "party", PartySecret(party)), r2 = PrivateRoom(2, "party", PartySecret(party));
        CHECK(r1.name == r1b.name && r1.proofKey == r1b.proofKey);
        CHECK(r1.name != r2.name && r1.proofKey != r2.proofKey);
        CHECK(r1.name.compare(0, 9, "/realm/1/") == 0);
        CHECK(r1.name.find(Hex(party)) == std::string::npos);
        CHECK(Hex(r1.proofKey).size() == 64 && r1.name.find(Hex(r1.proofKey)) == std::string::npos);
        Room chan = PrivateRoom(1, "chan", ChannelSecret("trade")), chan2 = PrivateRoom(1, "chan", ChannelSecret("trade2"));
        CHECK(chan.name != chan2.name);
        // pair rooms: both sides compute the same room; a third account cannot
        Account a = Account::Generate(), b = Account::Generate(), c = Account::Generate();
        Certificate ca = Certificate::Make(a, crypto::RandomId()), cb = Certificate::Make(b, crypto::RandomId()), cc = Certificate::Make(c, crypto::RandomId());
        Bytes sab, sba, sca;
        CHECK(PairSecret(a, ca, cb, sab) && PairSecret(b, cb, ca, sba) && PairSecret(c, cc, ca, sca));
        CHECK(sab == sba && sab != sca);
        // envelopes: signature, MAC, tampering, wrong room key
        Room pair = PrivateRoom(1, "pair", sab), wrong = PrivateRoom(1, "pair", sca);
        Bytes wire = Envelope::Make(a, ca, pair, 1234, ToBytes("hi"));
        Envelope e;
        CHECK(Envelope::Decode(wire, e) && e.Verify(&pair));
        CHECK(!e.Verify(&wrong));
        Envelope pub;
        Room open;
        open.name = "/realm/1/world/0/1_1";
        CHECK(Envelope::Decode(Envelope::Make(a, ca, open, 5, ToBytes("x")), pub) && pub.Verify(nullptr));
        Envelope moved = pub;   // a public announcement moved to another room
        moved.room = "/realm/1/world/0/1_2";
        CHECK(!moved.Verify(nullptr));
        Envelope late = pub;
        late.time = 6;
        CHECK(!late.Verify(nullptr));
        Envelope swapped = pub;   // payload changed by the node
        swapped.payload = ToBytes("y");
        CHECK(!swapped.Verify(nullptr));
        // a node that knows the room name but not the secret cannot write in it: it has no proof key
        Envelope forged = e;
        forged.mac = Key32{};
        CHECK(!forged.Verify(&pair));
        CHECK(!Envelope::Decode(Bytes{1, 2, 3}, e));
    }
    // links
    {
        Account a = Account::Generate(), b = Account::Generate(), m = Account::Generate();
        Certificate ca = Certificate::Make(a, crypto::RandomId()), cb = Certificate::Make(b, crypto::RandomId());
        Link la(a, ca, "inboxA"), lb(b, cb, "inboxB");
        Bytes ha = la.Hello("inboxB", 1000), hb = lb.Hello("inboxA", 1000);
        CHECK(la.Hello("inboxB", 1000) == ha);
        CHECK(!la.Up());
        CHECK(lb.OnHello(ha, 1000, 5000));
        CHECK(la.OnHello(hb, 1000, 5000));
        CHECK(la.Up() && lb.Up());
        Bytes f = la.Seal(Reliable, ToBytes("one"));
        Channel ch; Bytes plain;
        CHECK(lb.Open(f, ch, plain) && ToString(plain) == "one" && ch == Reliable);
        CHECK(!lb.Open(f, ch, plain));   // replay
        Bytes f2 = la.Seal(Unreliable, ToBytes("two")), f3 = la.Seal(Unreliable, ToBytes("three"));
        CHECK(lb.Open(f3, ch, plain) && ToString(plain) == "three");
        CHECK(lb.Open(f2, ch, plain) && ToString(plain) == "two");   // late but inside the window
        CHECK(!lb.Open(f2, ch, plain));
        Bytes back = lb.Seal(Reliable, ToBytes("back"));
        CHECK(la.Open(back, ch, plain) && ToString(plain) == "back");
        CHECK(!la.Open(f, ch, plain));   // a frame of our own direction does not open for us
        Bytes bad = f3; bad.back() ^= 1;
        CHECK(!lb.Open(bad, ch, plain));
        // a hello meant for someone else, an old one, one from the node itself
        Link lc(b, cb, "inboxC");
        CHECK(!lc.OnHello(ha, 1000, 5000));
        Link ld(b, cb, "inboxB");
        CHECK(!ld.OnHello(ha, 100000, 5000));
        Certificate cm = Certificate::Make(m, crypto::RandomId());
        Link lm(m, cm, "node");
        Bytes hm = lm.Hello("inboxB", 1000);
        Link le(b, cb, "inboxB");
        CHECK(le.OnHello(hm, 1000, 5000));   // (a node may make its own character, like anybody)
        le.Hello("node", 1000);
        CHECK(le.Up());
        Bytes tampered = ha; tampered[40] ^= 1;
        Link lf(b, cb, "inboxB");
        CHECK(!lf.OnHello(tampered, 1000, 5000));
        // a man in the middle that swaps in its own ephemeral key cannot: the signature covers it
        // the keys of two sessions differ
        Link la2(a, ca, "inboxA"), lb2(b, cb, "inboxB");
        Bytes ha2 = la2.Hello("inboxB", 1000), hb2 = lb2.Hello("inboxA", 1000);
        lb2.OnHello(ha2, 1000, 5000); la2.OnHello(hb2, 1000, 5000);
        Bytes g = la2.Seal(Reliable, ToBytes("x"));
        CHECK(!lb.Open(g, ch, plain));
        // restart: the peer starts over with a new hello
        Link lr(a, ca, "inboxA");
        Bytes hr = lr.Hello("inboxB", 1000);
        CHECK(lb.OnHello(hr, 1000, 5000));
        CHECK(!lb.Up() && !lb.HelloMade());
        lb.Hello("inboxA", 1000);
        CHECK(lr.OnHello(lb.Hello("inboxA", 1000), 1000, 5000) && lr.Up() && lb.Up());
    }
    DONE();
}
