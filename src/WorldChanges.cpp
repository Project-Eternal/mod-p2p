/*
 * The world's changes follow the realm.
 *
 * A game master's commands change the world database: they spawn and move creatures and objects, give a vendor goods, lay paths, add teleport places and
 * graveyards and disable spells. The SQLite shim keeps such changes as a change log in the realm's directory (src/changelog.cpp of the shim). When the server
 * changes realm, the shim takes the changes of the realm that is left out of the database, puts those of the realm that is entered in, and reports the rows
 * both logs touch; the core's tables that were read from those rows are read again here, table by table (the `.reload` model) or, for the spawns of creatures
 * and objects, row by row (the maps are unloaded with the realm, so a spawn only has to be known to the grids; it appears when its grid is loaded).
 *
 * The reload of a table that has none here is logged ("changed, not reloaded"): that is a gap to close, not a change to leave out.
 */
#include "WorldChanges.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "GameGraveyard.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "P2PService.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "ScriptMgr.h"
#include "Transport.h"
#include "WaypointMgr.h"
#include "WorldScript.h"
#include <map>
#include <set>
#include <sstream>
#include <string>

extern "C" char const* mysql_sqlite_changes_switch(char const* database);   // the SQLite shim (src/mysql_sqlite.cpp)

namespace p2p
{
    namespace
    {
        // creature: the row of one spawn is read again (and the spawn forgotten when the row is gone)
        void ReloadCreature(uint32 spawnId)
        {
            if (CreatureData const* old = sObjectMgr->GetCreatureData(spawnId))
            {
                sObjectMgr->RemoveCreatureFromGrid(spawnId, old);
                sObjectMgr->DeleteCreatureData(spawnId);
            }
            QueryResult result = WorldDatabase.Query("SELECT creature.guid, id, map, equipment_id, position_x, position_y, position_z, orientation, spawntimesecs, wander_distance, "
                "currentwaypoint, curhealth, curmana, MovementType, spawnMask, phaseMask, eventEntry, pool_entry, creature.npcflag, creature.unit_flags, creature.dynamicflags, "
                "creature.ScriptName FROM creature LEFT OUTER JOIN game_event_creature ON creature.guid = game_event_creature.guid "
                "LEFT OUTER JOIN pool_creature ON creature.guid = pool_creature.guid WHERE creature.guid = {}", spawnId);
            if (!result)
                return;
            Field* fields = result->Fetch();
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(fields[1].Get<uint32>());
            if (!info)
            {
                LOG_ERROR("module", "[P2P] The creature spawn {} of the world's changes has the unknown entry {}", spawnId, fields[1].Get<uint32>());
                return;
            }
            CreatureData& data = sObjectMgr->NewOrExistCreatureData(spawnId);
            data.spawnId = spawnId;
            data.id = info->Entry;
            data.mapid = fields[2].Get<uint16>();
            data.equipmentId = fields[3].Get<int8>();
            data.posX = fields[4].Get<float>();
            data.posY = fields[5].Get<float>();
            data.posZ = fields[6].Get<float>();
            data.orientation = fields[7].Get<float>();
            data.spawntimesecs = fields[8].Get<uint32>();
            data.wander_distance = fields[9].Get<float>();
            data.currentwaypoint = fields[10].Get<uint32>();
            data.curhealth = fields[11].Get<uint32>();
            data.curmana = fields[12].Get<uint32>();
            data.movementType = fields[13].Get<uint8>();
            data.spawnMask = fields[14].Get<uint8>();
            data.phaseMask = fields[15].Get<uint32>() ? fields[15].Get<uint32>() : 1;
            int16 gameEvent = fields[16].Get<int16>();
            data.poolId = fields[17].Get<uint32>();
            data.npcflag = fields[18].Get<uint32>();
            data.unit_flags = fields[19].Get<uint32>();
            data.dynamicflags = fields[20].Get<uint32>();
            data.ScriptId = sObjectMgr->GetScriptId(fields[21].Get<std::string>());
            if (!data.ScriptId)
                data.ScriptId = info->ScriptID;
            data.spawnGroupId = 0;
            if (gameEvent == 0)
                sObjectMgr->AddCreatureToGrid(spawnId, &data);
        }

