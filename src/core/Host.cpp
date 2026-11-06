#include "Host.h"

namespace p2p
{
    Bytes Encode(HostEvent const& e)
    {
        Writer w;
        w.U8(e.kind).Str(e.name).Blob(e.data).Bool(e.direct).U8(e.node).U8(e.route).U8(e.reason);
        return w.data;
    }

    bool Decode(uint8_t const* p, size_t n, HostEvent& e)
    {
        Reader r(p, n);
        e.kind = static_cast<HostEvent::Kind>(r.U8());
        e.name = r.Str(1024);
        e.data = r.Blob(1 << 17);
        e.direct = r.Bool();
        e.node = r.U8();
        e.route = r.U8();
        e.reason = r.U8();
        return r.ok() && r.end() && e.kind >= HostEvent::NodeUp && e.kind <= HostEvent::NodeList;
    }

    Bytes Encode(HostCommand const& c)
    {
        Writer w;
        w.U8(c.kind).Str(c.name).Str(c.text).Blob(c.data).U8(c.channel).U8(c.route);
        return w.data;
    }

    bool Decode(uint8_t const* p, size_t n, HostCommand& c)
    {
        Reader r(p, n);
        c.kind = static_cast<HostCommand::Kind>(r.U8());
        c.name = r.Str(1024);
        c.text = r.Str(1024);
        c.data = r.Blob(1 << 17);
        c.channel = r.U8();
        c.route = r.U8();
        return r.ok() && r.end() && c.kind >= HostCommand::Open && c.kind <= HostCommand::NodeRemove;
    }
}
