/*
 * One encrypted channel between two servers, whatever carries the bytes (a WebRTC data channel, or the relay node's inbox topics).
 *
 * Handshake: each side sends one HELLO, signed by its account key, with a fresh ephemeral X25519 key, a random value and the inbox of the
 * receiver (so a hello cannot be replayed to someone else). Both compute the same shared secret, and derive one key per direction. Frames
 * are ChaCha20-Poly1305 with a per-channel sequence number as nonce; the receiver keeps a sliding window against replays. A frame that opens
 * proves the other side holds the ephemeral key that its account key signed, so the node cannot take part.
 *
 * In a private shard the hello also carries a proof: HMAC (key derived from the shard key) over the signed content of the hello,
 * which covers the ephemeral key and both inboxes, so it cannot be replayed into another link and shows nothing of the key. A hello without the
 * right proof (and, in the public world, a hello with any proof) is refused as an unknown party.
 *
 * Wire (as handed to the host for a peer): 'L' type ...
 *   hello: 'L' 1 Blob(certificate) Key(ephemeral) Id(nonce) Str(from inbox) Str(to inbox) I64(time) Blob(signature) Blob(proof)
 *   data:  'L' 2 channel u64(sequence) sealed
 *   reset: 'L' 3 Blob(certificate) Str(from inbox) Str(to inbox) I64(time) Blob(signature)   (sent to the sender of a data frame that cannot be opened)
 */
#pragma once
#include "Identity.h"

namespace p2p
{
    enum Channel : uint8_t { Reliable = 0, Unreliable = 1 };
    enum class OpenResult { Ok, Replay, Bad, NotUp };   // Replay: a frame already accepted (normal when a frame is sent again on another route)

    class Link
    {
    public:
        Link(Account const& account, Certificate const& mine, std::string myInbox, Key32 const* shardProofKey = nullptr);

        // the hello to send to the peer's inbox (the same bytes until Restart)
        Bytes Hello(std::string const& peerInbox, int64_t now);
        // handles a received hello; true when it is valid and fresh (the link is Up once our own hello was made too)
        bool OnHello(Bytes const& frame, int64_t now, int64_t maxAgeMs);
        void Restart();   // forget the handshake (a new ephemeral key at the next Hello)

        bool Up() const { return _up; }
        bool HelloMade() const { return _helloMade; }
        bool HelloSeen() const { return _peerSeen; }
        Certificate const& Peer() const { return _peer; }
        std::string const& PeerInbox() const { return _peerInbox; }

        Bytes Seal(Channel channel, Bytes const& plain);
        // false when the frame is not for this link, does not authenticate, or repeats one that was accepted
        bool Open(Bytes const& frame, Channel& channel, Bytes& plain) { return Try(frame, channel, plain) == OpenResult::Ok; }
        OpenResult Try(Bytes const& frame, Channel& channel, Bytes& plain);

        static bool IsHello(Bytes const& frame) { return frame.size() > 2 && frame[0] == 'L' && frame[1] == 1; }
        static bool IsData(Bytes const& frame) { return frame.size() > 2 && frame[0] == 'L' && frame[1] == 2; }
        static bool IsReset(Bytes const& frame) { return frame.size() > 2 && frame[0] == 'L' && frame[1] == 3; }
        // A signed reset: "I cannot open your frames". The receiver checks it against the certificate it knows for 'from'.
        static Bytes MakeReset(Account const& account, Certificate const& mine, std::string const& myInbox, std::string const& toInbox, int64_t now);
        static bool ParseReset(Bytes const& frame, Certificate& cert, std::string& from, std::string& to, int64_t& time);

    private:
        void Derive();

        bool _shard = false;
        Key32 _proofKey{};
        Account _account;
        Certificate _mine, _peer;
        std::string _myInbox, _peerInbox;
        crypto::KeyPair _ephemeral;
        Id128 _nonce{}, _peerNonce{};
        Key32 _peerEphemeral{};
        Bytes _hello;
        bool _helloMade = false, _peerSeen = false, _up = false;
        Key32 _sendKey{}, _recvKey{};
        uint64_t _sendSeq[2] = {0, 0};
        uint64_t _recvTop[2] = {0, 0};
        uint64_t _recvMask[2] = {0, 0};   // bit i: sequence (top - i) was seen
        bool _recvAny[2] = {false, false};
    };
}
