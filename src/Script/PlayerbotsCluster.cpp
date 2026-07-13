/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// ToCloud9 cluster partition enforcement for random bots. Each worldserver
// only owns the maps assigned to it by the servers registry; a random bot
// that ends up on a foreign map (portal, boat, zeppelin...) is logged out
// here and handed off through NATS (playerbots.login-request) to the
// worldserver that owns the map — or re-randomized onto an owned map when
// no playerbots-enabled worldserver serves the destination
// (AiPlayerbot.ClusterBotMaps).

#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsCluster.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "TC9Sidecar.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr char CLUSTER_LOGIN_REQUEST_SUBJECT[] = "playerbots.login-request";
    constexpr char GROUP_INVITE_CREATED_SUBJECT[] = "group.invite.created";
    constexpr uint32 CLUSTER_KICK_COOLDOWN_MS = 30 * 1000;  // per bot, breaks kick<->handoff loops
    constexpr uint32 CLUSTER_LOGIN_DELAY_MS = 2 * 1000;     // lets the sender's logout save reach the DB

    // Fed from map-update worker threads, drained on the world thread.
    std::mutex clusterPendingMutex;
    std::vector<ObjectGuid> clusterPendingKicks;

    // World thread only.
    std::unordered_map<ObjectGuid::LowType, int32> clusterKickCooldowns;
    struct ClusterPendingLogin
    {
        ObjectGuid::LowType guid;
        uint32 mapId;
        int32 delay;
    };
    std::vector<ClusterPendingLogin> clusterPendingLogins;
    bool clusterSubscribed = false;

    bool IsMapServedByClusterBots(uint32 mapId)
    {
        std::vector<uint32> const& maps = sPlayerbotAIConfig.clusterBotMaps;
        return maps.empty() || std::find(maps.begin(), maps.end(), mapId) != maps.end();
    }

    // Runs on the world thread (delivered through ProcessHooks).
    void OnClusterLoginRequest(char const* /*subject*/, char const* payload, int payloadLen)
    {
        uint32 guidLow = 0;
        uint32 mapId = 0;
        std::string data(payload, payloadLen);
        if (sscanf(data.c_str(), "{\"g\":%u,\"m\":%u}", &guidLow, &mapId) != 2)
            return;

        if (!sPlayerbotAIConfig.enabled || !sToCloud9Sidecar->IsMapAssigned(mapId))
            return;

        for (ClusterPendingLogin const& pending : clusterPendingLogins)
            if (pending.guid == guidLow)
                return;

        clusterPendingLogins.push_back({guidLow, mapId, int32(CLUSTER_LOGIN_DELAY_MS)});
    }

    // Minimal extractor for one numeric field of the groupserver JSON events
    // (envelope {"v":...,"t":...,"p":{...}} built by EventToSendGenericPayload).
    bool ExtractJsonUInt64(std::string const& json, char const* key, uint64& out)
    {
        std::string needle = std::string("\"") + key + "\":";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return false;

        return sscanf(json.c_str() + pos + needle.size(), "%llu", (unsigned long long*)&out) == 1;
    }

    // Runs on the world thread (delivered through ProcessHooks). The group
    // service created an invite for a character without gateway session; if
    // that character is one of our local random bots, accept on its behalf
    // (BUG-022: the SMSG_GROUP_INVITE only reaches gateway sessions).
    void OnClusterGroupInviteCreated(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!sPlayerbotAIConfig.enabled)
            return;

        std::string data(payload, payloadLen);
        uint64 inviteeGUID = 0;
        if (!ExtractJsonUInt64(data, "InviteeGUID", inviteeGUID) || !inviteeGUID)
            return;

        Player* bot = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(inviteeGUID)));
        if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot())
            return;  // not one of our in-process sessions (real players use the gateway)

        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            // Alt bots mirror vanilla AcceptInvitationAction: only their owner
            // may pull them into a group (cluster stand-in for the
            // PLAYERBOT_SECURITY_INVITE check — the inviter can be cross-shard,
            // so compare GUIDs instead of resolving a local Player).
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            uint64 inviterGUID = 0;
            if (!botAI || !botAI->GetMaster() ||
                !ExtractJsonUInt64(data, "InviterGUID", inviterGUID) ||
                botAI->GetMaster()->GetGUID().GetCounter() != ObjectGuid::LowType(inviterGUID))
                return;
        }

        if (bot->GetGroup())
            return;  // already grouped (local mirror fed by group.* events)

        LOG_INFO("playerbots", "Cluster: accepting group invite for bot {}", bot->GetName());

        // Blocking gRPC call: keep it off the world thread. Group state comes
        // back asynchronously through the group.created/member.added events.
        uint64 guidCounter = bot->GetGUID().GetCounter();
        std::thread([guidCounter]() {
            sToCloud9Sidecar->GroupAcceptInvite(guidCounter);
        }).detach();
    }
}

