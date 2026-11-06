/*
 * The world's changes shared between the players of a realm:
 *
 *   local command  ->  world database  ->  SQLite shim: the commit's rows as full images (listener)  ->  this module: a record  ->  every server in touch
 *
 * A record is one commit of the world database: its author (the character whose server made it), the author's sequence number, the network time, and the rows
 * (put: the whole row; delete: its key). Every server holds every record of the realm (the table p2p_world_change), knows the highest sequence number it holds
 * for each author, and compares these numbers with a server when their link comes up: each sends the other what it lacks. A new record goes at once to every
 * server in touch and is passed on, so a change spreads through any chain of meetings.
 *
 * Keys: a row of the world database has the same key everywhere. A row an author created has a key from the author's own range (World.CustomIdStart and up),
 * which means nothing elsewhere: the receiving server gives it a key of its own and keeps the pair (author, domain, theirs -> ours) in p2p_world_map, also for
 * the columns that refer to such a row (the Refs table below).
 * Two changes of one row: the later in network time wins, the author's identity breaks a tie (p2p_world_row keeps the change each row came from); the record that
 * loses is kept, so every server comes to the same rows.
 * Applying a record writes the rows to the world database through the realm's change log and reads the core's tables again (WorldChanges.cpp).
 */
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "P2PService.h"
#include "Player.h"
#include "Protocol.h"
#include "WorldChanges.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>

extern "C" void mysql_sqlite_changes_listen(void (*listener)(char const* database, char const* rows));   // the SQLite shim

namespace p2p
{
    namespace
    {
        enum Step : uint8_t { StepHave = 1, StepRecords = 2 };
        enum Domain : uint8_t { DSpawn = 1, DObject = 2, DTele = 3, DPath = 4, DScript = 5 };

        // the columns that hold an identifier from a range of the author: a key of the row itself or a reference to the key of another
        struct Ref
        {
            char const* table;
            char const* column;
            Domain domain;
        };
        constexpr Ref Refs[] =
        {
            { "creature", "guid", DSpawn }, { "creature_addon", "guid", DSpawn }, { "creature_addon", "path_id", DPath },
            { "creature_formations", "leaderGUID", DSpawn }, { "creature_formations", "memberGUID", DSpawn },
            { "linked_respawn", "guid", DSpawn }, { "linked_respawn", "linkedGuid", DSpawn },
            { "game_event_creature", "guid", DSpawn }, { "pool_creature", "guid", DSpawn },
            { "gameobject", "guid", DObject }, { "gameobject_addon", "guid", DObject },
            { "game_event_gameobject", "guid", DObject }, { "pool_gameobject", "guid", DObject },
            { "game_tele", "id", DTele },
            { "waypoint_data", "id", DPath }, { "waypoint_data", "action", DScript }, { "waypoint_scripts", "guid", DScript },
        };

        // one cell of a row in the text form of the shim: the name (a key column ends with '*') and the value (N null, I integer, F real, T hex of text, B hex of bytes)
        struct Cell
        {
            std::string name, value;
            bool key = false;
        };
        struct Op
        {
            bool put = true;
            std::string table;
            std::vector<Cell> cells;
        };
        struct Change
        {
            Id128 author{};
            uint64_t seq = 0;
            int64_t at = 0;
            std::vector<Op> ops;
        };

        // the rows of a commit as the shim gives them (one line each: P or D, table, name=value ...)
        std::vector<Op> ParseRows(std::string const& rows)
        {
            std::vector<Op> ops;
            std::istringstream lines(rows);
            std::string line;
            while (std::getline(lines, line))
            {
                std::vector<std::string> parts;
                size_t from = 0;
                while (true)
                {
                    size_t tab = line.find('\t', from);
                    parts.push_back(line.substr(from, tab == std::string::npos ? tab : tab - from));
                    if (tab == std::string::npos)
                        break;
                    from = tab + 1;
                }
                if (parts.size() < 3 || (parts[0] != "P" && parts[0] != "D"))
                    continue;
                Op op;
                op.put = parts[0] == "P";
                op.table = parts[1];
                for (size_t i = 2; i < parts.size(); ++i)
                {
                    size_t eq = parts[i].find('=');
                    if (eq == std::string::npos || eq == 0)
                        continue;
                    Cell c;
                    c.name = parts[i].substr(0, eq);
                    if (c.name.back() == '*')
                    {
                        c.key = true;
                        c.name.pop_back();
                    }
                    c.value = parts[i].substr(eq + 1);
                    op.cells.push_back(std::move(c));
                }
                bool anyKey = false;
                for (Cell const& c : op.cells)
                    anyKey = anyKey || c.key;
                if (anyKey)
                    ops.push_back(std::move(op));
            }
            return ops;
        }

