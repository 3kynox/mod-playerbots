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

#include "BattleGroundJoinAction.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsCluster.h"
#include "PositionValue.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "TC9Sidecar.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr char CLUSTER_LOGIN_REQUEST_SUBJECT[] = "playerbots.login-request";
    constexpr char GROUP_INVITE_CREATED_SUBJECT[] = "group.invite.created";
    constexpr char GUILD_INVITE_CREATED_SUBJECT[] = "guild.invite.created";
    constexpr char GROUP_MESSAGE_NEW_SUBJECT[] = "group.message.new";
    // The gateway swallows whisper and guild chat: it answers the chat service
    // itself and never forwards the packet to the worldserver, so the vanilla
    // OnPlayerCanUseChat hooks never fire and in-process bots stay deaf
    // (BUG-TC9-057). These two subjects re-inject the message on the world side.
    // The whisper subject is published per gateway, hence the wildcard token.
    constexpr char WHISPER_INCOME_SUBJECT[] = "chat.gw.*.income.whisper";
    constexpr char GUILD_MESSAGE_NEW_SUBJECT[] = "guild.message.new";
    constexpr char MATCHMAKING_INVITED_SUBJECT[] = "matchmaking.pvpqueue.invited";
    constexpr char MATCHMAKING_QUEUED_SUBJECT[] = "matchmaking.pvpqueue.joined";
    constexpr char MATCHMAKING_EXPIRED_SUBJECT[] = "matchmaking.pvpqueue.invite.expired";
    constexpr uint32 CLUSTER_KICK_COOLDOWN_MS = 30 * 1000;  // per bot, breaks kick<->handoff loops
    constexpr uint32 CLUSTER_LOGIN_DELAY_MS = 2 * 1000;     // lets the sender's logout save reach the DB
    constexpr int32 CLUSTER_BG_JOIN_RETRY_MS = 500;         // local BG instance can lag behind the invite
    constexpr int32 CLUSTER_BG_JOIN_ATTEMPTS = 20;
    // Safety net only: matchmaking owns the queue timers, the map is cleared
    // on invite/expire events; the TTL just unblocks bots after lost events.
    constexpr int32 CLUSTER_BG_QUEUE_TTL_MS = 10 * 60 * 1000;

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

    // Fed from the sidecar-query threads, drained on the world thread.
    struct ClusterPendingBGJoin
    {
        ObjectGuid::LowType guid;
        uint32 bgTypeId;
        uint32 instanceId;
        int32 attemptsLeft;
        int32 delay;
        // Backfill joins (C-BG.5) bypass the matchmaking queue: the service
        // never invited the bot, so a joined confirmation would be rejected.
        bool notifyMatchmaking = true;
    };
    std::mutex clusterPendingBGMutex;
    std::vector<ClusterPendingBGJoin> clusterPendingBGJoins;

    // World thread only. Random bots we enqueued into the matchmaking BG
    // queues (C-BG.3), keyed by guid. typeId/minLvl identify the queue so a
    // later fill subtracts the bots already sitting in it (§ F.2: they were
    // double-booked, observed as 6v5).
    struct ClusterQueuedBot
    {
        int32 ttl;
        uint32 typeId;
        uint32 minLvl;
    };
    std::unordered_map<ObjectGuid::LowType, ClusterQueuedBot> clusterBGQueuedBots;

    // World thread only. Humans waiting per matchmaking queue (bg type +
    // bracket): the queue is re-filled with bots on a timer while they wait,
    // so a queue left short (declined invite, leaver) still pops (§ F.3 —
    // the fill used to run only on the enqueue event). The wait TTL covers
    // the missing "player left the queue" event.
    struct ClusterBGWait
    {
        uint32 typeId;
        uint32 minLvl;
        uint32 maxLvl;
        std::vector<ObjectGuid::LowType> humans;
        int32 refillIn;
        int32 ttl;
        // A human that leaves the queue emits no event: without a cap, its
        // phantom wait entry would re-fill (and pop) full battlegrounds
        // every 30s until the TTL. Each real enqueue re-arms the budget.
        int32 refillsLeft;
    };
    std::vector<ClusterBGWait> clusterBGWaits;
    constexpr int32 CLUSTER_BG_REFILL_MS = 30 * 1000;
    constexpr int32 CLUSTER_BG_REFILL_BUDGET = 4;

    // World thread only. Local battleground instances that contain our bots
    // (fed by the force-join/backfill path): the ejection scan only makes
    // sense there, and no public BattlegroundMgr API iterates instances.
    std::unordered_map<uint32 /*instanceId*/, uint32 /*typeId*/> clusterBGBotInstances;

    // World thread only. Slots freed on purpose for a waiting human (§ F.4):
    // C-BG.5 must not refill them while the matchmaking invites the human.
    struct ClusterBGEjectHold
    {
        uint32 instanceId;
        uint8 team;
        int32 ttl;
    };
    std::vector<ClusterBGEjectHold> clusterBGEjectHolds;
    constexpr int32 CLUSTER_BG_EJECT_HOLD_MS = 45 * 1000;

    // World thread only. Bots being ejected on purpose: lets the teleport
    // veto (OnPlayerBeforeTeleport) wave their leave through.
    std::unordered_set<ObjectGuid::LowType> clusterBGEjecting;

    void RemoveWaitingHuman(ObjectGuid::LowType guidLow);
    void FillBGQueue(uint32 typeId, uint32 minLvl, uint32 maxLvl);

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

    // String variant. Handles the escapes Go's json.Marshal emits inside chat
    // text; exotic escapes (\uXXXX) are kept raw — chat commands are plain
    // words, anything else was never a command to begin with.
    bool ExtractJsonString(std::string const& json, char const* key, std::string& out)
    {
        std::string needle = std::string("\"") + key + "\":\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return false;

        out.clear();
        for (size_t i = pos + needle.size(); i < json.size(); ++i)
        {
            char c = json[i];
            if (c == '"')
                return true;

            if (c == '\\' && i + 1 < json.size())
            {
                char next = json[++i];
                if (next == '"' || next == '\\' || next == '/')
                    out += next;
                else
                {
                    out += '\\';
                    out += next;
                }
                continue;
            }

            out += c;
        }

        return false;  // unterminated string
    }

    // Array-of-numbers variant ("Receivers":[1,2,...]).
    bool ExtractJsonUInt64Array(std::string const& json, char const* key, std::vector<uint64>& out)
    {
        std::string needle = std::string("\"") + key + "\":[";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return false;

        char const* cursor = json.c_str() + pos + needle.size();
        while (*cursor && *cursor != ']')
        {
            unsigned long long value = 0;
            int consumed = 0;
            if (sscanf(cursor, "%llu%n", &value, &consumed) != 1)
                return false;

            out.push_back(uint64(value));
            cursor += consumed;
            if (*cursor == ',')
                ++cursor;
        }

        return *cursor == ']';
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

    // Runs on the world thread (delivered through ProcessHooks). The guild
    // service created an invite for a character without gateway session; if
    // that character is one of our local random bots, accept on its behalf.
    // Mirror of OnClusterGroupInviteCreated (SMSG_GUILD_INVITE only reaches
    // gateway sessions, so the vanilla "guild invite"->"guild accept" trigger
    // never fires for in-process bots).
    void OnClusterGuildInviteCreated(char const* /*subject*/, char const* payload, int payloadLen)
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
            // Alt bots: only their owner may pull them into a guild (cluster
            // stand-in for the security check; the inviter can be cross-shard,
            // so compare GUIDs instead of resolving a local Player).
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            uint64 inviterGUID = 0;
            if (!botAI || !botAI->GetMaster() ||
                !ExtractJsonUInt64(data, "InviterGUID", inviterGUID) ||
                botAI->GetMaster()->GetGUID().GetCounter() != ObjectGuid::LowType(inviterGUID))
                return;
        }

        if (bot->GetGuildId())
            return;  // already in a guild (local mirror fed by guild.* events)

        LOG_INFO("playerbots", "Cluster: accepting guild invite for bot {}", bot->GetName());

        // Snapshot the bot's character on the world thread; the guild service's
        // InviteAccepted RPC needs the full member row. Blocking gRPC call runs
        // off-thread; the guild membership comes back via guild.member.added.
        uint64 guidCounter = bot->GetGUID().GetCounter();
        std::string name = bot->GetName();
        uint32 lvl = bot->GetLevel();
        uint32 race = bot->getRace();
        uint32 classId = bot->getClass();
        uint32 gender = bot->getGender();
        uint32 areaId = bot->GetZoneId();
        uint64 accountId = bot->GetSession()->GetAccountId();
        std::thread([=]() {
            sToCloud9Sidecar->GuildAcceptInvite(guidCounter, name, lvl, race, classId, gender, areaId, accountId);
        }).detach();
    }

    // Runs on the world thread (delivered through ProcessHooks). Behind the
    // gateway, group/raid chat is served by the group service: the sender's
    // CMSG_MESSAGECHAT never reaches this worldserver, so the vanilla
    // OnPlayerCanUseChat(Group*) hook — the entry point for every chat
    // command (follow, stay, summon...) — never fires for in-process bots.
    // Feed the receivers that are local bots from the NATS mirror instead.
    void OnClusterGroupChatMessage(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!sPlayerbotAIConfig.enabled)
            return;

        std::string data(payload, payloadLen);
        uint64 senderGUID = 0;
        if (!ExtractJsonUInt64(data, "SenderGUID", senderGUID) || !senderGUID)
            return;

        // HandleCommand needs the sender as a local Player. Alt bots live on
        // their owner's shard so the interesting sender always resolves; a
        // cross-shard sender has no local Player to command through.
        Player* sender = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(senderGUID)));
        if (!sender)
            return;

        if (sender->GetSession() && sender->GetSession()->IsBot())
            return;  // bot chatter already goes through the vanilla local hook

        uint64 messageType = 0;
        std::string msg;
        std::vector<uint64> receivers;
        if (!ExtractJsonUInt64(data, "MessageType", messageType) ||
            !ExtractJsonString(data, "Msg", msg) || msg.empty() ||
            !ExtractJsonUInt64Array(data, "Receivers", receivers))
            return;

        for (uint64 guid : receivers)
        {
            if (guid == senderGUID)
                continue;

            Player* member = ObjectAccessor::FindPlayer(
                ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(guid)));
            if (!member || !member->GetSession() || !member->GetSession()->IsBot())
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(member))
                botAI->HandleCommand(uint32(messageType), msg, sender);
        }
    }

    // Whisper re-injection. Unlike the group hook we must NOT filter on
    // session->IsBot(): a selfbot (".playerbot bot self") is a real player who
    // happens to carry a PlayerbotAI, and it is the only channel it can be
    // commanded through. The vanilla hook's own condition is simply "does the
    // receiver have an AI", so we mirror it.
    // We only feed the AI here; delivering the text to a human client stays the
    // gateway's job, so there is no double delivery.
    void OnClusterWhisper(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!sPlayerbotAIConfig.enabled)
            return;

        std::string data(payload, payloadLen);
        uint64 senderGUID = 0;
        uint64 receiverGUID = 0;
        std::string msg;
        if (!ExtractJsonUInt64(data, "SenderGUID", senderGUID) || !senderGUID ||
            !ExtractJsonUInt64(data, "ReceiverGUID", receiverGUID) || !receiverGUID ||
            !ExtractJsonString(data, "Msg", msg) || msg.empty())
            return;

        // HandleCommand needs the sender as a local Player; a cross-shard
        // sender cannot be resolved (same limitation as the group hook).
        Player* sender = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(senderGUID)));
        if (!sender)
            return;

        if (sender->GetSession() && sender->GetSession()->IsBot())
            return;  // bot chatter already goes through the vanilla local hook

        Player* receiver = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(receiverGUID)));
        if (!receiver)
            return;

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(receiver))
            botAI->HandleCommand(CHAT_MSG_WHISPER, msg, sender);
    }

    // Guild chat re-injection. Vanilla semantics are deliberately preserved:
    // only the sender's OWN bots react, never every bot of the guild — our
    // main guild holds several hundred of them and a single line would
    // otherwise command them all.
    void OnClusterGuildChatMessage(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!sPlayerbotAIConfig.enabled)
            return;

        std::string data(payload, payloadLen);
        uint64 senderGUID = 0;
        std::string msg;
        if (!ExtractJsonUInt64(data, "SenderGUID", senderGUID) || !senderGUID ||
            !ExtractJsonString(data, "Msg", msg) || msg.empty())
            return;

        Player* sender = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(senderGUID)));
        if (!sender)
            return;

        if (sender->GetSession() && sender->GetSession()->IsBot())
            return;

        PlayerbotMgr* playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(sender);
        if (!playerbotMgr)
            return;

        for (PlayerBotMap::const_iterator it = playerbotMgr->GetPlayerBotsBegin();
             it != playerbotMgr->GetPlayerBotsEnd(); ++it)
        {
            Player* const bot = it->second;
            if (!bot || bot->GetGuildId() != sender->GetGuildId())
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                botAI->HandleCommand(CHAT_MSG_GUILD, msg, sender);
        }
    }

    // Runs on the world thread (delivered through ProcessHooks). The
    // matchmaking service invited players to a battleground; an in-process
    // bot has no gateway session, so nobody accepts the invite nor performs
    // the enterBattleground sequence (queue data -> AddPlayers -> joined
    // confirmation) on its behalf (chantier C-BG, DESIGN-bots-bg-cluster.md).
    void OnClusterBGInvite(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!sPlayerbotAIConfig.enabled)
            return;

        std::string data(payload, payloadLen);
        std::vector<uint64> invited;
        if (!ExtractJsonUInt64Array(data, "PlayersGUID", invited))
            return;

        for (uint64 guidRaw : invited)
        {
            RemoveWaitingHuman(ObjectGuid::LowType(guidRaw));

            Player* bot = ObjectAccessor::FindPlayer(
                ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(guidRaw)));
            if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot())
                continue;

            clusterBGQueuedBots.erase(bot->GetGUID().GetCounter());

            if (sRandomPlayerbotMgr.IsRandomBot(bot) && !sPlayerbotAIConfig.randomBotJoinBG)
                continue;  // random bots opt into BG queues through RandomBotJoinBG (C-BG.3)

            LOG_INFO("playerbots", "Cluster: bot {} invited to BG, querying assignment", bot->GetName());

            // Blocking gRPC calls: keep them off the world thread. The join
            // itself needs the world thread, so it comes back through
            // clusterPendingBGJoins.
            uint64 guidCounter = bot->GetGUID().GetCounter();
            std::thread([guidCounter]() {
                uint32 bgTypeId = 0;
                uint32 instanceId = 0;
                uint32 mapId = 0;
                bool isLocal = false;
                if (!sToCloud9Sidecar->BattlegroundQueueDataForLocalPlayer(
                        guidCounter, bgTypeId, instanceId, mapId, isLocal))
                {
                    LOG_INFO("playerbots", "Cluster: no BG assignment for invited bot guid {}", guidCounter);
                    return;
                }

                if (!isLocal)
                {
                    // C-BG.2 (cross-shard: pending BG in the login-request)
                    // not implemented: let the invite expire.
                    LOG_INFO("playerbots", "Cluster: bot guid {} BG instance {} runs on another shard, skipping",
                             guidCounter, instanceId);
                    return;
                }

                std::lock_guard<std::mutex> lock(clusterPendingBGMutex);
                clusterPendingBGJoins.push_back({ObjectGuid::LowType(guidCounter), bgTypeId, instanceId,
                                                 CLUSTER_BG_JOIN_ATTEMPTS, CLUSTER_BG_JOIN_RETRY_MS});
            }).detach();
        }
    }

    // World thread. A human is no longer waiting in a queue (invited, or
    // invite expired): drop it from the re-fill bookkeeping.
    void RemoveWaitingHuman(ObjectGuid::LowType guidLow)
    {
        for (auto itr = clusterBGWaits.begin(); itr != clusterBGWaits.end();)
        {
            std::vector<ObjectGuid::LowType>& humans = itr->humans;
            humans.erase(std::remove(humans.begin(), humans.end(), guidLow), humans.end());
            if (humans.empty())
                itr = clusterBGWaits.erase(itr);
            else
                ++itr;
        }
    }

    // World thread. Complete one matchmaking queue (bg type + bracket) with
    // local random bots up to the battleground's MAXIMUM per faction (§ F.1),
    // minus the humans known to be waiting and the bots we already enqueued.
    void FillBGQueue(uint32 typeId, uint32 minLvl, uint32 maxLvl)
    {
        if (!sPlayerbotAIConfig.enabled || !sPlayerbotAIConfig.randomBotJoinBG)
            return;

        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundTypeId(typeId));
        if (!bgTemplate || !sToCloud9Sidecar->IsMapAssigned(bgTemplate->GetMapId()))
            return;

        uint32 perFaction = std::max<uint32>(1, bgTemplate->GetMaxPlayersPerTeam());

        struct BGFillPick
        {
            ObjectGuid::LowType guid;
            uint32 level;
            uint32 pvpTeamId;  // matchmaking enum: 1 alliance, 2 horde
        };
        uint32 needed[2] = {perFaction, perFaction};  // TEAM_ALLIANCE, TEAM_HORDE

        // The waiting humans hold slots on their own faction. Out-of-bracket
        // group members are excluded from the count (§ F.6: a level-4 member
        // was counted in a 10-19 bracket).
        for (ClusterBGWait const& wait : clusterBGWaits)
        {
            if (wait.typeId != typeId || wait.minLvl != minLvl)
                continue;

            for (ObjectGuid::LowType guidLow : wait.humans)
            {
                uint32 level = 0;
                uint8 race = 0;
                if (!GetLevelAndRace(guidLow, level, race) || level < minLvl || level > maxLvl)
                    continue;

                TeamId team = Player::TeamIdForRace(race);
                if (team <= TEAM_HORDE && needed[team])
                    --needed[team];
            }
        }

        // The bots already sitting in this queue hold slots too (§ F.2).
        for (auto const& itr : clusterBGQueuedBots)
        {
            if (itr.second.typeId != typeId || itr.second.minLvl != minLvl)
                continue;

            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(itr.first));
            if (!bot)
                continue;

            TeamId team = bot->GetTeamId();
            if (team <= TEAM_HORDE && needed[team])
                --needed[team];
        }

        std::vector<BGFillPick> picks;

        for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld() || !bot->GetSession() || !bot->GetSession()->IsBot() ||
                !sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;

            if (bot->GetGroup() || bot->InBattleground() || bot->InBattlegroundQueue() || !bot->IsAlive())
                continue;

            uint32 level = bot->GetLevel();
            if (level < minLvl || level > maxLvl)
                continue;

            if (clusterBGQueuedBots.count(bot->GetGUID().GetCounter()))
                continue;  // already sitting in a matchmaking queue

            TeamId team = bot->GetTeamId();
            if (team > TEAM_HORDE || !needed[team])
                continue;

            --needed[team];
            picks.push_back({bot->GetGUID().GetCounter(), level, team == TEAM_HORDE ? 2u : 1u});
            if (!needed[TEAM_ALLIANCE] && !needed[TEAM_HORDE])
                break;
        }

        if (picks.empty())
            return;

        for (BGFillPick const& pick : picks)
            clusterBGQueuedBots[pick.guid] = {CLUSTER_BG_QUEUE_TTL_MS, typeId, minLvl};

        LOG_INFO("playerbots",
                 "Cluster: filling BG type {} bracket {}-{} queue with {} bots ({} alliance / {} horde still short)",
                 typeId, minLvl, maxLvl, picks.size(), needed[TEAM_ALLIANCE], needed[TEAM_HORDE]);

        // Blocking gRPC calls: keep them off the world thread, sequential to
        // spare the matchmaking service. Each enqueue also re-triggers the
        // matchmaking process() pass for this queue.
        std::thread([picks, typeId]() {
            for (BGFillPick const& pick : picks)
                if (!sToCloud9Sidecar->EnqueueLocalPlayerToBattleground(
                        pick.guid, pick.level, typeId, pick.pvpTeamId))
                    LOG_WARN("playerbots", "Cluster: BG enqueue failed for bot guid {}", pick.guid);
        }).detach();
    }

    uint32 CountEjectHolds(uint32 instanceId, uint8 team)
    {
        uint32 count = 0;
        for (ClusterBGEjectHold const& hold : clusterBGEjectHolds)
            if (hold.instanceId == instanceId && hold.team == team)
                ++count;
        return count;
    }

    // BUG-TC9-032: a character created after this worldserver booted is
    // absent from its CharacterCache until a restart — seen live: a fresh
    // character could never trigger the ejection. Fall back to the DB row
    // (sync query, but only on rare human-enqueue events).
    bool GetLevelAndRace(ObjectGuid::LowType guidLow, uint32& level, uint8& race)
    {
        if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(
                ObjectGuid::Create<HighGuid::Player>(guidLow)))
        {
            level = entry->Level;
            race = entry->Race;
            return true;
        }

        QueryResult result = CharacterDatabase.Query("SELECT level, race FROM characters WHERE guid = {}", guidLow);
        if (!result)
            return false;

        level = (*result)[0].Get<uint8>();
        race = (*result)[1].Get<uint8>();
        return true;
    }

    // § F.4: a balanced running battleground accepts nobody through the
    // matchmaking backfill — a human queuing against a match full of bots
    // would never enter it. Free one slot by making a random bot leave; the
    // matchmaking then re-runs the queue pass (PlayerLeftBattleground) and
    // seats the waiting human. True = a slot was freed or already exists.
    bool EjectBotForWaitingHuman(uint32 typeId, uint32 minLvl, TeamId team)
    {
        for (auto itr = clusterBGBotInstances.begin(); itr != clusterBGBotInstances.end();)
        {
            if (itr->second != typeId)
            {
                ++itr;
                continue;
            }

            Battleground* bg = sBattlegroundMgr->GetBattleground(itr->first, BattlegroundTypeId(typeId));
            if (!bg)
            {
                itr = clusterBGBotInstances.erase(itr);
                continue;
            }

            if (bg->GetMinLevel() != minLvl ||
                (bg->GetStatus() != STATUS_WAIT_JOIN && bg->GetStatus() != STATUS_IN_PROGRESS))
            {
                ++itr;
                continue;
            }

            TeamId other = team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
            uint32 have = bg->GetPlayersCountByTeam(team);
            uint32 target = std::min<uint32>(
                std::max<uint32>(bg->GetMinPlayersPerTeam(), bg->GetPlayersCountByTeam(other)),
                bg->GetMaxPlayersPerTeam());
            if (have + CountEjectHolds(bg->GetInstanceID(), uint8(team)) < target)
                return true;  // a backfill slot already exists, matchmaking seats the human

            for (auto const& pair : bg->GetPlayers())
            {
                Player* member = pair.second;
                if (!member || !member->GetSession() || !member->GetSession()->IsBot() ||
                    !sRandomPlayerbotMgr.IsRandomBot(member))
                    continue;

                if (member->GetTeamId(true) != team)
                    continue;

                ObjectGuid memberGuid = member->GetGUID();
                if (memberGuid == bg->GetFlagPickerGUID(TEAM_ALLIANCE) ||
                    memberGuid == bg->GetFlagPickerGUID(TEAM_HORDE))
                    continue;  // never yank a flag carrier

                PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
                if (!botAI)
                    continue;

                LOG_INFO("playerbots", "Cluster: ejecting bot {} from BG instance {} to seat a waiting player",
                         member->GetName(), bg->GetInstanceID());

                clusterBGEjectHolds.push_back({bg->GetInstanceID(), uint8(team), CLUSTER_BG_EJECT_HOLD_MS});
                clusterBGEjecting.insert(memberGuid.GetCounter());
                bool left = BGStatusAction::LeaveBG(botAI);
                clusterBGEjecting.erase(memberGuid.GetCounter());
                if (!left)
                    clusterBGEjectHolds.pop_back();

                return left;
            }

            ++itr;
        }

        return false;  // no bot-filled instance of this queue: fill path handles it
    }

    // Runs on the world thread (delivered through ProcessHooks). A player
    // entered a matchmaking BG queue; in vanilla RandomBotJoinBG makes random
    // bots fill both factions so the battleground pops, but the vanilla
    // detection reads the LOCAL AC queue, empty behind the gateway. Fill the
    // matchmaking queue instead, from the shard hosting the battleground map
    // (the only one whose bots can force-join at the pop, see C-BG.1 above).
    void OnClusterBGQueueJoined(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!sPlayerbotAIConfig.enabled || !sPlayerbotAIConfig.randomBotJoinBG)
            return;

        std::string data(payload, payloadLen);

        uint64 arenaType = 0;
        if (ExtractJsonUInt64(data, "ArenaType", arenaType) && arenaType)
            return;  // arenas are out of scope

        uint64 typeId = 0;
        uint64 minLvl = 0;
        uint64 maxLvl = 0;
        std::vector<uint64> queued;
        if (!ExtractJsonUInt64(data, "TypeID", typeId) ||
            !ExtractJsonUInt64(data, "PVPQueueMinLVL", minLvl) ||
            !ExtractJsonUInt64(data, "PVPQueueMaxLVL", maxLvl) ||
            !ExtractJsonUInt64Array(data, "PlayersGUID", queued) || queued.empty())
            return;

        // Only the shard hosting this battleground's map fills, ejects and
        // tracks waits — the others were burning re-fill passes on phantom
        // "humans" (cross-shard bots they cannot resolve).
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundTypeId(typeId));
        if (!bgTemplate || !sToCloud9Sidecar->IsMapAssigned(bgTemplate->GetMapId()))
            return;

        // Our own fills echo back through this event: an enqueue where every
        // player is a local bot must not trigger another fill. A human
        // grouping with alt bots still counts as a real enqueue (skipping it
        // left the opposite faction unfilled: observed as a 10v5 join).
        bool allLocalBots = true;
        for (uint64 guidRaw : queued)
        {
            Player* who = ObjectAccessor::FindPlayer(
                ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(guidRaw)));
            if (!who || !who->GetSession() || !who->GetSession()->IsBot())
            {
                allLocalBots = false;
                break;
            }
        }
        if (allLocalBots)
            return;

        // Track the waiting humans of this queue and (re)arm its refill
        // timer, then fill it (§ F.3: the fill used to run only here, so a
        // queue left short by a declined invite stayed stuck forever).
        ClusterBGWait* wait = nullptr;
        for (ClusterBGWait& existing : clusterBGWaits)
            if (existing.typeId == uint32(typeId) && existing.minLvl == uint32(minLvl))
                wait = &existing;
        if (!wait)
        {
            clusterBGWaits.push_back({uint32(typeId), uint32(minLvl), uint32(maxLvl), {},
                                      CLUSTER_BG_REFILL_MS, CLUSTER_BG_QUEUE_TTL_MS,
                                      CLUSTER_BG_REFILL_BUDGET});
            wait = &clusterBGWaits.back();
        }
        wait->refillIn = CLUSTER_BG_REFILL_MS;
        wait->ttl = CLUSTER_BG_QUEUE_TTL_MS;
        wait->refillsLeft = CLUSTER_BG_REFILL_BUDGET;

        for (uint64 guidRaw : queued)
        {
            ObjectGuid::LowType guidLow = ObjectGuid::LowType(guidRaw);
            Player* who = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow));
            if (who && who->GetSession() && who->GetSession()->IsBot())
                continue;  // our own fills echo back through this event

            if (std::find(wait->humans.begin(), wait->humans.end(), guidLow) == wait->humans.end())
                wait->humans.push_back(guidLow);
        }

        // § F.4: a solo human queuing while a local bot-filled match of this
        // bracket is running (and balanced) gets a seat by ejection instead
        // of waiting for a fresh instance. Groups keep the fill path.
        if (queued.size() == 1)
        {
            ObjectGuid::LowType guidLow = ObjectGuid::LowType(queued[0]);
            Player* who = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow));
            if (!who || !who->GetSession() || !who->GetSession()->IsBot())
            {
                uint32 level = 0;
                uint8 race = 0;
                if (GetLevelAndRace(guidLow, level, race) && level >= minLvl && level <= maxLvl)
                {
                    TeamId team = Player::TeamIdForRace(race);
                    if (team <= TEAM_HORDE && EjectBotForWaitingHuman(uint32(typeId), uint32(minLvl), team))
                        return;  // seated by ejection/backfill, no fill needed
                }
            }
        }

        FillBGQueue(uint32(typeId), uint32(minLvl), uint32(maxLvl));
    }

    // Runs on the world thread (delivered through ProcessHooks). Invited
    // players who never joined got dropped from the queue: free the bots so
    // a later enqueue can pick them again.
    void OnClusterBGInviteExpired(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!sPlayerbotAIConfig.enabled)
            return;

        std::string data(payload, payloadLen);
        std::vector<uint64> expired;
        if (!ExtractJsonUInt64Array(data, "PlayersGUID", expired))
            return;

        for (uint64 guidRaw : expired)
        {
            clusterBGQueuedBots.erase(ObjectGuid::LowType(guidRaw));
            RemoveWaitingHuman(ObjectGuid::LowType(guidRaw));
        }
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
        PLAYERHOOK_ON_UPDATE_ZONE,
        PLAYERHOOK_ON_BEFORE_TELEPORT
    }) {}

    // A random bot pulled out of a running battleground shrinks its team below
    // MinPlayersPerTeam and AC ends the match "not enough players" (observed:
    // bot silently teleported to its grind map mid-WSG). Veto the teleport and
    // log the attempt so the trigger path becomes visible.
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float /*x*/, float /*y*/, float /*z*/,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        if (!sToCloud9Sidecar->ClusterModeEnabled())
            return true;

        if (!player->GetSession() || !player->GetSession()->IsBot() ||
            !sRandomPlayerbotMgr.IsRandomBot(player))
            return true;

        Battleground* bg = player->GetBattleground();
        if (!bg || mapid == bg->GetMapId())
            return true;

        if (bg->GetStatus() != STATUS_WAIT_JOIN && bg->GetStatus() != STATUS_IN_PROGRESS)
            return true;

        if (clusterBGEjecting.count(player->GetGUID().GetCounter()))
            return true;  // deliberate ejection (§ F.4), let the leave through

        LOG_INFO("playerbots", "Cluster: blocked teleport of BG participant bot {} to map {} (bg instance {} status {})",
                 player->GetName(), mapid, bg->GetInstanceID(), uint32(bg->GetStatus()));
        return false;
    }

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
                sToCloud9Sidecar->NatsSubscribe(GROUP_INVITE_CREATED_SUBJECT, &OnClusterGroupInviteCreated) &&
                sToCloud9Sidecar->NatsSubscribe(GUILD_INVITE_CREATED_SUBJECT, &OnClusterGuildInviteCreated) &&
                sToCloud9Sidecar->NatsSubscribe(GROUP_MESSAGE_NEW_SUBJECT, &OnClusterGroupChatMessage) &&
                sToCloud9Sidecar->NatsSubscribe(WHISPER_INCOME_SUBJECT, &OnClusterWhisper) &&
                sToCloud9Sidecar->NatsSubscribe(GUILD_MESSAGE_NEW_SUBJECT, &OnClusterGuildChatMessage) &&
                sToCloud9Sidecar->NatsSubscribe(MATCHMAKING_INVITED_SUBJECT, &OnClusterBGInvite) &&
                sToCloud9Sidecar->NatsSubscribe(MATCHMAKING_QUEUED_SUBJECT, &OnClusterBGQueueJoined) &&
                sToCloud9Sidecar->NatsSubscribe(MATCHMAKING_EXPIRED_SUBJECT, &OnClusterBGInviteExpired);

        UpdateCooldowns(diff);
        ProcessPendingKicks();
        ProcessPendingLogins(diff);
        ProcessPendingBGJoins(diff);
        ProcessBGRefills(diff);
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

        std::vector<std::pair<uint64, uint32>> staleQueueEntries;
        for (auto itr = clusterBGQueuedBots.begin(); itr != clusterBGQueuedBots.end();)
        {
            itr->second.ttl -= int32(diff);
            if (itr->second.ttl <= 0)
            {
                // Never invited within the TTL: also purge the matchmaking
                // queue entry, or the bot gets invited long after (seen
                // live: a 26 min leftover joined out of bracket).
                staleQueueEntries.push_back({uint64(itr->first), itr->second.typeId});
                itr = clusterBGQueuedBots.erase(itr);
            }
            else
                ++itr;
        }
        if (!staleQueueEntries.empty())
            std::thread([staleQueueEntries]() {
                for (auto const& entry : staleQueueEntries)
                    sToCloud9Sidecar->RemovePlayerFromBattlegroundQueue(entry.first, entry.second);
            }).detach();

        for (auto itr = clusterBGEjectHolds.begin(); itr != clusterBGEjectHolds.end();)
        {
            itr->ttl -= int32(diff);
            if (itr->ttl <= 0)
                itr = clusterBGEjectHolds.erase(itr);
            else
                ++itr;
        }
    }

    // World thread. Re-fill the queues that still have humans waiting
    // without a battleground pop (§ F.3).
    void ProcessBGRefills(uint32 diff)
    {
        for (auto itr = clusterBGWaits.begin(); itr != clusterBGWaits.end();)
        {
            itr->ttl -= int32(diff);
            if (itr->ttl <= 0 || itr->humans.empty())
            {
                itr = clusterBGWaits.erase(itr);
                continue;
            }

            itr->refillIn -= int32(diff);
            if (itr->refillIn <= 0 && itr->refillsLeft > 0)
            {
                itr->refillIn = CLUSTER_BG_REFILL_MS;
                --itr->refillsLeft;
                LOG_INFO("playerbots", "Cluster: re-fill pass for BG type {} bracket {}-{} ({} humans waiting, {} passes left)",
                         itr->typeId, itr->minLvl, itr->maxLvl, itr->humans.size(), itr->refillsLeft);
                FillBGQueue(itr->typeId, itr->minLvl, itr->maxLvl);
            }

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

            if (bot->InBattleground())
                continue;  // BG participants stay with their match (C-BG.5)

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

    // World thread. Replicates the gateway's enterBattleground tail for an
    // in-process bot whose BG runs on THIS shard: AddPlayersToBattleground
    // equivalent (entry point + bg id + teleport) then joined confirmation,
    // plus the playerbots force-join state (BattleGroundJoinAction mirror).
    void ProcessPendingBGJoins(uint32 diff)
    {
        std::vector<ClusterPendingBGJoin> joins;
        {
            std::lock_guard<std::mutex> lock(clusterPendingBGMutex);
            if (clusterPendingBGJoins.empty())
                return;
            joins.swap(clusterPendingBGJoins);
        }

        std::vector<ClusterPendingBGJoin> keep;
        for (ClusterPendingBGJoin& join : joins)
        {
            join.delay -= int32(diff);
            if (join.delay > 0)
            {
                keep.push_back(join);
                continue;
            }

            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(join.guid));
            if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot() || bot->InBattleground())
                continue;

            Battleground* bg = sBattlegroundMgr->GetBattleground(join.instanceId, BATTLEGROUND_TYPE_NONE);
            if (!bg)
            {
                // The local instance is created by the matchmaking's
                // StartBattleground call, which can land after the invite.
                if (--join.attemptsLeft > 0)
                {
                    join.delay = CLUSTER_BG_JOIN_RETRY_MS;
                    keep.push_back(join);
                }
                else
                    LOG_WARN("playerbots", "Cluster: BG instance {} never appeared locally, bot {} stays out",
                             join.instanceId, bot->GetName());
                continue;
            }

            // A bot can outlevel the bracket between the enqueue and the pop
            // (it keeps questing while queued — seen live: 19 -> 20 during a
            // 26 min leftover wait). Drop it and purge its queue entry.
            uint32 botLevel = bot->GetLevel();
            if (botLevel < bg->GetMinLevel() || botLevel > bg->GetMaxLevel())
            {
                LOG_INFO("playerbots", "Cluster: bot {} level {} out of bracket {}-{} of BG instance {}, dropped",
                         bot->GetName(), botLevel, bg->GetMinLevel(), bg->GetMaxLevel(), join.instanceId);
                clusterBGQueuedBots.erase(join.guid);
                uint64 staleGuid = join.guid;
                uint32 staleType = join.bgTypeId;
                std::thread([staleGuid, staleType]() {
                    sToCloud9Sidecar->RemovePlayerFromBattlegroundQueue(staleGuid, staleType);
                }).detach();
                continue;
            }

            clusterBGBotInstances[join.instanceId] = join.bgTypeId;

            BattlegroundTypeId bgTypeId = BattlegroundTypeId(join.bgTypeId);

            LOG_INFO("playerbots", "Cluster: bot {} force-joins BG type {} instance {}",
                     bot->GetName(), join.bgTypeId, join.instanceId);

            bot->SetEntryPoint();
            bot->SetBattlegroundId(bg->GetInstanceID(), bg->GetBgTypeID(), 1, true,
                                   bgTypeId == BATTLEGROUND_RB, bot->GetTeamId(true));
            sBattlegroundMgr->SendToBattleground(bot, bg->GetInstanceID(), bgTypeId);

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                WorldPacket emptyPacket;
                bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);

                botAI->ResetStrategies(false);
                if (!bot->GetBattleground())
                    botAI->ChangeStrategy("+bg", BOT_STATE_NON_COMBAT);

                AiObjectContext* context = botAI->GetAiObjectContext();
                context->GetValue<uint32>("bg role")->Set(urand(0, 9));
                PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
                PositionInfo pos = posMap["bg objective"];
                pos.Reset();
                posMap["bg objective"] = pos;
            }

            if (join.notifyMatchmaking)
            {
                // Blocking gRPC call: keep it off the world thread.
                uint64 guidCounter = join.guid;
                uint32 instanceId = join.instanceId;
                std::thread([guidCounter, instanceId]() {
                    if (!sToCloud9Sidecar->NotifyPlayerJoinedBattleground(guidCounter, instanceId))
                        LOG_WARN("playerbots", "Cluster: PlayerJoinedBattleground failed for bot guid {}", guidCounter);
                }).detach();
            }
        }

        if (!keep.empty())
        {
            std::lock_guard<std::mutex> lock(clusterPendingBGMutex);
            clusterPendingBGJoins.insert(clusterPendingBGJoins.end(), keep.begin(), keep.end());
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

// Chantier C-DJ: bots must walk into the instance behind their master, like
// vanilla. The vanilla relay (PlayerbotMgr::HandleMasterIncomingPacket, fed
// by PlayerbotsServerScript) forwards the master's CMSG_AREATRIGGER to every
// random bot whose master POINTER matches — brittle in cluster, where the
// master's Player object is recreated on every shard switch. Relay through
// the local group mirror instead, skipping bots the vanilla loop already
// covered so the packet is delivered exactly once.
class PlayerbotsClusterServerScript : public ServerScript
{
public:
    PlayerbotsClusterServerScript() : ServerScript("PlayerbotsClusterServerScript", {
        SERVERHOOK_CAN_PACKET_RECEIVE
    }) {}

    void OnPacketReceived(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_AREATRIGGER)
            return;

        if (!sPlayerbotAIConfig.enabled || !sToCloud9Sidecar->ClusterModeEnabled())
            return;

        Player* player = session->GetPlayer();
        if (!player || session->IsBot())
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        bool masterMgrRelayed = GET_PLAYERBOT_MGR(player) != nullptr;
        uint32 covered = 0;
        uint32 relayed = 0;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            if (slot.guid == player->GetGUID())
                continue;

            Player* member = ObjectAccessor::FindPlayer(slot.guid);
            if (!member || !member->GetSession() || !member->GetSession()->IsBot())
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
            if (!botAI)
                continue;

            if (masterMgrRelayed && botAI->GetMaster() == player)
            {
                ++covered;  // vanilla relay already delivered this packet
                continue;
            }

            botAI->HandleMasterIncomingPacket(packet);
            ++relayed;
        }

        if (covered || relayed)
            LOG_INFO("playerbots", "Cluster: area trigger from {}: {} bots on vanilla relay, {} relayed via group",
                     player->GetName(), covered, relayed);
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
            // BUG-030: the master pointer can be transiently null when the
            // removal lands; rebind it through the owner's real session
            // (alt bots share the owner's account, bot sessions are not in
            // the session map so this always yields the real player).
            if (WorldSession* ownerSession = sWorldSessionMgr->FindSession(bot->GetSession()->GetAccountId()))
            {
                Player* owner = ownerSession->GetPlayer();
                if (owner && owner->IsInWorld())
                {
                    LOG_INFO("playerbots", "Cluster: alt bot {} freed from group with no master, rebinding to {}",
                             bot->GetName(), owner->GetName());
                    botAI->SetMaster(owner);
                }
            }
        }

        if (!botAI->GetMaster())
        {
            // Owner unreachable on this shard: reset anyway, the next
            // invite rebinds the follow.
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

// Chantier C-BG.5: a participant leaving a running battleground (human quit,
// bot yanked out) shrinks its team below MinPlayersPerTeam and AC schedules
// the "not enough players" premature end. Refill the short team with local
// random bots through the C-BG.1 force-join path (queue bypassed: the bots
// join the running instance directly).
class PlayerbotsClusterBGScript : public AllBattlegroundScript
{
public:
    PlayerbotsClusterBGScript() : AllBattlegroundScript("PlayerbotsClusterBGScript", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_REMOVE_PLAYER_AT_LEAVE
    }) {}

    void OnBattlegroundRemovePlayerAtLeave(Battleground* bg, Player* player) override
    {
        if (!sPlayerbotAIConfig.enabled || !sToCloud9Sidecar->ClusterModeEnabled())
            return;

        if (!bg || !bg->isBattleground() || bg->GetStatus() != STATUS_IN_PROGRESS)
            return;

        // The hook runs after the leaver's bg data reset: use the natural
        // faction (no cross-faction battlegrounds on 3.3.5).
        TeamId team = player->GetTeamId(true);
        if (team > TEAM_HORDE)
            return;

        // § F.7: refill toward the opposite team's headcount (mirror of the
        // matchmaking backfill target), not just the minimum — a human
        // leaving a 6v6 must be replaced by a bot. Slots held for an
        // ejection stay free for the invited human.
        TeamId other = team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
        uint32 have = bg->GetPlayersCountByTeam(team);
        uint32 target = std::min<uint32>(
            std::max<uint32>(bg->GetMinPlayersPerTeam(), bg->GetPlayersCountByTeam(other)),
            bg->GetMaxPlayersPerTeam());
        uint32 held = CountEjectHolds(bg->GetInstanceID(), uint8(team));
        if (have + held >= target)
            return;

        uint32 need = target - have - held;
        uint32 scheduled = 0;

        std::lock_guard<std::mutex> lock(clusterPendingBGMutex);
        for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             need && it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld() || !bot->GetSession() || !bot->GetSession()->IsBot() ||
                !sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;

            if (bot->GetGroup() || bot->InBattleground() || bot->InBattlegroundQueue() || !bot->IsAlive())
                continue;

            if (bot->GetTeamId() != team)
                continue;

            uint32 level = bot->GetLevel();
            if (level < bg->GetMinLevel() || level > bg->GetMaxLevel())
                continue;

            ObjectGuid::LowType guidLow = bot->GetGUID().GetCounter();
            if (clusterBGQueuedBots.count(guidLow))
                continue;

            bool alreadyPending = false;
            for (ClusterPendingBGJoin const& pending : clusterPendingBGJoins)
            {
                if (pending.guid == guidLow)
                {
                    alreadyPending = true;
                    break;
                }
            }
            if (alreadyPending)
                continue;

            clusterPendingBGJoins.push_back({guidLow, uint32(bg->GetBgTypeID()), bg->GetInstanceID(),
                                             CLUSTER_BG_JOIN_ATTEMPTS, 0, false});
            --need;
            ++scheduled;
        }

        LOG_INFO("playerbots", "Cluster: backfilling BG instance {} team {} with {} bots ({} still short)",
                 bg->GetInstanceID(), uint32(team), scheduled, need);
    }
};

void AddPlayerbotsClusterScripts()
{
    new PlayerbotsClusterPlayerScript();
    new PlayerbotsClusterWorldScript();
    new PlayerbotsClusterGroupScript();
    new PlayerbotsClusterServerScript();
    new PlayerbotsClusterBGScript();
}