namespace PlayerbotsCluster
{
    bool PoolFilterActive()
    {
        return sToCloud9Sidecar->ClusterModeEnabled();
    }

    bool ShouldSkipPoolCandidate(uint32 mapId)
    {
        // Strict partition: only pick characters saved on maps this
        // worldserver owns. Characters on maps served by nobody (e.g.
        // blood elf/draenei starters while no shard owns map 530) stay
        // benched until a shard owns their map: re-randomizing them is
        // unreliable (RandomTeleportForLevel has no valid location for
        // some level/race combos and can land outside randomBotMaps,
        // observed as kalimdor bots leaking to map 0).
        return sToCloud9Sidecar->ClusterModeEnabled()
            && !sToCloud9Sidecar->IsMapAssigned(mapId);
    }
}

class PlayerbotsClusterPlayerScript : public PlayerScript
{
public:
    PlayerbotsClusterPlayerScript() : PlayerScript("PlayerbotsClusterPlayerScript", {
        PLAYERHOOK_ON_UPDATE_ZONE
    }) {}

    // May run on map-update worker threads: only collect, never act here.
    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        if (!sToCloud9Sidecar->ClusterModeEnabled())
            return;

        if (!player->GetSession() || !player->GetSession()->IsBot())
            return;

        if (sToCloud9Sidecar->IsMapAssigned(player->GetMapId()))
            return;

        std::lock_guard<std::mutex> lock(clusterPendingMutex);
        if (std::find(clusterPendingKicks.begin(), clusterPendingKicks.end(), player->GetGUID()) ==
            clusterPendingKicks.end())
            clusterPendingKicks.push_back(player->GetGUID());
    }
};

class PlayerbotsClusterWorldScript : public WorldScript
{
public:
    PlayerbotsClusterWorldScript() : WorldScript("PlayerbotsClusterWorldScript", {
        WORLDHOOK_ON_UPDATE
    }) {}

    void OnUpdate(uint32 diff) override
    {
        if (!sToCloud9Sidecar->ClusterModeEnabled())
            return;

        if (!clusterSubscribed && sPlayerbotAIConfig.enabled)
            clusterSubscribed =
                sToCloud9Sidecar->NatsSubscribe(CLUSTER_LOGIN_REQUEST_SUBJECT, &OnClusterLoginRequest) &&
                sToCloud9Sidecar->NatsSubscribe(GROUP_INVITE_CREATED_SUBJECT, &OnClusterGroupInviteCreated);

        UpdateCooldowns(diff);
        ProcessPendingKicks();
        ProcessPendingLogins(diff);
    }

private:
    void UpdateCooldowns(uint32 diff)
    {
        for (auto itr = clusterKickCooldowns.begin(); itr != clusterKickCooldowns.end();)
        {
            itr->second -= int32(diff);
            if (itr->second <= 0)
                itr = clusterKickCooldowns.erase(itr);
            else
                ++itr;
        }
    }