        Bytes Encode(Change const& c)
        {
            Writer w;
            w.Id(c.author).U64(c.seq).I64(c.at).U16(static_cast<uint16_t>(c.ops.size()));
            for (Op const& op : c.ops)
            {
                w.Bool(op.put).Str(op.table).U16(static_cast<uint16_t>(op.cells.size()));
                for (Cell const& cell : op.cells)
                    w.Str(cell.name).Bool(cell.key).Str(cell.value);
            }
            return w.data;
        }

        bool Decode(Reader& r, Change& c)
        {
            c.author = r.Id();
            c.seq = r.U64();
            c.at = r.I64();
            uint16_t n = r.U16();
            for (uint16_t i = 0; i < n && r.ok(); ++i)
            {
                Op op;
                op.put = r.Bool();
                op.table = r.Str(256);
                uint16_t cells = r.U16();
                for (uint16_t k = 0; k < cells && r.ok(); ++k)
                {
                    Cell cell;
                    cell.name = r.Str(256);
                    cell.key = r.Bool();
                    cell.value = r.Str(1 << 20);
                    op.cells.push_back(std::move(cell));
                }
                c.ops.push_back(std::move(op));
            }
            return r.ok();
        }

        int64_t IntOf(std::string const& token) { return token.size() > 1 && token[0] == 'I' ? strtoll(token.c_str() + 1, nullptr, 10) : -1; }

        // a value of the text form as an SQL literal
        std::string Literal(std::string const& token)
        {
            if (token.empty() || token[0] == 'N')
                return "NULL";
            if (token[0] == 'I' || token[0] == 'F')
                return token.substr(1);
            Bytes raw;
            FromHex(token.substr(1), raw);
            if (token[0] == 'B')
                return "X'" + token.substr(1) + "'";
            std::string text(raw.begin(), raw.end());
            WorldDatabase.EscapeString(text);
            return "'" + text + "'";
        }

        thread_local bool g_applying = false;   // this thread is writing a change that came from another server: the listener ignores the commits it makes

        class WorldShare : public Feature
        {
        public:
            static WorldShare& Get()
            {
                static WorldShare share;
                return share;
            }

            // the shim's listener: any thread
            static void Listener(char const* database, char const* rows)
            {
                if (g_applying || !database || std::string(database) != "world")
                    return;
                WorldShare& self = Get();
                if (!self._on.load())
                    return;
                std::lock_guard<std::mutex> lock(self._queueMutex);
                if (self._queue.size() < 10000)
                    self._queue.push_back(rows);
            }

            void Start() { mysql_sqlite_changes_listen(&WorldShare::Listener); }

            void OnLogin(Player*) override { Load(); }
            void OnLogout(Player*) override { _on = false; }
            void OnRealmUnbind() override { _loaded = false; _on = false; _have.clear(); _map.clear(); _rowAt.clear(); }

            void Update(uint32_t diff, Player* local) override
            {
                if (!_loaded)
                    Load();
                if (!_on)
                    return;
                std::deque<std::string> batches;
                {
                    std::lock_guard<std::mutex> lock(_queueMutex);
                    batches.swap(_queue);
                }
                for (std::string const& rows : batches)
                    Local(rows);
            }

