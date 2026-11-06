#include "Link.h"
#include <algorithm>

namespace p2p
{
    Link::Link(Account const& account, Certificate const& mine, std::string myInbox, Key32 const* shardProofKey)
        : _shard(shardProofKey != nullptr), _proofKey(shardProofKey ? *shardProofKey : Key32{}), _account(account), _mine(mine), _myInbox(std::move(myInbox))
    {
        Restart();
    }

    void Link::Restart()
    {
        _ephemeral = crypto::NewAgreementKey();
        _nonce = crypto::RandomId();
        _hello.clear();
        _helloMade = _peerSeen = _up = false;
        _sendSeq[0] = _sendSeq[1] = 0;
        _recvTop[0] = _recvTop[1] = 0;
        _recvMask[0] = _recvMask[1] = 0;
        _recvAny[0] = _recvAny[1] = false;
    }

    static Bytes HelloSigned(Certificate const& c, Key32 const& eph, Id128 const& nonce, std::string const& from, std::string const& to, int64_t time)
    {
        Writer w;
        w.Raw("p2p hello", 9).Id(c.id).Key(eph).Id(nonce).Str(from).Str(to).I64(time);
        return w.data;
    }

    Bytes Link::Hello(std::string const& peerInbox, int64_t now)
    {
        if (_helloMade && peerInbox == _peerInbox)
            return _hello;
        _peerInbox = peerInbox;
        Writer w;
        w.U8('L').U8(1);
        w.Blob(_mine.Encode()).Key(_ephemeral.pub).Id(_nonce).Str(_myInbox).Str(peerInbox).I64(now);
        Bytes signedPart = HelloSigned(_mine, _ephemeral.pub, _nonce, _myInbox, peerInbox, now);
        w.Blob(crypto::Sign(_account.signing.priv, signedPart));
        if (_shard)
        {
            Key32 proof = crypto::Hmac(_proofKey, signedPart);
            w.Blob(proof.data(), proof.size());
        }
        else
            w.Blob(nullptr, 0);
        _hello = w.data;
        _helloMade = true;
        if (_peerSeen)
            Derive();
        return _hello;
    }

    bool Link::OnHello(Bytes const& frame, int64_t now, int64_t maxAgeMs)
    {
        if (!IsHello(frame))
            return false;
        Reader r(frame.data() + 2, frame.size() - 2);
        Certificate cert;
        Bytes certBytes = r.Blob(1024);
        Key32 eph = r.Key();
        Id128 nonce = r.Id();
        std::string from = r.Str(512), to = r.Str(512);
        int64_t time = r.I64();
        Bytes sig = r.Blob(64);
        Bytes proof = r.Blob(32);
        if (!r.ok() || !r.end() || !Certificate::Decode(certBytes, cert) || !cert.Verify())
            return false;
        if (to != _myInbox || (maxAgeMs > 0 && (time < now - maxAgeMs || time > now + maxAgeMs)))
            return false;
        if (!crypto::Verify(cert.accountKey, HelloSigned(cert, eph, nonce, from, to, time), sig))
            return false;
        if (_shard)
        {
            Key32 expected = crypto::Hmac(_proofKey, HelloSigned(cert, eph, nonce, from, to, time));
            if (proof.size() != 32 || !crypto::ConstantTimeEqual(expected.data(), proof.data(), 32))
                return false;
        }
        else if (!proof.empty())
            return false;
        if (cert.id == _mine.id)
            return false;
        if (_peerSeen && (cert.id != _peer.id))
            return false;
        if (_peerSeen && eph == _peerEphemeral && nonce == _peerNonce)
            return true;   // (a repeated hello: nothing changes)
        if (_peerSeen)
        {
            // a different ephemeral key of the same peer: it started again; so do we, and answer with a new hello
            Restart();
        }
        _peer = cert;
        _peerInbox = from;
        _peerEphemeral = eph;
        _peerNonce = nonce;
        _peerSeen = true;
        if (_helloMade)
            Derive();
        return true;
    }

