/*
 * An item as it travels between servers and sits in the module's tables: not the object, a description from which an equal item is made again
 * (a mail's items, a trade's offers, the items sold to a vendor). The fields are those of the core's item_instance table that give an item its
 * identity for the player.
 */
#pragma once
#include "core/Codec.h"
#include <string>

class Item;
class Player;

namespace p2p
{
    struct ItemData
    {
        uint32_t entry = 0, count = 1, durability = 0, flags = 0;
        int32_t randomProperty = 0;
        std::string charges, enchantments;   // as in item_instance: charges "a b c d e", enchantments "id duration charges" for each enchantment slot
    };

    void Write(Writer& w, ItemData const& i);
    ItemData ReadItem(Reader& r);
    ItemData Describe(Item const* item);                    // from a live item
    bool Plausible(ItemData const& item);                   // the item exists and its count is within what the game allows
    Item* Create(Player* owner, ItemData const& item);      // a new item with that description (nullptr if it cannot be made); the caller places it
    // equal for the purpose of a trade: the same item, count, random property and enchantments (not the durability, charges or the time left of a temporary one)
    bool SameItem(ItemData const& a, ItemData const& b);
    inline bool operator==(ItemData const& a, ItemData const& b) { return SameItem(a, b); }
}