            void OnEvent(InEvent const& e, Player* local) override
            {
                if (!_on || !local)
                    return;
                if (e.kind == InEvent::LinkUp)
                {
                    SendHave(e.peer);
                    return;
                }
                if (e.kind != InEvent::Message || e.type != game::MsgWorldChange)
                    return;
                Reader r(e.data);
                uint8_t step = r.U8();
                if (step == StepHave)
                    OnHave(e.peer, r);
                else if (step == StepRecords)
                    OnRecords(e.peer, r, local);
            }

        private:
            // ---- the state of the realm
            void Load()
            {
                _loaded = true;
                _on = false;
                // the realms whose world is shared (a setting; Sandbox is realm 2)
                std::string realms = sConfigMgr->GetOption<std::string>("P2P.ShareWorldChanges.Realms", "2");
                std::istringstream list(realms);
                std::string item;
                bool shared = false;
                while (std::getline(list, item, ','))
                    shared = shared || uint32(atoi(item.c_str())) == Service::Get().RealmId();
                if (!shared)
                    return;
                CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_world_change (author TEXT NOT NULL, seq INTEGER NOT NULL, at INTEGER NOT NULL, body TEXT NOT NULL, "
                    "PRIMARY KEY (author, seq))");
                CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_world_row (tbl TEXT NOT NULL, rowkey TEXT NOT NULL, at INTEGER NOT NULL, author TEXT NOT NULL, "
                    "PRIMARY KEY (tbl, rowkey))");
                CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_world_map (author TEXT NOT NULL, domain INTEGER NOT NULL, theirs INTEGER NOT NULL, ours INTEGER NOT NULL, "
                    "PRIMARY KEY (author, domain, theirs))");
                _have.clear();
                _map.clear();
                _rowAt.clear();
                if (QueryResult r = CharacterDatabase.Query("SELECT author, MAX(seq) FROM p2p_world_change GROUP BY author"))
                    do
                    {
                        Field* f = r->Fetch();
                        Id128 id{};
                        Bytes raw;
                        if (FromHex(f[0].Get<std::string>(), raw) && raw.size() == 16)
                        {
                            memcpy(id.data(), raw.data(), 16);
                            _have[id] = f[1].Get<uint64>();
                        }
                    } while (r->NextRow());
                if (QueryResult r = CharacterDatabase.Query("SELECT author, domain, theirs, ours FROM p2p_world_map"))
                    do
                    {
                        Field* f = r->Fetch();
                        _map[{f[0].Get<std::string>(), uint8_t(f[1].Get<uint8>()), f[2].Get<uint64>()}] = f[3].Get<uint64>();
                    } while (r->NextRow());
                _customStart = sConfigMgr->GetOption<uint32>("World.CustomIdStart", 0);
                _on = true;
                LOG_INFO("module", "[P2P] The world's changes are shared on realm {} ({} authors known)", Service::Get().RealmId(), _have.size());
            }

            // ---- a change made here
            void Local(std::string const& rows)
            {
                Id128 me = Service::Get().Me().id;
                if (me == Id128{})
                    return;
                Change c;
                c.author = me;
                c.seq = _have[me] + 1;
                c.at = Service::Get().NetNow();
                c.ops = ParseRows(rows);
                if (c.ops.empty())
                    return;
                // rows of tables whose keys are all unknown here are not told (a table without a key column is not in the list)
                _have[me] = c.seq;
                Store(c);
                for (Op const& op : c.ops)
                    Won(op, c.author, c.at, nullptr);   // (this change is the latest we know of its rows)
                Spread(c, Id128{});
                LOG_INFO("module", "[P2P] World change {} of {} told ({} rows)", c.seq, Hex(c.author).substr(0, 8), c.ops.size());
            }

            void Store(Change const& c)
            {
                CharacterDatabase.DirectExecute("REPLACE INTO p2p_world_change (author, seq, at, body) VALUES ('{}', {}, {}, '{}')", Hex(c.author), c.seq, c.at, Hex(Encode(c)));
            }