    void Link::Derive()
    {
        Key32 shared{};
        if (!crypto::Agree(_ephemeral.priv, _peerEphemeral, shared))
            return;
        Id128 lo = _nonce, hi = _peerNonce;
        if (hi < lo)
            std::swap(lo, hi);
        Writer salt;
        salt.Id(lo).Id(hi);
        Bytes okm = crypto::Hkdf(shared.data(), 32, salt.data, "p2p link keys", 64);
        Key32 first{}, second{};
        memcpy(first.data(), okm.data(), 32);
        memcpy(second.data(), okm.data() + 32, 32);
        bool iAmLow = _mine.id < _peer.id;
        _sendKey = iAmLow ? first : second;
        _recvKey = iAmLow ? second : first;
        _up = true;
    }

    static void NonceOf(Channel channel, uint64_t seq, uint8_t nonce[12])
    {
        memset(nonce, 0, 12);
        nonce[0] = channel;
        memcpy(nonce + 4, &seq, 8);
    }

    static Bytes AadOf(Channel channel, Id128 const& from, Id128 const& to)
    {
        Writer w;
        w.U8('L').U8(2).U8(channel).Id(from).Id(to);
        return w.data;
    }

    Bytes Link::Seal(Channel channel, Bytes const& plain)
    {
        if (!_up)
            return {};
        uint64_t seq = ++_sendSeq[channel];
        uint8_t nonce[12];
        NonceOf(channel, seq, nonce);
        Bytes sealed = crypto::Seal(_sendKey, nonce, AadOf(channel, _mine.id, _peer.id), plain);
        Writer w;
        w.U8('L').U8(2).U8(channel).U64(seq).Raw(sealed);
        return w.data;
    }

    static Bytes ResetSigned(Id128 const& id, std::string const& from, std::string const& to, int64_t time)
    {
        Writer w;
        w.Raw("p2p reset", 9).Id(id).Str(from).Str(to).I64(time);
        return w.data;
    }

    Bytes Link::MakeReset(Account const& account, Certificate const& mine, std::string const& myInbox, std::string const& toInbox, int64_t now)
    {
        Writer w;
        w.U8('L').U8(3).Blob(mine.Encode()).Str(myInbox).Str(toInbox).I64(now);
        w.Blob(crypto::Sign(account.signing.priv, ResetSigned(mine.id, myInbox, toInbox, now)));
        return w.data;
    }

    bool Link::ParseReset(Bytes const& frame, Certificate& cert, std::string& from, std::string& to, int64_t& time)
    {
        if (!IsReset(frame))
            return false;
        Reader r(frame.data() + 2, frame.size() - 2);
        Bytes certBytes = r.Blob(1024);
        from = r.Str(512);
        to = r.Str(512);
        time = r.I64();
        Bytes sig = r.Blob(64);
        if (!r.ok() || !r.end() || !Certificate::Decode(certBytes, cert) || !cert.Verify())
            return false;
        return crypto::Verify(cert.accountKey, ResetSigned(cert.id, from, to, time), sig);
    }

    OpenResult Link::Try(Bytes const& frame, Channel& channel, Bytes& plain)
    {
        if (!_up || !IsData(frame) || frame.size() < 3 + 1 + 8 + 16)
            return _up ? OpenResult::Bad : OpenResult::NotUp;
        Reader r(frame.data() + 2, frame.size() - 2);
        uint8_t ch = r.U8();
        uint64_t seq = r.U64();
        if (ch > 1 || seq == 0)
            return OpenResult::Bad;
        // replay window of 64 sequences
        if (_recvAny[ch])
        {
            if (seq <= _recvTop[ch])
            {
                uint64_t back = _recvTop[ch] - seq;
                if (back >= 64 || (_recvMask[ch] >> back & 1))
                    return OpenResult::Replay;
            }
        }
        uint8_t nonce[12];
        NonceOf(static_cast<Channel>(ch), seq, nonce);
        if (!crypto::Open(_recvKey, nonce, AadOf(static_cast<Channel>(ch), _peer.id, _mine.id), r.here(), r.remaining(), plain))
            return OpenResult::Bad;
        if (!_recvAny[ch] || seq > _recvTop[ch])
        {
            uint64_t shift = _recvAny[ch] ? seq - _recvTop[ch] : 64;
            _recvMask[ch] = shift >= 64 ? 1 : (_recvMask[ch] << shift) | 1;
            _recvTop[ch] = seq;
            _recvAny[ch] = true;
        }
        else
            _recvMask[ch] |= 1ull << (_recvTop[ch] - seq);
        channel = static_cast<Channel>(ch);
        return OpenResult::Ok;
    }
}
