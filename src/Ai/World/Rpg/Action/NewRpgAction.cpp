#include "NewRpgAction.h"

#include <cmath>
#include <cstdlib>

#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "G3D/Vector2.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;
    std::string out = botAI->rpgInfo.ToString();
    bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
    return true;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            // Arrival is handled inside NewRpgTravelFlightAction (it owns
            // take-off and landing); this timeout is the backstop so a
            // bot that never boards or never lands cleanly can't stay
            // wedged in this status. Never fires mid-flight.
            if (!bot->IsInFlight() && info.HasStatusPersisted(statusTravelFlightDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        auto statusItr = bot->getQuestStatusMap().find(questId);
        if (statusItr == bot->getQuestStatusMap().end())
        {
            // Quest vanished from the log (rewarded/dropped) while this
            // state was still active — stop pursuing it.
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        const QuestStatusData& q_status = statusItr->second;
        bool completed = true;
        // objectiveIdx is -1 while heading to turn-in; if the quest fell out
        // of COMPLETE again we have no valid objective — treat as completed
        // so a fresh objective is picked below.
        if (currentObjective >= 0 && currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective >= QUEST_OBJECTIVES_COUNT &&
                 currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
            data.pursuedLootGO.Clear();
            data.pursuedUseGO.Clear();
            data.pursuedUseTarget.Clear();
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        uint32 rndIdx = urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        // The top-down probe above lands on the topmost surface; for an
        // objective inside a cave that is the hill ABOVE it. Refine the
        // height from the objective's actual spawns when they exist near
        // the POI, so the destination is the cave floor, not the roof.
        dz = ResolveQuestPOIDestZ(data.quest, objectiveIdx, dx, dy, dz);

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
        data.pursuedLootGO.Clear();
        data.pursuedUseGO.Clear();
        data.pursuedUseTarget.Clear();
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        // yield to attack-anything if a quest mob is right next to us
        if (HasNearbyQuestMob(15.0f))
            return false;

        // Note: previously yielded ~10%/tick when any hostile was
        // within 25y. That overrode the do-quest multiplier in
        // practice (combined with bots getting aggroed on the way,
        // which ALSO bypasses the multiplier via combat engine) and
        // bots ended up grinding their way to POIs instead of
        // travelling. Quest-mob exception above is kept so we don't
        // walk past a quest target while gathering. Anything else
        // hostile is the multiplier's job to throttle — and bots
        // that DO get aggroed switch to combat engine where the
        // class strategy handles it.

        if (MoveFarTo(data.pos))
            return true;
        // sampler found nothing — nudge so next tick tries a new pos
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        auto statusItr = bot->getQuestStatusMap().find(questId);
        if (statusItr == bot->getQuestStatusMap().end())
        {
            // Quest vanished from the log (rewarded/dropped) while this
            // state was still active — stop pursuing it.
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        const QuestStatusData& q_status = statusItr->second;
        if (currentObjective >= 0 && currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective >= QUEST_OBJECTIVES_COUNT &&
                 currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        data.pursuedLootGO.Clear();
        data.pursuedUseGO.Clear();
        data.pursuedUseTarget.Clear();
        return true;
    }

    // at POI: drive toward specific objectives first
    if (TryUseQuestItem(data.pursuedUseGO, data.pursuedUseTarget))
        return true;
    if (TryLootQuestGO(data.pursuedLootGO))
        return true;
    if (TryUseQuestGO(data.pursuedUseGO))
        return true;

    // How long a POI may stay dry (no quest mob/GO in sight) before the
    // scout below rotates to a different cluster.
    constexpr uint32 scoutTimeoutMs = 30 * 1000;

    // gather quests: roam for spawns. kill quests: yield to grind.
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        int32 obj = data.objectiveIdx;
        bool isGatherObjective = false;
        if (obj < QUEST_OBJECTIVES_COUNT)
        {
            int32 entry = quest->RequiredNpcOrGo[obj];
            if (entry < 0)  // GO objective
                isGatherObjective = true;
            if (entry == 0 && obj < QUEST_ITEM_OBJECTIVES_COUNT && quest->RequiredItemId[obj])
                isGatherObjective = true;
        }
        else if (obj < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            isGatherObjective = true;
        }
        // source-item quest: need to find the target to use it on
        if (quest->GetSrcItemId())
            isGatherObjective = true;

        if (isGatherObjective)
        {
            // Item objectives sourced from mob drops are kill quests in
            // disguise (venom sacs, spider legs): when a mob that can
            // drop the item is nearby, yield the tick so the grind
            // action picks it up. Under per-tick movement resolution
            // MoveRandomNear succeeds almost every tick, so without
            // this yield the bot roams the camp forever, surrounded by
            // the very mobs it needs to kill.
            if (HasNearbyQuestMob(30.0f))
                return false;

            // If the source mobs are in SIGHT but farther out, head to
            // the nearest one instead of camping the spot for local
            // respawns. Travel gets us within grind range, then the
            // yield above hands off to combat.
            if (Creature* mob = NearestQuestMob(sPlayerbotAIConfig.sightDistance))
                return MoveWorldObjectTo(mob->GetGUID());

            // Nothing to kill or gather in sight: roam briefly, but once
            // this cluster has been dry for the scout window, fall
            // THROUGH to the POI-rotation scout below and move to
            // another cluster (other Grellkin camps) instead of camping
            // respawns here forever.
            if (!data.lastReachPOI || GetMSTimeDiffToNow(data.lastReachPOI) < scoutTimeoutMs)
                return MoveRandomNear(20.0f);
        }
    }

    // POI scout (kill AND dried-up gather): at POI for 30s+ with no
    // quest mob in sight means this cluster is empty. Switch to a
    // different POI candidate (>50y away) if one exists; otherwise roam
    // in place.
    if (data.lastReachPOI && GetMSTimeDiffToNow(data.lastReachPOI) >= scoutTimeoutMs &&
        !HasNearbyQuestMob(30.0f))
    {
        std::vector<POIInfo> poiInfo;
        if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            std::vector<size_t> alternatives;
            for (size_t i = 0; i < poiInfo.size(); ++i)
            {
                float dx = poiInfo[i].pos.x - data.pos.GetPositionX();
                float dy = poiInfo[i].pos.y - data.pos.GetPositionY();
                if (dx * dx + dy * dy > 50.0f * 50.0f)
                    alternatives.push_back(i);
            }
            if (!alternatives.empty())
            {
                size_t pickIdx = alternatives[urand(0, alternatives.size() - 1)];
                G3D::Vector2 newPoi = poiInfo[pickIdx].pos;
                float dz = std::max(bot->GetMap()->GetHeight(newPoi.x, newPoi.y, MAX_HEIGHT),
                                    bot->GetMap()->GetWaterLevel(newPoi.x, newPoi.y));
                if (dz != INVALID_HEIGHT && dz != VMAP_INVALID_HEIGHT_VALUE)
                {
                    data.pos = WorldPosition(bot->GetMapId(), newPoi.x, newPoi.y, dz);
                    data.objectiveIdx = poiInfo[pickIdx].objectiveIdx;
                    data.lastReachPOI = 0;
                    data.pursuedLootGO.Clear();
                    data.pursuedUseGO.Clear();
                    data.pursuedUseTarget.Clear();
                    return true;
                }
            }
        }
        return MoveRandomNear(20.0f);
    }

    // kill quest: walk toward the marker before handing off to grind.
    // lastReachPOI trips at ~10y so without this the bot fights on the
    // edge and never reaches the dense cluster. Skip if a quest mob is
    // in sight (might be the target) or a hostile is mid-pull.
    if (bot->GetDistance(data.pos) > 5.0f)
    {
        if (HasNearbyQuestMob(30.0f))
            return false;

        GuidVector nearby = AI_VALUE(GuidVector, "possible targets");
        bool hostileClose = false;
        for (ObjectGuid guid : nearby)
        {
            Unit* u = botAI->GetUnit(guid);
            if (u && u->IsAlive() && bot->GetDistance(u) < 15.0f)
            {
                hostileClose = true;
                break;
            }
        }
        if (!hostileClose)
            return MoveFarTo(data.pos);
    }

    // yield to grind
    return false;
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    const Quest* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->rpgStatistic.questCompleted++;
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        // The top-down probe lands on the terrain; quest enders on
        // treehouse platforms or towers sit well above it. Refine the
        // height from the ender's actual spawn so the walk targets the
        // platform (and routes up its ramp) instead of the ground
        // below it.
        dz = ResolveQuestTurnInDestZ(questId, dx, dy, dz);

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;

        // Drop the spline + lastPath that DoIncompleteQuest committed
        // to the now-completed objective. Without this, MoveFarTo on
        // the next tick hits the bot->isMoving() / lastPath-reuse
        // early-exits at the top of MoveFarTo and rides the stale
        // path instead of replanning toward the turn-in POI. (This
        // is what `.playerbot bot self` masks by recreating the AI.)
        bot->GetMotionMaster()->Clear();
        AI_VALUE(LastMovement&, "last movement").clear();
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    // waiting for SearchQuestGiverAndAcceptOrReward to pick up the NPC;
    // wander instead of false so we don't fall through to grind
    return MoveRandomNear(15.0f);
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;

    // Arrival: we had boarded a flight (data.inFlight) and we're no longer in
    // it → we just landed. Special-case Rut'theran: walk to the portal GO so
    // it teleports the bot into Darnassus, flipping the zone to AREA_DARNASSUS
    // so this branch falls through to ChangeToIdle on the next tick.
    if (data.inFlight && !bot->IsInFlight())
    {
        if (bot->GetZoneId() == AREA_TELDRASSIL)
        {
            static WorldPosition const rutTheranPortalEntrance(1, 8799.41f, 969.787f, 26.2409f, 0.0f);
            return MoveFarTo(rutTheranPortalEntrance);
        }
        info.ChangeToIdle();
        return true;
    }

    if (bot->IsInFlight())
    {
        data.inFlight = true;
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }

    if (!TakeFlight(data.path, flightMaster))
    {
        info.ChangeToIdle();
        return true;
    }
    return true;
}
