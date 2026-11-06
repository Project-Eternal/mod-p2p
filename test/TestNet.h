// An exchange inside the process: stands in for the relay nodes and the WebRTC links in tests. Delivery takes the route's latency in milliseconds
// of the test's clock and may lose a share of the messages; a host can be cut off, killed (its last will is published on every node it was on),
// made "relay only", or taken off one node. There is one node unless 'nodes' is raised.
#pragma once
#include "../src/core/Host.h"
#include "../src/core/Rooms.h"
#include <deque>
#include <map>
#include <random>
#include <set>

namespace p2p
{
    class TestNet;

    class TestHost : public PeerHost
    {
    public:
        explicit TestHost(TestNet& net) : _net(net) { }
        void Command(HostCommand const& c) override;
        bool Poll(HostEvent& e) override
        {
            if (_events.empty())
                return false;
            e = std::move(_events.front());
            _events.pop_front();
            return true;
        }
        bool directAllowed = true;
        int64_t pending() const { return static_cast<int64_t>(_events.size()); }
        bool OnNode(unsigned k) const { return (nodeMask >> k & 1) != 0; }

        TestNet& _net;
        std::string inbox, realm;
        Bytes will;
        std::set<std::string> topics;
        std::deque<HostEvent> _events;
        bool open = false;
        unsigned nodeMask = 0xFF;                 // the nodes this host is connected to
        std::set<std::string> directTo;
        std::map<HostCommand::Kind, int> counts;
        std::map<std::string, int> sentOn;        // frames sent to a peer by route ("<inbox>/<route>")
    };

    class TestNet
    {
    public:
        struct Item { int64_t at; TestHost* to; HostEvent event; };
        int64_t now = 0, latency = 0;
        std::vector<TestHost*> hosts;
        std::deque<Item> queue;
        bool nodeUp = true;                         // node 0
        unsigned nodes = 1;
        bool otherUp[MaxNodes] = {true, true, true, true, true, true, true, true};   // nodes 1 and up
        // per route: extra latency (ms) and the share of messages lost; index 0 = direct, 1 + k = node k
        int64_t routeLatency[MaxNodes + 1] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};   // -1: 'latency'
        double routeLoss[MaxNodes + 1] = {};
        std::mt19937 rng{1};
        // what the node (the one who relays) saw: the topic names and the sizes, never to be able to read anything
        std::vector<std::pair<std::string, Bytes>> wire;
        std::map<int, size_t> frames;               // link frames by route, as delivered
        std::set<std::string> muted;                // hosts (by inbox) whose frames to peers are dropped: one direction of a connection lost

