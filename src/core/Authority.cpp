#include "Authority.h"
#include "Crypto.h"

namespace p2p
{
    Key32 AuthorityTable::Hash(Bytes const& b) { return crypto::Sha256(b); }
}
