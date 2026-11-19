#pragma once
#include <cmath>
#include <algorithm>

// Small shared rules also exercised outside the editor; no multiplayer test harness.
namespace CombatRules
{
constexpr int MaxHealth = 100;
constexpr int Damage = 25;
constexpr double AttackInterval = 0.65;
constexpr float MeleeWindup = 0.2f;
constexpr float MeleeReach = 160.f;
constexpr float RifleReach = 3000.f;
constexpr double RespawnDelay = 3.0;
constexpr double SpawnProtectionSeconds = 3.0;
constexpr double RoundSeconds = 480.0;
constexpr bool AllowRoundRestart = false;
constexpr double CountdownSeconds = 3.0;
inline bool CanAttack(int Health, bool Active, double Now, double LastAttack)
{
    return Health > 0 && Active && std::isfinite(Now) && std::isfinite(LastAttack)
        && Now - LastAttack >= AttackInterval;
}
inline int ApplyDamage(int Health, int Amount)
{
    if (Health <= 0) return 0;
    if (Amount <= 0) return Health;
    return Amount >= Health ? 0 : Health - Amount;
}
inline bool IsSpawnProtected(int Health, double Now, double ProtectedUntil)
{
    return Health > 0 && std::isfinite(Now) && std::isfinite(ProtectedUntil)
        && ProtectedUntil > 0 && Now < ProtectedUntil;
}
inline bool CanReceiveDamage(int Health, bool Active, double Now, double ProtectedUntil)
{
    return Health > 0 && Active && std::isfinite(Now) && std::isfinite(ProtectedUntil)
        && !IsSpawnProtected(Health, Now, ProtectedUntil);
}
inline bool IsInitialGun(bool Sample, int Preset) { return Sample && Preset == 9; }
inline bool IsLethalHit(int Health, int Amount) { return Health > 0 && ApplyDamage(Health, Amount) == 0; }
inline bool CanRespawn(int Health, bool Active, double Now, double Deadline)
{
    return Health <= 0 && Active && Deadline > 0 && std::isfinite(Now)
        && std::isfinite(Deadline) && Now >= Deadline;
}
inline double FallBoundary(double SpawnZ, double KillZ)
{
    return (std::min)(SpawnZ - 600.0, (std::max)(KillZ + 300.0, SpawnZ - 1800.0));
}
inline int HoldPointsDue(bool Active, double Now, double Since, int Awarded)
{
    if (!Active || !std::isfinite(Now) || !std::isfinite(Since) || Now < Since) return 0;
    const int Completed = static_cast<int>((std::min)(std::floor(Now - Since), 1000000.0));
    return (std::max)(0, Completed - Awarded);
}
inline int KillPoints(bool Active, bool HasKiller, bool SelfKill)
{
    return Active && HasKiller && !SelfKill ? 3 : 0;
}
inline bool PlayersReady(int Participants, int Ready) { return Participants >= 2 && Ready == Participants; }
inline bool RoundActive(bool Playing, double Now, double Deadline)
{
    return Playing && std::isfinite(Now) && std::isfinite(Deadline) && Now < Deadline;
}
inline bool CanRestart(bool Finished, bool Authority, bool LocalHost) { return AllowRoundRestart && Finished && Authority && LocalHost; }
inline int RankForScore(int Score, const int* Scores, int Count)
{
    int Rank = 1;
    for (int Index = 0; Index < Count; ++Index) if (Scores[Index] > Score) ++Rank;
    return Rank;
}
inline bool ObjectiveDropInBounds(double HorizontalDistanceSquared, double HeightDelta)
{
    return std::isfinite(HorizontalDistanceSquared) && std::isfinite(HeightDelta)
        && HorizontalDistanceSquared >= 0 && HorizontalDistanceSquared <= 25000000.0
        && HeightDelta >= -500.0 && HeightDelta <= 300.0;
}
}