        // gameobject: the same for one object
        void ReloadGameObject(uint32 spawnId)
        {
            if (GameObjectData const* old = sObjectMgr->GetGameObjectData(spawnId))
            {
                sObjectMgr->RemoveGameobjectFromGrid(spawnId, old);
                sObjectMgr->DeleteGOData(spawnId);
            }
            QueryResult result = WorldDatabase.Query("SELECT gameobject.guid, id, map, position_x, position_y, position_z, orientation, rotation0, rotation1, rotation2, rotation3, "
                "spawntimesecs, animprogress, state, spawnMask, phaseMask, eventEntry, pool_entry, ScriptName FROM gameobject "
                "LEFT OUTER JOIN game_event_gameobject ON gameobject.guid = game_event_gameobject.guid LEFT OUTER JOIN pool_gameobject ON gameobject.guid = pool_gameobject.guid "
                "WHERE gameobject.guid = {}", spawnId);
            if (!result)
                return;
            Field* fields = result->Fetch();
            GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(fields[1].Get<uint32>());
            if (!info)
            {
                LOG_ERROR("module", "[P2P] The object spawn {} of the world's changes has the unknown entry {}", spawnId, fields[1].Get<uint32>());
                return;
            }
            GameObjectData& data = sObjectMgr->NewGOData(spawnId);
            data.spawnId = spawnId;
            data.id = info->entry;
            data.mapid = fields[2].Get<uint16>();
            data.posX = fields[3].Get<float>();
            data.posY = fields[4].Get<float>();
            data.posZ = fields[5].Get<float>();
            data.orientation = fields[6].Get<float>();
            data.rotation.x = fields[7].Get<float>();
            data.rotation.y = fields[8].Get<float>();
            data.rotation.z = fields[9].Get<float>();
            data.rotation.w = fields[10].Get<float>();
            data.spawntimesecs = fields[11].Get<int32>();
            data.animprogress = fields[12].Get<uint8>();
            data.artKit = 0;
            uint32 state = fields[13].Get<uint8>();
            data.go_state = GOState(state < MAX_GO_STATE ? state : GO_STATE_ACTIVE);
            data.spawnMask = fields[14].Get<uint8>();
            data.phaseMask = fields[15].Get<uint32>() ? fields[15].Get<uint32>() : 1;
            int16 gameEvent = fields[16].Get<int16>();
            data.poolId = fields[17].Get<uint32>();
            data.ScriptId = sObjectMgr->GetScriptId(fields[18].Get<std::string>());
            if (!data.ScriptId)
                data.ScriptId = info->ScriptId;
            data.spawnGroupId = 0;
            if (gameEvent == 0)
                sObjectMgr->AddGameobjectToGrid(spawnId, &data);
        }

        TouchedRows Parse(char const* report)
        {
            TouchedRows t;
            std::istringstream lines(report);
            std::string line;
            while (std::getline(lines, line))
            {
                size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
                if (b == std::string::npos)
                    continue;
                std::string table = line.substr(a + 1, b - a - 1), key = line.substr(b + 1);
                if (table == "creature")
                    t.creatures.insert(uint32(strtoul(key.c_str(), nullptr, 10)));
                else if (table == "gameobject")
                    t.objects.insert(uint32(strtoul(key.c_str(), nullptr, 10)));
                else
                    t.tables.insert(table);
            }
            return t;
        }

        // a spawn that changed is made again in the map that is loaded (its old one is removed first)
        void LiveCreature(Map* map, uint32 spawn)
        {
            std::vector<Creature*> old;
            auto range = map->GetCreatureBySpawnIdStore().equal_range(spawn);
            for (auto it = range.first; it != range.second; ++it)
                old.push_back(it->second);
            for (Creature* c : old)
                c->AddObjectToRemoveList();
            CreatureData const* data = sObjectMgr->GetCreatureData(spawn);
            if (!data || data->mapid != map->GetId() || !map->IsGridLoaded(data->posX, data->posY))
                return;
            Creature* creature = new Creature();
            if (!creature->LoadCreatureFromDB(spawn, map, true, true))
                delete creature;
        }