            // the change goes to every server in touch (but 'except')
            void Spread(Change const& c, Id128 const& except)
            {
                Writer w;
                w.U8(StepRecords).U16(1);
                w.Blob(Encode(c));
                for (auto const& [id, p] : Service::Get().Peers())
                    if (p.linked && !(id == except))
                        Service::Get().Say(id, game::MsgWorldChange, w.data);
            }

            // ---- the comparison when a link comes up
            void SendHave(Id128 const& peer)
            {
                Writer w;
                w.U8(StepHave).U16(static_cast<uint16_t>(_have.size()));
                for (auto const& [author, seq] : _have)
                    w.Id(author).U64(seq);
                Service::Get().Say(peer, game::MsgWorldChange, w.data);
            }

            void OnHave(Id128 const& peer, Reader& r)
            {
                std::map<Id128, uint64_t> theirs;
                uint16_t n = r.U16();
                for (uint16_t i = 0; i < n && r.ok(); ++i)
                {
                    Id128 author = r.Id();
                    theirs[author] = r.U64();
                }
                if (!r.ok())
                    return;
                // what this server holds beyond what the peer holds, oldest first, in messages of a few records
                for (auto const& [author, mine] : _have)
                {
                    uint64_t from = theirs.count(author) ? theirs[author] : 0;
                    while (from < mine)
                    {
                        QueryResult q = CharacterDatabase.Query("SELECT body FROM p2p_world_change WHERE author = '{}' AND seq > {} ORDER BY seq LIMIT 20", Hex(author), from);
                        if (!q)
                            break;
                        Writer w;
                        w.U8(StepRecords);
                        std::vector<Bytes> bodies;
                        uint64_t last = from;
                        do
                        {
                            Bytes raw;
                            if (FromHex(q->Fetch()[0].Get<std::string>(), raw))
                            {
                                Reader rr(raw);
                                Id128 a = rr.Id();
                                (void)a;
                                last = std::max<uint64_t>(last, rr.U64());
                                bodies.push_back(std::move(raw));
                            }
                        } while (q->NextRow());
                        if (bodies.empty() || last <= from)
                            break;
                        w.U16(static_cast<uint16_t>(bodies.size()));
                        for (Bytes const& b : bodies)
                            w.Blob(b);
                        Service::Get().Say(peer, game::MsgWorldChange, w.data);
                        from = last;
                    }
                }
            }

            // ---- records of other servers
            void OnRecords(Id128 const& from, Reader& r, Player* local)
            {
                uint16_t n = r.U16();
                TouchedRows touched;
                std::vector<Change> fresh;
                for (uint16_t i = 0; i < n && r.ok(); ++i)
                {
                    Bytes body = r.Blob(1 << 22);
                    Reader rr(body);
                    Change c;
                    if (!r.ok() || !Decode(rr, c) || c.author == Id128{} || c.author == Service::Get().Me().id)
                        continue;
                    if (c.seq <= _have[c.author] && _have[c.author] != 0 && Known(c))
                        continue;   // (already held)
                    _have[c.author] = std::max(_have[c.author], c.seq);
                    Store(c);
                    Apply(c, touched);
                    fresh.push_back(std::move(c));
                }
                if (!touched.Empty())
                    ReloadWorldRows(touched, true);
                for (Change const& c : fresh)
                    Spread(c, from);   // (passed on: a server in touch with this one and not with the author hears of it)
                (void)local;
            }

            bool Known(Change const& c)
            {
                QueryResult q = CharacterDatabase.Query("SELECT 1 FROM p2p_world_change WHERE author = '{}' AND seq = {}", Hex(c.author), c.seq);
                return q != nullptr;
            }

            // ---- the keys of other authors' rows
            static Ref const* RefOf(std::string const& table, std::string const& column)
            {
                for (Ref const& ref : Refs)
                    if (table == ref.table && column == ref.column)
                        return &ref;
                return nullptr;
            }

