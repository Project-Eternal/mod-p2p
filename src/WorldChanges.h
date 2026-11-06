/*
 * The world's changes (WorldChanges.cpp, WorldShare.cpp): the rows of the world database that the core's tables were read from, read again.
 */
#pragma once
#include "Define.h"
#include <set>
#include <string>

namespace p2p
{
    // the rows that changed: the spawns of creatures and objects by their identifier, the other tables whole
    struct TouchedRows
    {
        std::set<uint32> creatures, objects;
        std::set<std::string> tables;
        bool Empty() const { return creatures.empty() && objects.empty() && tables.empty(); }
    };

    // The core's tables are read again from the rows. 'live': there is a player in the world, whose map is loaded: the spawns that changed are made again in it.
    void ReloadWorldRows(TouchedRows const& rows, bool live);
}
