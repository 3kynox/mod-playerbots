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
#include "PlayerbotsCluster.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "TC9Sidecar.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr char CLUSTER_LOGIN_REQUEST_SUBJECT[] = "playerbots.login-request";
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
}

namespace PlayerbotsCluster
{
    bool PoolFilterActive()
    {
        return sToCloud9Sidecar->ClusterModeEnabled();
    }

    bool ShouldSkipPoolCandidate(uint32 mapId)
    {
        return sToCloud9Sidecar->ClusterModeEnabled()
            && IsMapServedByClusterBots(mapId)
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
            clusterSubscribed = sToCloud9Sidecar->NatsSubscribe(CLUSTER_LOGIN_REQUEST_SUBJECT, &OnClusterLoginRequest);

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

void AddPlayerbotsClusterScripts()
{
    new PlayerbotsClusterPlayerScript();
    new PlayerbotsClusterWorldScript();
}
