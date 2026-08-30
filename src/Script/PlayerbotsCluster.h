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

#ifndef _PLAYERBOT_PLAYERBOTSCLUSTER_H
#define _PLAYERBOT_PLAYERBOTSCLUSTER_H

#include "Define.h"

#include <string>

// ToCloud9 cluster partition helpers shared with the random bot pool.
namespace PlayerbotsCluster
{
    // True when the random bot pool must be partition-aware (cluster mode).
    bool PoolFilterActive();

    // True for a character saved on a map owned by another worldserver of
    // the cluster: that worldserver's own pool picks it up instead, so
    // selecting it here would only trigger a kick/handoff round-trip.
    bool ShouldSkipPoolCandidate(uint32 mapId);

    // BUG-TC9-071: a master switching worldserver (boat, zeppelin, GM
    // teleport) takes his alt bots down with his session. The leaving server
    // publishes the active set; every server caches it briefly, and the one
    // where the master lands re-adds the bots. The short TTL is what keeps a
    // plain evening logout from resurrecting them at the next morning login.
    void PublishAltbotsParked(uint32 masterLow, std::string const& botNames);

    // Consumes (and erases) the parked set for this master, "" if none/expired.
    std::string TakeParkedAltbots(uint32 masterLow);
}

#endif