            // is this identifier free here (no row of the world has it, no other author's row was given it)?
            bool Free(Domain domain, uint64_t id)
            {
                for (auto const& [key, ours] : _map)
                    if (std::get<1>(key) == domain && ours == id)
                        return false;
                switch (domain)
                {
                    case DSpawn:  return !sObjectMgr->GetCreatureData(uint32(id));
                    case DObject: return !sObjectMgr->GetGameObjectData(uint32(id));
                    default: break;
                }
                char const* sql = domain == DTele ? "SELECT 1 FROM game_tele WHERE id = {}" : domain == DPath ? "SELECT 1 FROM waypoint_data WHERE id = {}" : "SELECT 1 FROM waypoint_scripts WHERE guid = {}";
                QueryResult q = WorldDatabase.Query(sql, id);
                return !q;
            }

            // The identifier for another author's row: the author's own number when it is free here (the creatures of the shared world are known by their spawn number on every
            // server, so the same number is the best), else the next number of this server's own range. The generator of the core is moved past the number taken.
            uint64_t NextOurs(Domain domain, uint64_t theirs)
            {
                bool useTheirs = Free(domain, theirs);
                switch (domain)
                {
                    case DSpawn:
                    case DObject:
                    {
                        for (int guard = 0; guard < 100000; ++guard)
                        {
                            uint64_t g = domain == DSpawn ? sObjectMgr->GenerateCreatureSpawnId() : sObjectMgr->GenerateGameObjectSpawnId();
                            if (useTheirs && g < theirs)
                                continue;   // (the generator is moved up to the number)
                            if (useTheirs && g > theirs)
                                return theirs;   // (it was past the number already: nothing of this server's has it)
                            return g;       // (g == theirs, or a number of the server's own)
                        }
                        return theirs;
                    }
                    default: break;
                }
                char const* sql = domain == DTele ? "SELECT MAX(id) FROM game_tele" : domain == DPath ? "SELECT MAX(id) FROM waypoint_data" : "SELECT MAX(guid) FROM waypoint_scripts";
                uint64_t top = 0;
                if (QueryResult q = WorldDatabase.Query("{}", sql))
                    top = q->Fetch()[0].Get<uint64>();
                // (a number given out and not yet written must not be given twice: the largest given so far is remembered)
                uint64_t& given = _given[domain];
                if (useTheirs)
                {
                    given = std::max<uint64_t>(given, theirs);
                    return theirs;
                }
                given = std::max<uint64_t>({ top + 1, uint64_t(_customStart), given + 1 });
                return given;
            }

            // an identifier of an author's range, as this server has it; 'create': a number is given if there is none yet
            bool Mapped(Id128 const& author, Domain domain, uint64_t theirs, bool create, uint64_t& ours)
            {
                if (theirs < _customStart || _customStart == 0 || author == Service::Get().Me().id)
                {
                    ours = theirs;   // (a row of the world as everyone has it, or this server's own)
                    return true;
                }
                auto key = std::make_tuple(Hex(author), uint8_t(domain), theirs);
                auto it = _map.find(key);
                if (it != _map.end())
                {
                    ours = it->second;
                    return true;
                }
                if (!create)
                    return false;
                ours = NextOurs(domain, theirs);
                _map[key] = ours;
                CharacterDatabase.DirectExecute("REPLACE INTO p2p_world_map (author, domain, theirs, ours) VALUES ('{}', {}, {}, {})", Hex(author), uint32(domain), theirs, ours);
                return true;
            }

            // the cells of the row with this server's identifiers (false: a row of another author's that this server never had, to be deleted)
            bool MapRow(Op& op, Id128 const& author)
            {
                for (Cell& cell : op.cells)
                {
                    Ref const* ref = RefOf(op.table, cell.name);
                    if (!ref || cell.value.empty() || cell.value[0] != 'I')
                        continue;
                    uint64_t ours = 0;
                    // a key of a row that is put is given a number; a delete (or a reference) of a row never seen is dropped
                    bool create = op.put;
                    if (!Mapped(author, ref->domain, uint64_t(IntOf(cell.value)), create, ours))
                        return false;
                    cell.value = "I" + std::to_string(ours);
                }
                return true;
            }

