#include "Items.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Tokenize.h"
#include <algorithm>
#include <vector>

namespace p2p
{
    void Write(Writer& w, ItemData const& i)
    {
        w.U32(i.entry).U32(i.count).U32(i.durability).U32(i.flags).U32(static_cast<uint32_t>(i.randomProperty)).Str(i.charges).Str(i.enchantments);
    }

    ItemData ReadItem(Reader& r)
    {
        ItemData i;
        i.entry = r.U32(); i.count = r.U32(); i.durability = r.U32(); i.flags = r.U32();
        i.randomProperty = static_cast<int32_t>(r.U32());
        i.charges = r.Str(100);
        i.enchantments = r.Str(400);
        return i;
    }

    ItemData Describe(Item const* item)
    {
        ItemData d;
        d.entry = item->GetEntry();
        d.count = item->GetCount();
        d.durability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        d.flags = item->GetUInt32Value(ITEM_FIELD_FLAGS);
        d.randomProperty = item->GetItemRandomPropertyId();
        for (uint8 i = 0; i < 5; ++i)
            d.charges += (i ? " " : "") + std::to_string(item->GetSpellCharges(i));
        for (uint32 s = 0; s < MAX_ENCHANTMENT_SLOT; ++s)
            d.enchantments += (s ? " " : "") + std::to_string(item->GetEnchantmentId(EnchantmentSlot(s))) + " " +
                std::to_string(item->GetEnchantmentDuration(EnchantmentSlot(s))) + " " + std::to_string(item->GetEnchantmentCharges(EnchantmentSlot(s)));
        return d;
    }

    // the enchantment identifiers of an enchantments string ("id duration charges" for each slot)
    static std::vector<int> EnchantIds(std::string const& enchantments)
    {
        std::vector<int> ids;
        int n = 0;
        for (std::string_view token : Acore::Tokenize(enchantments, ' ', false))
            if (n++ % 3 == 0)
                ids.push_back(atoi(std::string(token).c_str()));
        return ids;
    }

    bool SameItem(ItemData const& a, ItemData const& b)
    {
        return a.entry == b.entry && a.count == b.count && a.randomProperty == b.randomProperty && EnchantIds(a.enchantments) == EnchantIds(b.enchantments);
    }

    bool Plausible(ItemData const& i)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(i.entry);
        return proto && i.count > 0 && i.count <= proto->GetMaxStackSize();
    }

    Item* Create(Player* owner, ItemData const& i)
    {
        if (!Plausible(i))
            return nullptr;
        Item* item = Item::CreateItem(i.entry, i.count, owner, false, 0);
        if (!item)
            return nullptr;
        if (i.randomProperty)
            item->SetItemRandomProperties(i.randomProperty);
        if (i.durability)
            item->SetUInt32Value(ITEM_FIELD_DURABILITY, std::min<uint32>(i.durability, item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY)));
        item->SetUInt32Value(ITEM_FIELD_FLAGS, i.flags);
        uint32 slot = 0;
        for (std::string_view token : Acore::Tokenize(i.charges, ' ', false))
            if (slot < 5)
                item->SetSpellCharges(slot++, atoi(std::string(token).c_str()));
        auto tokens = Acore::Tokenize(i.enchantments, ' ', false);
        for (uint32 s = 0; s < MAX_ENCHANTMENT_SLOT && s * 3 + 2 < tokens.size(); ++s)
            item->SetEnchantment(EnchantmentSlot(s), atoi(std::string(tokens[s * 3]).c_str()), atoi(std::string(tokens[s * 3 + 1]).c_str()),
                atoi(std::string(tokens[s * 3 + 2]).c_str()));
        return item;
    }
}
