/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LootObjectStack.h"
#include "LootMgr.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Unit.h"

#define MAX_LOOT_OBJECT_COUNT 200

LootTarget::LootTarget(ObjectGuid guid) : guid(guid), asOfTime(time(nullptr)) {}

LootTarget::LootTarget(LootTarget const& other)
{
    guid = other.guid;
    asOfTime = other.asOfTime;
}

LootTarget& LootTarget::operator=(LootTarget const& other)
{
    if ((void*)this == (void*)&other)
        return *this;

    guid = other.guid;
    asOfTime = other.asOfTime;

    return *this;
}

bool LootTarget::operator<(LootTarget const& other) const { return guid < other.guid; }

void LootTargetList::shrink(time_t fromTime)
{
    for (std::set<LootTarget>::iterator i = begin(); i != end();)
    {
        if (i->asOfTime <= fromTime)
            erase(i++);
        else
            ++i;
    }
}

LootObject::LootObject(Player* bot, ObjectGuid guid)
    : guid(), skillId(SKILL_NONE), reqSkillValue(0), reqItem(0), isNeededQuestItem(false)
{
    Refresh(bot, guid);
}

void LootObject::Refresh(Player* bot, ObjectGuid lootGUID)
{
    skillId = SKILL_NONE;
    reqSkillValue = 0;
    reqItem = 0;
    isNeededQuestItem = false;
    guid.Clear();

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return;
    }
    Creature* creature = botAI->GetCreature(lootGUID);
    if (creature && creature->getDeathState() == DeathState::Corpse)
    {
        if (creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
            guid = lootGUID;

        if (creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE))
        {
            skillId = creature->GetCreatureTemplate()->GetRequiredLootSkill();
            uint32 targetLevel = creature->GetLevel();
            reqSkillValue = targetLevel < 10 ? 1 : targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5;
            if (botAI->HasSkill((SkillType)skillId) && bot->GetSkillValue(skillId) >= reqSkillValue)
                guid = lootGUID;
        }

        return;
    }

    GameObject* go = botAI->GetGameObject(lootGUID);
    if (go && go->isSpawned() && go->GetGoState() == GO_STATE_READY)
    {
        bool onlyHasQuestItems = true;
        bool hasAnyQuestItems = false;
        bool neededQuestItem = false;

        GameObjectQuestItemList const* items = sObjectMgr->GetGameObjectQuestItemList(go->GetEntry());
        for (size_t i = 0; i < MAX_GAMEOBJECT_QUEST_ITEMS; i++)
        {
            if (!items || i >= items->size())
                break;

            uint32 itemId = uint32((*items)[i]);
            if (!itemId)
                continue;

            hasAnyQuestItems = true;

            if (IsNeededForQuest(bot, itemId))
            {
                // A gathering node can also drop a needed quest item (e.g.
                // Root Sample off Barrens herbs); gathering yields both, so
                // keep reading the lock below to set skillId rather than
                // bailing here.
                this->guid = lootGUID;
                this->isNeededQuestItem = true;
                neededQuestItem = true;
            }

            const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto)
                continue;

            if (proto->Class != ITEM_CLASS_QUEST)
            {
                onlyHasQuestItems = false;
            }
        }

        // Retrieve the correct loot table entry
        uint32 lootEntry = go->GetGOInfo()->GetLootId();
        if (lootEntry == 0)
            return;

        // Check the main loot template
        if (const LootTemplate* lootTemplate = LootTemplates_Gameobject.GetLootFor(lootEntry))
        {
            Loot loot;
            lootTemplate->Process(loot, LootTemplates_Gameobject, 1, bot);

            // Loot::AddItem routes QuestRequired entries to quest_items, never
            // to items, so the scan below cannot see them — Corrupted Flower
            // (quest 9799) and every other QuestRequired drop went unflagged.
            // Same handling as the gameobject_questitem loop above: flag and
            // keep reading, a gathering node may still need its skill.
            for (const LootItem& item : loot.quest_items)
            {
                if (!item.itemid || !IsNeededForQuest(bot, item.itemid))
                    continue;

                this->guid = lootGUID;
                this->isNeededQuestItem = true;
                neededQuestItem = true;
            }

            for (const LootItem& item : loot.items)
            {
                uint32 itemId = item.itemid;
                if (!itemId)
                    continue;

                const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                // Moonpetal Lily, Hyacinth Mushroom etc. expose quest
                // drops here (not in gameobject_questitem). Flag it so
                // the INTERACT_COND gate lets the bot through.
                if (IsNeededForQuest(bot, itemId))
                {
                    this->guid = lootGUID;
                    this->isNeededQuestItem = true;
                    return;
                }

                if (proto->Class != ITEM_CLASS_QUEST)
                {
                    onlyHasQuestItems = false;
                    // keep scanning — a later item may be needed
                    continue;
                }

                // If this item references another loot table, process it
                if (const LootTemplate* refLootTemplate = LootTemplates_Reference.GetLootFor(itemId))
                {
                    Loot refLoot;
                    refLootTemplate->Process(refLoot, LootTemplates_Reference, 1, bot);

                    for (const LootItem& refItem : refLoot.items)
                    {
                        uint32 refItemId = refItem.itemid;
                        if (!refItemId)
                            continue;

                        const ItemTemplate* refProto = sObjectMgr->GetItemTemplate(refItemId);
                        if (!refProto)
                            continue;

                        if (IsNeededForQuest(bot, refItemId))
                        {
                            this->guid = lootGUID;
                            this->isNeededQuestItem = true;
                            return;
                        }

                        if (refProto->Class != ITEM_CLASS_QUEST)
                            onlyHasQuestItems = false;
                    }
                }
            }
        }

        // If gameobject has only quest items that bot doesn’t need, skip it.
        if (!neededQuestItem && hasAnyQuestItems && onlyHasQuestItems)
            return;

        // Otherwise, loot it.
        guid = lootGUID;

        uint32 goId = go->GetEntry();
        uint32 lockId = go->GetGOInfo()->GetLockId();
        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
            return;

        // A lock holds up to 8 alternative ways in; the client opens the
        // object as soon as ONE of them is satisfied. Recording a skill
        // requirement unconditionally kept only the last one and hid any
        // free alternative — Corrupted Flower (lock 259) pairs
        // OPEN_KNEELING, which needs no skill, with HERBALISM, so every
        // non-herbalist bot was refused an object a player right-clicks.
        bool hasFreeEntry = false;
        for (uint8 i = 0; i < 8; ++i)
        {
            switch (lockInfo->Type[i])
            {
                case LOCK_KEY_ITEM:
                    if (lockInfo->Index[i] > 0)
                    {
                        reqItem = lockInfo->Index[i];
                        guid = lootGUID;
                    }
                    break;

                case LOCK_KEY_SKILL:
                    if (goId == 13891 || goId == 19535)  // Serpentbloom
                    {
                        this->guid = lootGUID;
                        hasFreeEntry = true;
                    }
                    else if (SkillByLockType(LockType(lockInfo->Index[i])) > 0)
                    {
                        skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        reqSkillValue = std::max((uint32)1, lockInfo->Skill[i]);
                        guid = lootGUID;
                    }
                    else if (lockInfo->Index[i] > 0)
                    {
                        // OPEN, TREASURE, OPEN_KNEELING… map to SKILL_NONE:
                        // an opening spell is enough, no profession needed.
                        guid = lootGUID;
                        hasFreeEntry = true;
                    }
                    break;

                case LOCK_KEY_NONE:
                    guid = lootGUID;
                    break;
            }
        }

        if (hasFreeEntry)
        {
            skillId = SKILL_NONE;
            reqSkillValue = 0;
        }
    }
}