    void ProcessPendingKicks()
    {
        std::vector<ObjectGuid> kicks;
        {
            std::lock_guard<std::mutex> lock(clusterPendingMutex);
            kicks.swap(clusterPendingKicks);
        }

        for (ObjectGuid const& guid : kicks)
        {
            if (clusterKickCooldowns.count(guid.GetCounter()))
                continue;

            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot())
                continue;

            uint32 mapId = bot->GetMapId();
            if (sToCloud9Sidecar->IsMapAssigned(mapId))
                continue;  // maps got reassigned meanwhile

            if (!sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;  // alt bots follow their master, not the partition

            clusterKickCooldowns[guid.GetCounter()] = int32(CLUSTER_KICK_COOLDOWN_MS);

            if (IsMapServedByClusterBots(mapId))
            {
                // Logout first so the receiving worldserver loads the
                // position saved on the destination map.
                LOG_INFO("playerbots", "Cluster: handing off bot {} on foreign map {}", bot->GetName(), mapId);
                sRandomPlayerbotMgr.LogoutPlayerBot(guid);

                char payload[64];
                int len = snprintf(payload, sizeof(payload), "{\"g\":%u,\"m\":%u}", guid.GetCounter(), mapId);
                sToCloud9Sidecar->NatsPublish(CLUSTER_LOGIN_REQUEST_SUBJECT, std::string(payload, len));
            }
            else
            {
                // No playerbots-enabled worldserver serves this map:
                // bring the bot back onto a map this worldserver owns.
                LOG_INFO("playerbots", "Cluster: re-randomizing bot {} from unserved map {}", bot->GetName(), mapId);
                sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
                // Persist the new position now: the pool can log the bot out
                // before the next periodic save, which would leave the stale
                // unserved map in the DB and re-trigger this path forever.
                // (During a far teleport this schedules DELAYED_SAVE_PLAYER,
                // executed on arrival with the destination map.)
                bot->SaveToDB(false, false);
            }
        }
    }

    void ProcessPendingLogins(uint32 diff)
    {
        for (auto itr = clusterPendingLogins.begin(); itr != clusterPendingLogins.end();)
        {
            itr->delay -= int32(diff);
            if (itr->delay > 0)
            {
                ++itr;
                continue;
            }

            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(itr->guid);
            if (sToCloud9Sidecar->IsMapAssigned(itr->mapId) && !ObjectAccessor::FindPlayer(guid))
            {
                LOG_INFO("playerbots", "Cluster: logging in handed-off bot guid {} for map {}", itr->guid, itr->mapId);
                sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
            }

            itr = clusterPendingLogins.erase(itr);
        }
    }
};

