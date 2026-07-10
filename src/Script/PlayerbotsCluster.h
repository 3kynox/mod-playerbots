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

// ToCloud9 cluster partition helpers shared with the random bot pool.
namespace PlayerbotsCluster
{
    // True when the random bot pool must be partition-aware (cluster mode).
    bool PoolFilterActive();

    // True for a character saved on a map owned by another worldserver of
    // the cluster: that worldserver's own pool picks it up instead, so
    // selecting it here would only trigger a kick/handoff round-trip.
    bool ShouldSkipPoolCandidate(uint32 mapId);
}

#endif