bool LootObject::IsNeededForQuest(Player* bot, uint32 itemId)
{
    for (int qs = 0; qs < MAX_QUEST_LOG_SIZE; ++qs)
    {
        uint32 questId = bot->GetQuestSlotQuestId(qs);
        if (questId == 0)
            continue;

        QuestStatusData& qData = bot->getQuestStatusMap()[questId];
        if (qData.Status != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* qInfo = sObjectMgr->GetQuestTemplate(questId);
        if (!qInfo)
            continue;

        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (!qInfo->RequiredItemCount[i] || (qInfo->RequiredItemCount[i] - qData.ItemCount[i]) <= 0)
                continue;

            if (qInfo->RequiredItemId[i] != itemId)
                continue;

            return true;
        }
    }

    return false;
}

WorldObject* LootObject::GetWorldObject(Player* bot)
{
    Refresh(bot, guid);

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return nullptr;
    }
    Creature* creature = botAI->GetCreature(guid);
    if (creature && creature->getDeathState() == DeathState::Corpse && creature->IsInWorld())
        return creature;

    GameObject* go = botAI->GetGameObject(guid);
    if (go && go->isSpawned() && go->IsInWorld())
        return go;

    return nullptr;
}

LootObject::LootObject(LootObject const& other)
{
    guid = other.guid;
    skillId = other.skillId;
    reqSkillValue = other.reqSkillValue;
    reqItem = other.reqItem;
    isNeededQuestItem = other.isNeededQuestItem;
}

