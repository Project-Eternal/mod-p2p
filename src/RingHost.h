/*
 * The peer host as the page provides it: two rings in this module's memory (Ring.h), commands going out and events coming in, with the
 * page's JavaScript (web/p2p.js) at the other end. Outside the page there is no host: Available() is false and the module stays idle.
 */
#pragma once
#include "core/Host.h"
#include "core/Ring.h"
#include <string>
#include <vector>
#include <memory>

namespace p2p
{
    class RingHost : public PeerHost
    {
    public:
        RingHost();
        ~RingHost() override;

        // true when a page is there to take the rings (Module.p2p given by web/acore.js, with a node address)
        bool Attach();
        bool Available() const { return _attached; }
        bool ChatEvents() const { return _chatEvents; }   // the page's setting: network events written into the chat
        std::string const& Shard() const { return _shard; }   // the page's private shard identifier (empty: the public world); never sent anywhere

        void Command(HostCommand const& c) override;
        bool Poll(HostEvent& e) override;
        // blocks up to 'ms' until an event arrives (the page's futex notify), or returns at once if there is one
        void Wait(unsigned ms);
        // tells the page there are commands (it also polls by itself)
        void Kick();
        uint64_t Dropped() const { return _dropped; }

    private:
        void* _memory[2] = {nullptr, nullptr};
        std::unique_ptr<Ring> _toHost, _toModule;
        bool _attached = false, _kickPending = false, _chatEvents = true;
        std::string _shard;
        uint32_t _seen = 0;
        uint64_t _dropped = 0;
        std::vector<uint8_t> _scratch;
    };
}