        bool NodeIsUp(unsigned k) const { return k == 0 ? nodeUp : (k < nodes && otherUp[k]); }
        void Register(TestHost* h) { hosts.push_back(h); }
        TestHost* ByInbox(std::string const& inbox)
        {
            for (TestHost* h : hosts)
                if (h->open && h->inbox == inbox)
                    return h;
            return nullptr;
        }
        int64_t LatencyOf(int route) const { return routeLatency[route] >= 0 ? routeLatency[route] : latency; }
        bool Lost(int route) { return routeLoss[route] > 0 && std::uniform_real_distribution<>(0, 1)(rng) < routeLoss[route]; }
        void Deliver(TestHost* to, HostEvent e, int route = 0)
        {
            queue.push_back({now + LatencyOf(route), to, std::move(e)});
        }
        void Publish(TestHost* from, std::string const& topic, Bytes const& data, int onlyNode = -1)
        {
            wire.emplace_back(topic, data);
            for (unsigned k = 0; k < nodes; ++k)
            {
                if ((onlyNode >= 0 && onlyNode != (int)k) || !NodeIsUp(k) || !from->OnNode(k))
                    continue;
                if (Lost(1 + k))
                    continue;
                for (TestHost* h : hosts)
                    if (h != from && h->open && h->OnNode(k) && h->topics.count(topic))
                    {
                        HostEvent e;
                        e.kind = HostEvent::RoomMessage;
                        e.name = topic;
                        e.data = data;
                        e.node = k;
                        Deliver(h, e, 1 + k);
                    }
            }
        }
        void Advance(int64_t ms)
        {
            now += ms;
            for (size_t i = 0; i < queue.size();)
            {
                if (queue[i].at <= now)
                {
                    Item item = std::move(queue[i]);
                    queue.erase(queue.begin() + i);
                    if (item.to->open)
                        item.to->_events.push_back(std::move(item.event));
                }
                else
                    ++i;
            }
        }
        void PublishWill(TestHost* h, int onlyNode = -1)
        {
            std::string topic;
            Envelope env;
            if (Envelope::Decode(h->will, env))
                topic = env.room;
            if (topic.empty())
                return;
            for (unsigned k = 0; k < nodes; ++k)
            {
                if ((onlyNode >= 0 && onlyNode != (int)k) || !h->OnNode(k))
                    continue;
                for (TestHost* o : hosts)
                    if (o != h && o->open && o->OnNode(k) && o->topics.count(topic))
                    {
                        HostEvent e;
                        e.kind = HostEvent::RoomMessage;
                        e.name = topic;
                        e.data = h->will;
                        e.node = k;
                        Deliver(o, e, 1 + k);
                    }
            }
        }
        // the connection of a host dies: the node publishes its will
        void Kill(TestHost* h)
        {
            if (!h->open)
                return;
            for (TestHost* o : hosts)
                if (o != h && o->open)
                {
                    if (o->directTo.erase(h->inbox))
                    {
                        HostEvent e;
                        e.kind = HostEvent::LinkDown;
                        e.name = h->inbox;
                        Deliver(o, e);
                    }
                }
            PublishWill(h);
            h->open = false;
        }
        // node k goes away for everyone: the hosts on it are told, and the node publishes their wills
        void NodeFails(unsigned k)
        {
            if (k == 0)
                nodeUp = false;
            else
                otherUp[k] = false;
            for (TestHost* h : hosts)
                if (h->open && h->OnNode(k))
                {
                    HostEvent e;
                    e.kind = HostEvent::NodeDown;
                    e.node = k;
                    e.name = "node" + std::to_string(k);
                    h->_events.push_back(e);
                }
        }
        void NodeReturns(unsigned k)
        {
            if (k == 0)
                nodeUp = true;
            else
                otherUp[k] = true;
            for (TestHost* h : hosts)
                if (h->open && h->OnNode(k))
                {
                    HostEvent e;
                    e.kind = HostEvent::NodeUp;
                    e.node = k;
                    e.name = "node" + std::to_string(k);
                    h->_events.push_back(e);
                }
        }
        // a host loses its connection to one node (the others stay): that node publishes its will
        void LeaveNode(TestHost* h, unsigned k)
        {
            PublishWill(h, k);
            h->nodeMask &= ~(1u << k);
            HostEvent e;
            e.kind = HostEvent::NodeDown;
            e.node = k;
            e.name = "node" + std::to_string(k);
            h->_events.push_back(e);
        }
    };

    inline void TestHost::Command(HostCommand const& c)
    {
        ++counts[c.kind];
        switch (c.kind)
        {
            case HostCommand::Open:
            {
                inbox = c.name;
                realm = c.text;
                will = c.data;
                open = true;
                topics.insert(realm + "/inbox/" + inbox);
                for (unsigned k = 0; k < _net.nodes; ++k)
                    if (OnNode(k) && _net.NodeIsUp(k))
                    {
                        HostEvent e;
                        e.kind = HostEvent::NodeUp;
                        e.node = k;
                        e.name = "node" + std::to_string(k);
                        _net.Deliver(this, e, 1 + k);
                    }
                break;
            }
            case HostCommand::Close:
                open = false;
                break;
            case HostCommand::Will:
                _net.PublishWill(this);
                break;
            case HostCommand::Join:
            case HostCommand::Watch:
                topics.insert(c.name);
                break;
            case HostCommand::Leave:
            case HostCommand::Unwatch:
                topics.erase(c.name);
                break;
            case HostCommand::Publish:
                _net.Publish(this, c.name, c.data, c.route == RouteAny ? -1 : (int)c.route - 1);
                break;
            case HostCommand::Send:
            {
                TestHost* to = _net.ByInbox(c.name);
                if (!to)
                    break;
                int route = -1;
                auto viaNode = [&](unsigned k) { return k < _net.nodes && _net.NodeIsUp(k) && OnNode(k) && to->OnNode(k); };
                if (c.route == RouteAny)
                {
                    if (directTo.count(c.name))
                        route = 0;
                    else
                        for (unsigned k = 0; k < _net.nodes && route < 0; ++k)
                            if (viaNode(k))
                                route = 1 + k;
                }
                else if (c.route == RouteDirect)
                    route = directTo.count(c.name) ? 0 : -1;
                else if (RouteIsNode(c.route) && viaNode(c.route - 1))
                    route = c.route;
                if (route < 0 || _net.muted.count(inbox))
                    break;
                ++sentOn[c.name + "/" + std::to_string(route)];
                ++_net.frames[route];
                if (route != 0)
                    _net.wire.emplace_back(realm + "/inbox/" + c.name, c.data);
                if (_net.Lost(route))
                    break;
                HostEvent e;
                e.kind = HostEvent::InboxMessage;
                e.name = inbox;
                e.data = c.data;
                e.route = route;
                _net.Deliver(to, e, route);
                break;
            }
            case HostCommand::Connect:
            {
                TestHost* to = _net.ByInbox(c.name);
                if (to && directAllowed && to->directAllowed && !directTo.count(c.name))
                {
                    directTo.insert(c.name);
                    to->directTo.insert(inbox);
                    HostEvent e;
                    e.kind = HostEvent::LinkUp;
                    e.name = c.name;
                    e.direct = true;
                    _net.Deliver(this, e);
                    e.name = inbox;
                    _net.Deliver(to, e);
                }
                else if (to && (!directAllowed || !to->directAllowed))
                {
                    HostEvent e;
                    e.kind = HostEvent::DirectFailed;
                    e.name = c.name;
                    e.reason = directAllowed ? DirectNoPath : DirectDisabled;
                    _net.Deliver(this, e);
                }
                break;
            }
            case HostCommand::Disconnect:
            {
                if (directTo.erase(c.name))
                {
                    HostEvent e;
                    e.kind = HostEvent::LinkDown;
                    e.name = c.name;
                    _net.Deliver(this, e);
                    if (TestHost* to = _net.ByInbox(c.name))
                    {
                        to->directTo.erase(inbox);
                        e.name = inbox;
                        _net.Deliver(to, e);
                    }
                }
                break;
            }
        }
    }
}