bool LootObject::IsLootPossible(Player* bot)
{
    if (IsEmpty() || !bot)
        return false;

    WorldObject* worldObj = GetWorldObject(bot);  // Store result to avoid multiple calls
    if (!worldObj)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        return false;
    }
    if (reqItem && !bot->HasItemCount(reqItem, 1))
        return false;

    if (abs(worldObj->GetPositionZ() - bot->GetPositionZ()) > INTERACTION_DISTANCE - 2.0f)
        return false;

    Creature* creature = botAI->GetCreature(guid);
    if (creature && creature->getDeathState() == DeathState::Corpse)
    {
        if (!bot->isAllowedToLoot(creature) && skillId != SKILL_SKINNING)
            return false;
    }

    // Block event-gated chests (Gunship Armory pre-event) and unspawned
    // GOs. INTERACT_COND alone is allowed when the GO holds a quest
    // item we need — ConditionMgr already gates on quest state.
    GameObject* go = botAI->GetGameObject(guid);
    if (go && (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE) || !go->isSpawned()))
        return false;

    // Conditional objects (quest chests, goobers, ...) are gated client-side on quest state.
    // A bot has no client, so make the same call the server makes for one (upstream #2579).
    // We keep one extra term upstream does not have: isNeededQuestItem, fed from
    // gameobject_questitem, covers GOs ActivateToQuest answers no for while the bot does need
    // the item. Dropping it re-blocks the loots BUG-TC9-062 measured back to zero rejections.
    if (go && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) && !isNeededQuestItem &&
        !go->ActivateToQuest(bot))
        return false;

    if (skillId == SKILL_NONE)
        return true;

    if (skillId == SKILL_FISHING)
        return false;

    if (!botAI->HasSkill((SkillType)skillId))
        return false;

    if (!reqSkillValue)
        return true;

    uint32 skillValue = uint32(bot->GetSkillValue(skillId));
    if (reqSkillValue > skillValue)
        return false;

    if (skillId == SKILL_MINING && !bot->HasItemCount(756, 1) && !bot->HasItemCount(778, 1) &&
        !bot->HasItemCount(1819, 1) && !bot->HasItemCount(1893, 1) && !bot->HasItemCount(1959, 1) &&
        !bot->HasItemCount(2901, 1) && !bot->HasItemCount(9465, 1) && !bot->HasItemCount(20723, 1) &&
        !bot->HasItemCount(40772, 1) && !bot->HasItemCount(40892, 1) && !bot->HasItemCount(40893, 1))
    {
        return false;  // Bot is missing a mining pick
    }

    if (skillId == SKILL_SKINNING && !bot->HasItemCount(7005, 1) && !bot->HasItemCount(40772, 1) &&
        !bot->HasItemCount(40893, 1) && !bot->HasItemCount(12709, 1) && !bot->HasItemCount(19901, 1))
    {
        return false;  // Bot is missing a skinning knife
    }

    return true;
}

bool LootObjectStack::Add(ObjectGuid guid)
{
    // expire old completed entries so a despawn/respawn with a reused
    // guid can still be looted later
    completedLoot.shrink(time(nullptr) - 300);

    if (completedLoot.find(guid) != completedLoot.end())
        return false;

    if (availableLoot.size() >= MAX_LOOT_OBJECT_COUNT)
    {
        availableLoot.shrink(time(nullptr) - 30);
    }

    if (availableLoot.size() >= MAX_LOOT_OBJECT_COUNT)
    {
        availableLoot.clear();
    }

    if (!availableLoot.insert(guid).second)
        return false;

    return true;
}

void LootObjectStack::Remove(ObjectGuid guid)
{
    LootTargetList::iterator i = availableLoot.find(guid);
    if (i != availableLoot.end())
        availableLoot.erase(i);
}

void LootObjectStack::MarkCompleted(ObjectGuid guid)
{
    Remove(guid);

    // Corpses only. "add all loot" re-reads "nearest corpses" without a
    // lootable filter, so a plain Remove lets the same corpse back into
    // the stack on the very next tick — that is what this list is for.
    // Gameobjects need no such guard: Refresh() already drops anything
    // that is not spawned and GO_STATE_READY. Blacklisting one is
    // actively harmful, because DoLoot() reports success as soon as the
    // opening is *started*, so an opening that yields nothing hides the
    // object from this bot for the full 300 s window.
    if (guid.IsAnyTypeCreature())
        completedLoot.insert(guid);
}

void LootObjectStack::Clear()
{
    availableLoot.clear();
    completedLoot.clear();
}

bool LootObjectStack::CanLoot(float maxDistance)
{
    LootObject nearest = GetNearest(maxDistance);
    return !nearest.IsEmpty();
}

LootObject LootObjectStack::GetLoot(float maxDistance)
{
    LootObject nearest = GetNearest(maxDistance);
    return nearest.IsEmpty() ? LootObject() : nearest;
}

LootObject LootObjectStack::GetNearest(float maxDistance)
{
    availableLoot.shrink(time(nullptr) - 30);

    LootObject nearest;
    float nearestDistance = std::numeric_limits<float>::max();

    LootTargetList safeCopy(availableLoot);
    for (LootTargetList::iterator i = safeCopy.begin(); i != safeCopy.end(); i++)
    {
        ObjectGuid guid = i->guid;

        WorldObject* worldObj = ObjectAccessor::GetWorldObject(*bot, guid);
        if (!worldObj)
            continue;

        float distance = bot->GetDistance(worldObj);

        if (distance >= nearestDistance || (maxDistance && distance > maxDistance))
            continue;

        LootObject lootObject(bot, guid);

        if (!lootObject.IsLootPossible(bot))
            continue;

        nearestDistance = distance;
        nearest = lootObject;
    }

    return nearest;
}