// Cluster equivalent of the post-accept block in AcceptInvitationAction:
// behind the gateway, bots never receive SMSG_GROUP_INVITE, they join
// through the group service and the local mirror (Group::AddMember fires
// this hook on the world thread). Without it the bot is linked to the
// group but keeps its random strategies (grind/travel) and never follows.
class PlayerbotsClusterGroupScript : public GroupScript
{
public:
    PlayerbotsClusterGroupScript() : GroupScript("PlayerbotsClusterGroupScript", {
        GROUPHOOK_ON_ADD_MEMBER,
        GROUPHOOK_ON_REMOVE_MEMBER,
        GROUPHOOK_ON_DISBAND
    }) {}

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!sPlayerbotAIConfig.enabled || !sToCloud9Sidecar->ClusterModeEnabled())
            return;

        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot())
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;

        // Only obey a real player hosted on this shard: bot-led groups keep
        // the vanilla behavior, and a cross-shard leader has no local Player
        // to follow anyway.
        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        if (!leader || leader == bot || GET_PLAYERBOT_AI(leader))
            return;

        bool isRandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
        if (!isRandomBot && botAI->GetMaster() != leader)
            return;  // alt bots only follow their owner

        LOG_INFO("playerbots", "Cluster: bot {} grouped with {}, switching to follow", bot->GetName(), leader->GetName());

        // Alt bots keep their owner as master (vanilla AcceptInvitationAction
        // only rebinds the master for random bots).
        if (isRandomBot)
            botAI->SetMaster(leader);
        botAI->ResetStrategies();
        botAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
        botAI->Reset();
        botAI->TellMaster("Hello");
    }

    // A client "disband" is a Leave of the player (WoW semantics): the group
    // survives while >= 2 bots remain, stranding the alts grouped together
    // and unable to be re-invited (BUG-027). When the removed member is the
    // owner of local alt bots still in the group, make each alt leave through
    // the group service; their own removal (or the final disband) then puts
    // them back on follow.
    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod /*method*/,
                        ObjectGuid /*kicker*/, char const* /*reason*/) override
    {
        if (!sPlayerbotAIConfig.enabled || !sToCloud9Sidecar->ClusterModeEnabled())
            return;

        // The removed member is one of our local alt bots (owner uninvite or
        // our own GroupLeave below): resume following its owner.
        if (Player* bot = ObjectAccessor::FindPlayer(guid))
        {
            if (bot->GetSession() && bot->GetSession()->IsBot() && !sRandomPlayerbotMgr.IsRandomBot(bot))
            {
                ResumeFollowingOwner(bot);
                return;
            }
        }
        else
            // BUG-030: one alt missed its release with no trace; log the only
            // path where the removed member itself is unreachable.
            LOG_INFO("playerbots", "Cluster: removed member {} of group {} not found locally",
                     guid.GetCounter(), group->GetGUID().GetCounter());

        std::vector<uint64> altsToLeave;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            if (slot.guid == guid)
                continue;

            Player* bot = ObjectAccessor::FindPlayer(slot.guid);
            if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot() ||
                sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || !botAI->GetMaster() || botAI->GetMaster()->GetGUID() != guid)
                continue;

            LOG_INFO("playerbots", "Cluster: owner left group {}, alt bot {} leaves too",
                     group->GetGUID().GetCounter(), bot->GetName());

            altsToLeave.push_back(bot->GetGUID().GetCounter());
        }

        if (altsToLeave.empty())
            return;

        // Blocking gRPC calls: keep them off the world thread, and SEQUENTIAL
        // on purpose - concurrent Leave calls race in the group service
        // (double disband panic, lost member removals). The last leave can
        // come back "group not found" once the service disbands the group
        // under 2 members; that's expected.
        std::thread([altsToLeave]() {
            for (uint64 altGuid : altsToLeave)
                sToCloud9Sidecar->GroupLeave(altGuid);
        }).detach();
    }

    // Catch-all: the group service disbands the group once it falls under 2
    // members; free every local alt bot still linked to it.
    void OnDisband(Group* group) override
    {
        if (!sPlayerbotAIConfig.enabled || !sToCloud9Sidecar->ClusterModeEnabled())
            return;

        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            Player* bot = ObjectAccessor::FindPlayer(slot.guid);
            if (bot && bot->GetSession() && bot->GetSession()->IsBot() &&
                !sRandomPlayerbotMgr.IsRandomBot(bot))
                ResumeFollowingOwner(bot);
        }
    }

private:
    static void ResumeFollowingOwner(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            LOG_WARN("playerbots", "Cluster: alt bot {} freed from group but has no AI", bot->GetName());
            return;
        }

        if (!botAI->GetMaster())
        {
            // BUG-030: without this the alt stays stuck in group strategies.
            // Reset anyway; the next invite rebinds the follow.
            LOG_WARN("playerbots", "Cluster: alt bot {} freed from group but has no master, resetting strategies only",
                     bot->GetName());
            botAI->ResetStrategies();
            botAI->Reset();
            return;
        }

        LOG_INFO("playerbots", "Cluster: alt bot {} freed from group, following {} again",
                 bot->GetName(), botAI->GetMaster()->GetName());

        botAI->ResetStrategies();
        botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
        botAI->Reset();
    }
};

void AddPlayerbotsClusterScripts()
{
    new PlayerbotsClusterPlayerScript();
    new PlayerbotsClusterWorldScript();
    new PlayerbotsClusterGroupScript();
}
