#include "Check.h"
#include "../src/core/Authority.h"
using namespace p2p;

static Id128 Id(uint8_t b) { Id128 i{}; i[0] = b; return i; }

int main()
{
    Id128 a = Id(1), b = Id(2), c = Id(3);
    AuthorityTable ta(a), tb(b);
    EntityKey k{1, 0, 0, 1234, Id128{}};
    // the earlier claim wins; the tie goes to the lower identifier
    CHECK(Beats(Claim{b, 100}, Claim{a, 200}));
    CHECK(Beats(Claim{a, 100}, Claim{b, 100}));
    CHECK(!Beats(Claim{b, 100}, Claim{a, 100}));
    CHECK(Beats(Claim{a, 5}, Claim{}));
    CHECK(!Beats(Claim{}, Claim{a, 5}));
    // both claim at once: after they hear each other, both agree who decides, and the loser knows it lost
    bool lostA = false, lostB = false;
    auto& ea = ta.At(k); auto& eb = tb.At(k);
    ta.OnClaim(ea, Claim{a, 1000}, Situation::Untouched, lostA);
    tb.OnClaim(eb, Claim{b, 990}, Situation::Untouched, lostB);
    CHECK(ta.Mine(ea) && tb.Mine(eb));
    bool accepted = ta.OnClaim(ea, Claim{b, 990}, Situation::Untouched, lostA);
    CHECK(accepted && lostA && !ta.Mine(ea) && ea.claim.server == b);
    accepted = tb.OnClaim(eb, Claim{a, 1000}, Situation::Untouched, lostB);
    CHECK(!accepted && !lostB && tb.Mine(eb));
    // an engaged (negotiated) entity is not taken by a later untouched claim
    ta.Engage(ea, a, 2000);
    CHECK(ea.situation == Situation::Negotiated || ea.claim.server == b);
    AuthorityEntry n;
    ta.Engage(n, b, 3000);
    ta.Engage(n, c, 3100);
    ta.Engage(n, a, 3200);
    CHECK(n.situation == Situation::Negotiated && n.claim.server == b && n.participants.size() == 3);
    bool lost = false;
    CHECK(!ta.OnClaim(n, Claim{a, 100}, Situation::Untouched, lost));
    // the arbiter leaves: the next to have engaged claims
    CHECK(ta.Leave(n, b, 4000) == c);
    CHECK(n.claim.server == c && n.participants.size() == 2);
    CHECK(ta.Leave(n, a, 4100) == c);          // (not the arbiter: nothing changes)
    CHECK(ta.Leave(n, c, 4200) == Id128{});    // the last leaves: untouched again
    CHECK(n.situation == Situation::Untouched && n.claim.Empty());
    // a server vanishes: what it decided passes on
    AuthorityTable t(a);
    EntityKey k1{1, 0, 0, 1, {}}, k2{1, 0, 0, 2, {}}, k3{2, 0, 0, 3, {}};
    t.Engage(t.At(k1), b, 1); t.Engage(t.At(k1), c, 2);
    t.At(k2).situation = Situation::Owned; t.At(k2).claim = Claim{b, 5};
    t.At(k3).claim = Claim{c, 5};
    auto changed = t.ServerGone(b, 100);
    CHECK(changed.size() == 2);
    CHECK(t.At(k1).claim.server == c && t.At(k2).claim.Empty() && t.At(k3).claim.server == c);
    CHECK(AuthorityTable::GovernorShouldChange(30.0f, 10.0f, 5.0f) && !AuthorityTable::GovernorShouldChange(30.0f, 27.0f, 5.0f));
    CHECK(AuthorityTable::MakeGeneration(a, 5) != AuthorityTable::MakeGeneration(b, 5) && AuthorityTable::MakeGeneration(a, 5) != AuthorityTable::MakeGeneration(a, 6));
    EntityKey sorted1{1, 0, 0, 1, {}}, sorted2{1, 0, 0, 2, {}};
    CHECK(sorted1 < sorted2 && !(sorted2 < sorted1) && sorted1 == sorted1);
    Writer w; k.Write(w); Reader r(w.data); CHECK(EntityKey::Read(r) == k);
    DONE();
}
