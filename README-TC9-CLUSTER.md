# mod-playerbots on a ToCloud9 cluster — `tc9-cluster` branch

This branch is the maintained line of my work running
[liyunfan1223/mod-playerbots](https://github.com/liyunfan1223/mod-playerbots)
on a [ToCloud9](https://github.com/walkline/ToCloud9) sharded cluster:
several AzerothCore worldservers, each owning a subset of maps, behind a
shared gateway, with random bots populating all shards.

It tracks upstream `test-staging` (last alignment: 2026-08-23) and carries
the cluster layer on top. If you want to run playerbots on a TC9 cluster,
build from this branch; the companion repos are:

| repo | branch |
|---|---|
| ToCloud9 (sidecar, gateway, services) | `3kynox/ToCloud9` → `integration/kp-20260828` |
| AzerothCore (cluster-mode base) | `3kynox/azerothcore-wotlk` → `fix/tp-instance-altbots-20260830` |
| mod-playerbots | this branch |

## What the cluster layer adds

- Partition-aware random bot pool: each worldserver only picks characters
  saved on maps it owns; bots crossing shard boundaries are handed off
  (logout + login-request over NATS) with single-writer ownership
  accounting on both sides.
- Cross-shard battleground fill: a reconciler short of local bots
  broadcasts the need; owning shards hand over provably idle bots.
- Alt bots follow their master across worldservers (boat, zeppelin,
  teleport): the leaving server parks the active set, the arrival server
  re-adds the bots at the master's feet.
- Cluster-aware chat, groups, guilds, whispers and BG queue wiring
  through the ToCloud9 sidecar.

## Upstream tracking

Improvements that are not cluster-specific are meant to go upstream to
liyunfan1223/mod-playerbots as focused PRs. Current state:

**Pending, PR-able as-is** (clean cherry-picks on `test-staging`):
`69964238` (WANDER_NPC also seeks quest givers), `c0fb7a5d` (stop to
loot), `aa58af92` (flee facing forward), `2f2c89e2` (eat/drink before
combat — overlap with merged #2607 to re-check).

**Pending, need a manual port** (context drift vs upstream nav changes):
`158e46ca` (GO_GRIND/GO_CAMP bounded), `c6cb8046` (abandon quests with
no objective progress), `abc0214f` + `4e2a201d` (quest/POI choice by
distance), `bf767f34`, `8e3afc2a`.

**Superseded / not proposed**: rogue stealth and flee rewrites (replaced
by cmangos-derived ports), instrumentation commits, and everything
prefixed `Cluster:` (inherently tied to ToCloud9).

**Already merged upstream** while this work was ongoing: #2592, #2606,
#2372, #2607. Related upstream threads: issue #2610 and PR #2632
(quest-objective execution) cover the same ground as my quest-target
work, so that part stays local until they settle.