            static std::string RowKey(Op const& op)
            {
                std::string key;
                for (Cell const& cell : op.cells)
                    if (cell.key)
                        key += cell.name + "=" + cell.value + ",";
                return key;
            }

            // ---- the later change of a row wins
            // true: the change (at, author) is the latest known of the row and is recorded as such; false: a later one is held
            bool Won(Op const& op, Id128 const& author, int64_t at, bool* newer)
            {
                std::string table = op.table, key = RowKey(op);
                auto k = std::make_pair(table, key);
                auto it = _rowAt.find(k);
                if (it == _rowAt.end())
                {
                    if (QueryResult q = CharacterDatabase.Query("SELECT at, author FROM p2p_world_row WHERE tbl = '{}' AND rowkey = '{}'", table, Escaped(key)))
                    {
                        Id128 a{};
                        Bytes raw;
                        if (FromHex(q->Fetch()[1].Get<std::string>(), raw) && raw.size() == 16)
                            memcpy(a.data(), raw.data(), 16);
                        it = _rowAt.emplace(k, std::make_pair(q->Fetch()[0].Get<int64>(), a)).first;
                    }
                }
                bool wins = it == _rowAt.end() || at > it->second.first || (at == it->second.first && !(author < it->second.second));
                if (wins)
                {
                    _rowAt[k] = { at, author };
                    CharacterDatabase.DirectExecute("REPLACE INTO p2p_world_row (tbl, rowkey, at, author) VALUES ('{}', '{}', {}, '{}')", table, Escaped(key), at, Hex(author));
                }
                if (newer)
                    *newer = wins;
                return wins;
            }

            static std::string Escaped(std::string s)
            {
                CharacterDatabase.EscapeString(s);
                return s;
            }

            // ---- a record is applied
            void Apply(Change const& original, TouchedRows& touched)
            {
                g_applying = true;
                for (Op op : original.ops)
                {
                    // the row's key as this server has it
                    if (!MapRow(op, original.author))
                        continue;
                    if (!Won(op, original.author, original.at, nullptr))
                        continue;   // (a later change of the row is held: this one is kept in the table only)
                    std::string sql;
                    if (op.put)
                    {
                        std::string names, values;
                        for (Cell const& cell : op.cells)
                        {
                            names += (names.empty() ? "`" : ", `") + cell.name + "`";
                            values += (values.empty() ? "" : ", ") + Literal(cell.value);
                        }
                        sql = "REPLACE INTO `" + op.table + "` (" + names + ") VALUES (" + values + ")";
                    }
                    else
                    {
                        std::string where;
                        for (Cell const& cell : op.cells)
                            if (cell.key)
                                where += (where.empty() ? "" : " AND ") + std::string("`") + cell.name + "` = " + Literal(cell.value);
                        sql = "DELETE FROM `" + op.table + "` WHERE " + where;
                    }
                    WorldDatabase.DirectExecute("{}", sql);
                    // what to read again
                    if (op.table == "creature" || op.table == "gameobject")
                    {
                        for (Cell const& cell : op.cells)
                            if (cell.key && cell.name == "guid")
                                (op.table == "creature" ? touched.creatures : touched.objects).insert(uint32(IntOf(cell.value)));
                    }
                    else
                        touched.tables.insert(op.table);
                }
                g_applying = false;
            }

            bool _loaded = false;
            std::atomic<bool> _on{false};
            uint32_t _customStart = 0;
            std::map<Id128, uint64_t> _have;                                           // the highest sequence number held, by author
            std::map<std::tuple<std::string, uint8_t, uint64_t>, uint64_t> _map;       // (author, domain, theirs) -> ours
            std::map<std::pair<std::string, std::string>, std::pair<int64_t, Id128>> _rowAt;   // (table, key) -> the change it came from (time, author)
            std::map<uint8_t, uint64_t> _given;
            std::mutex _queueMutex;
            std::deque<std::string> _queue;
        };
    }

    void AddWorldShare()
    {
        Service::Get().AddFeature(&WorldShare::Get());
        WorldShare::Get().Start();
    }
}
