/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UsePotionsStrategy.h"

class UsePotionsStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    UsePotionsStrategyActionNodeFactory() { creators["healthstone"] = &healthstone; }

private:
    static ActionNode* healthstone(PlayerbotAI* /*botAI*/)
    {
        return new ActionNode("healthstone",
                              /*P*/ {},
                              /*A*/ { NextAction("healing potion") },
                              /*C*/ {});
    }
};

UsePotionsStrategy::UsePotionsStrategy(PlayerbotAI* botAI) : Strategy(botAI)
{
    actionNodeFactories.Add(new UsePotionsStrategyActionNodeFactory());
}

void UsePotionsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);

    // Above the flee that PanicTrigger fires at ACTION_EMERGENCY + 9. At 21 against 99 the
    // potion never got its turn: a bot below critical health ran off with a full stack in its
    // bag, at the same speed as whatever was chasing it, and died anyway. Drinking is the one
    // move that changes the outcome, so it is tried first -- and if there is no potion and no
    // healthstone the action simply fails and flee takes over on the same tick.
    triggers.push_back(new TriggerNode(
        "critical health", { NextAction("healthstone", ACTION_EMERGENCY + 10) }));
    triggers.push_back(
        new TriggerNode("medium mana", { NextAction("mana potion", ACTION_EMERGENCY) }));
}