        void LiveGameObject(Map* map, uint32 spawn)
        {
            std::vector<GameObject*> old;
            auto range = map->GetGameObjectBySpawnIdStore().equal_range(spawn);
            for (auto it = range.first; it != range.second; ++it)
                old.push_back(it->second);
            for (GameObject* go : old)
                go->AddObjectToRemoveList();
            GameObjectData const* data = sObjectMgr->GetGameObjectData(spawn);
            if (!data || data->mapid != map->GetId() || !map->IsGridLoaded(data->posX, data->posY))
                return;
            GameObject* go = sObjectMgr->IsGameObjectStaticTransport(data->id) ? new StaticTransport() : new GameObject();
            if (!go->LoadGameObjectFromDB(spawn, map, true))
                delete go;
        }
    }

    // the core's tables read again from the rows that changed; the others are said to be missing
    void ReloadWorldRows(TouchedRows const& t, bool live)
    {
        {
            for (uint32 spawn : t.creatures)
                ReloadCreature(spawn);
            for (uint32 spawn : t.objects)
                ReloadGameObject(spawn);
            if (live)
                if (Player* local = Service::Get().Local())
                    if (local->IsInWorld())
                    {
                        for (uint32 spawn : t.creatures)
                            LiveCreature(local->GetMap(), spawn);
                        for (uint32 spawn : t.objects)
                            LiveGameObject(local->GetMap(), spawn);
                    }
            for (std::string const& table : t.tables)
            {
                if (table == "npc_vendor")                    sObjectMgr->LoadVendors();
                else if (table == "game_tele")                sObjectMgr->LoadGameTele();
                else if (table == "graveyard_zone")           sGraveyard->LoadGraveyardZones();
                else if (table == "disables")                 sDisableMgr->LoadDisables();
                else if (table == "waypoint_data")            sWaypointMgr->Load();
                else if (table == "waypoint_scripts")         sObjectMgr->LoadWaypointScripts();
                else if (table == "creature_addon")           sObjectMgr->LoadCreatureAddons();
                else if (table == "gameobject_addon")         sObjectMgr->LoadGameObjectAddons();
                else if (table == "creature_formations")      sFormationMgr->LoadCreatureFormations();
                else if (table == "linked_respawn" || table == "creature_linked_respawn") sObjectMgr->LoadLinkedRespawn();
                else
                {
                    LOG_WARN("module", "[P2P] The world's table `{}` changed with the realm, and has no reload here", table);
                    continue;
                }
                LOG_INFO("module", "[P2P] The world's table `{}` read again", table);
            }
            LOG_INFO("module", "[P2P] The world's rows read again: {} creature spawns, {} object spawns, {} other tables", t.creatures.size(), t.objects.size(), t.tables.size());
        }
    }

    namespace
    {
        class WorldChangesScript : public WorldScript
        {
        public:
            WorldChangesScript() : WorldScript("P2PWorldChanges", { WORLDHOOK_ON_AFTER_REALM_BIND }) { }

            // the core has bound a realm (the maps are gone, nobody is in the world): the world database follows
            void OnAfterRealmBind(uint32 realmId) override
            {
                char const* report = mysql_sqlite_changes_switch("world");
                if (!report)
                    return;   // (no change log: the world is the same for every realm)
                TouchedRows t = Parse(report);
                LOG_INFO("module", "[P2P] Realm {}: the world database follows ({} spawns of creatures, {} of objects, {} tables touched)", realmId, t.creatures.size(), t.objects.size(), t.tables.size());
                ReloadWorldRows(t, false);
            }
        };
    }

    void AddWorldChanges() { new WorldChangesScript(); }
}
