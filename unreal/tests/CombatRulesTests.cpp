#include "CombatRules.h"
#include <cassert>
#include <limits>
#include <cstdio>

int main()
{
    assert(CombatRules::CanAttack(100, true, 1.0, 0.0));
    assert(!CombatRules::CanAttack(100, false, 1.0, 0.0));
    assert(!CombatRules::CanAttack(0, true, 1.0, 0.0));
    assert(!CombatRules::CanAttack(100, true, 0.64, 0.0));
    assert(CombatRules::CanAttack(100, true, 0.65, 0.0));
    assert(!CombatRules::CanAttack(100, true, std::numeric_limits<double>::quiet_NaN(), 0.0));
    assert(CombatRules::ApplyDamage(100, 25) == 75);
    assert(CombatRules::ApplyDamage(10, 25) == 0);
    assert(CombatRules::ApplyDamage(0, 25) == 0);
    assert(CombatRules::ApplyDamage(75, -25) == 75);
    assert(CombatRules::IsInitialGun(true, 9));
    assert(!CombatRules::IsInitialGun(false, 9)); // A drawn gun is still melee.
    assert(!CombatRules::IsInitialGun(true, 0));
    // A lethal transition happens once; further hits on a dead player do not repeat it.
    assert(CombatRules::IsLethalHit(25, 25));
    assert(!CombatRules::IsLethalHit(50, 25));
    assert(!CombatRules::IsLethalHit(0, 25));
    assert(!CombatRules::CanRespawn(0, true, 12.99, 13.0));
    assert(CombatRules::CanRespawn(0, true, 13.0, 13.0));
    assert(!CombatRules::CanRespawn(100, true, 13.0, 13.0));
    assert(!CombatRules::CanRespawn(0, false, 13.0, 13.0));
    assert(!CombatRules::CanRespawn(0, true, 13.0, 0.0));
    assert(!CombatRules::CanRespawn(0, true, std::numeric_limits<double>::quiet_NaN(), 13.0));
    assert(CombatRules::FallBoundary(100.0, -1000.0) < -400.0); // Never kill a player standing on the map.
    assert(CombatRules::FallBoundary(100.0, -1000.0) > -1000.0); // Intercept before UE destroys the Pawn.
    assert(CombatRules::FallBoundary(100.0, -100000.0) == -1700.0);
    assert(CombatRules::HoldPointsDue(true, 10.99, 10.0, 0) == 0);
    assert(CombatRules::HoldPointsDue(true, 11.0, 10.0, 0) == 1);
    assert(CombatRules::HoldPointsDue(true, 13.4, 10.0, 1) == 2); // Catch up after a slow frame.
    assert(CombatRules::HoldPointsDue(true, 13.4, 10.0, 3) == 0); // Never double award.
    assert(CombatRules::HoldPointsDue(false, 13.4, 10.0, 0) == 0);
    assert(CombatRules::HoldPointsDue(true, 9.0, 10.0, 0) == 0);
    assert(CombatRules::HoldPointsDue(true, std::numeric_limits<double>::infinity(), 10.0, 0) == 0);
    assert(CombatRules::HoldPointsDue(true, 13.4, 13.0, 0) == 0); // A new holder starts a new full second.
    assert(CombatRules::KillPoints(true, true, false) == 3);
    assert(CombatRules::KillPoints(true, false, false) == 0); // Falling has no killer.
    assert(CombatRules::KillPoints(true, true, true) == 0);
    assert(CombatRules::KillPoints(false, true, false) == 0);
    assert(!CombatRules::PlayersReady(1, 1));
    assert(!CombatRules::PlayersReady(2, 1));
    assert(CombatRules::PlayersReady(2, 2));
    assert(CombatRules::PlayersReady(4, 4));
    assert(!CombatRules::RoundActive(false, 10.0, 190.0));
    assert(CombatRules::RoundActive(true, 189.99, 190.0));
    assert(!CombatRules::RoundActive(true, 190.0, 190.0));
    assert(!CombatRules::RoundActive(true, 190.1, 190.0));
    assert(!CombatRules::RoundActive(true, std::numeric_limits<double>::quiet_NaN(), 190.0));
    assert(!CombatRules::CanRestart(true, true, true)); // This demo deliberately supports one match only.
    assert(!CombatRules::CanRestart(true, true, false));
    assert(!CombatRules::CanRestart(true, false, true));
    assert(!CombatRules::CanRestart(false, true, true));
    const int Scores[] = {10, 10, 3};
    assert(CombatRules::RankForScore(10, Scores, 3) == 1);
    assert(CombatRules::RankForScore(3, Scores, 3) == 3);
    assert(CombatRules::ObjectiveDropInBounds(0, 0));
    assert(CombatRules::ObjectiveDropInBounds(25000000, 300));
    assert(CombatRules::ObjectiveDropInBounds(0, -500));
    assert(!CombatRules::ObjectiveDropInBounds(25000001, 0));
    assert(!CombatRules::ObjectiveDropInBounds(0, 300.1));
    assert(!CombatRules::ObjectiveDropInBounds(0, -500.1));
    assert(!CombatRules::ObjectiveDropInBounds(std::numeric_limits<double>::infinity(), 0));
    assert(!CombatRules::ObjectiveDropInBounds(0, std::numeric_limits<double>::quiet_NaN()));
    assert(CombatRules::SpawnProtectionSeconds == 3.0);
    assert(CombatRules::IsSpawnProtected(100,10.0,13.0));
    assert(CombatRules::IsSpawnProtected(100,12.999,13.0));
    assert(!CombatRules::IsSpawnProtected(100,13.0,13.0));
    assert(!CombatRules::IsSpawnProtected(100,13.1,13.0));
    assert(!CombatRules::IsSpawnProtected(0,10.0,13.0));
    assert(!CombatRules::IsSpawnProtected(100,10.0,0.0));
    assert(!CombatRules::CanReceiveDamage(100,true,12.999,13.0));
    assert(CombatRules::CanReceiveDamage(100,true,13.0,13.0));
    assert(CombatRules::CanReceiveDamage(100,true,13.1,0.0));
    assert(!CombatRules::CanReceiveDamage(0,true,13.1,0.0));
    assert(!CombatRules::CanReceiveDamage(100,false,13.1,0.0));
    assert(!CombatRules::CanReceiveDamage(100,true,std::numeric_limits<double>::quiet_NaN(),13.0));
    assert(!CombatRules::CanReceiveDamage(100,true,10.0,std::numeric_limits<double>::infinity()));
    assert(CombatRules::RoundSeconds == 480.0);
    std::puts("Combat rule checks: 75 passed (8-minute single match and 3-second protection)");
}
