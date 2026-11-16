// license:GPLv3+

#pragma once

// Castcade state stream: the physics-driven visual state of a running table.
//
// A watcher's VPX runs no physics and no script. What a script sets reaches it as replicated script calls
// (CastcadeStream). Everything else the renderer reads is produced by the physics engine on the player's
// VPX and is carried here: the balls, and the moving parts of flippers, gates, spinners, plungers, bumpers,
// targets, triggers and slingshots. The byte format is private to CastcadeState.cpp; both ends run the same
// build and load the same table file, which the session checks before any state is exchanged.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "CastcadeWire.h"

class Player;
class Ball;

namespace castcade
{

// The sound names that VPX itself plays, outside the script: each DispReel's click (DispReel::UpdateAnimation).
// The stream's StopSoundFilter never drops a stop of these.
void ReelSoundNames(Player* player, std::vector<std::wstring>& out);

// Watch role, the StopSound audit: whether StopSound(name) would stop something that is playing now (the sound's
// own buffer, or the first copy of it in the copied-buffer list, which is what PinTable::StopSound stops). name is
// lower-cased; matching is case-insensitive like VPX's.
bool StopSoundWouldStop(Player* player, const std::wstring& name);

// Play role. The physics clock: the time, on VPX's usec() clock, that the physics state has been simulated up to
// (Player::m_curPhysicsFrameTime). A frame is stamped with it, not with the moment it is sent: after a stall the two
// differ by the stall.
uint64_t PhysicsClockUsec(Player* player);

// Play role. Appends the current physics-driven state of the table to w. ballIds gives the network id of
// every live ball (the session assigns one in Player::CreateBall and forgets it in Player::DestroyBall);
// a ball that is not in the map is not sent.
void CaptureState(Player* player, const std::unordered_map<const Ball*, uint32_t>& ballIds, ByteWriter& w);

// One captured state, parsed. Opaque outside CastcadeState.cpp.
struct StateSnapshot;

// Watch role. Parses what CaptureState wrote. Returns nullptr on malformed input.
std::shared_ptr<const StateSnapshot> ParseState(ByteReader& r);

// The player's physics time (Player::m_time_msec) the snapshot was captured at
uint32_t SnapshotPhysicsMsec(const StateSnapshot& s);

// Watch role. Writes snapshots into the local table so that the next Render shows them.
class StateApplier final
{
public:
   explicit StateApplier(Player* player);
   ~StateApplier();

   // Shows the state a fraction t (0..1) of the way from a to b, for the frame about to be rendered. The
   // session calls this once per frame, after the table's UpdateAnimation pass and before anything is
   // drawn. a and b may be the same snapshot. The local balls are created and destroyed here so that they
   // match the snapshot being shown: a ball that exists in a is shown (interpolated toward b when b has it
   // too, held at a's state when not); a ball that exists only in b appears once the session passes b as a.
   // A ball that moved further between a and b than its velocity explains (a teleport: kicker to trough)
   // is not interpolated.
   void Apply(const StateSnapshot& a, const StateSnapshot& b, float t);

   // The local ball standing in for the player's ball with this network id, or nullptr. Valid until the
   // next Apply or Reset. The session uses it to replay script calls made on a ball (colour, image, ...).
   Ball* FindBall(uint32_t ballId) const;

   // The largest distance (VP units) any ball moved between the previous Apply and the last one: how far the
   // picture jumped in one frame. A ball that appeared or disappeared does not count.
   float LastMaxBallStep() const;

   // Destroys every local ball this applier created (a new keyframe, or the session ending)
   void Reset();

private:
   struct Impl;
   std::unique_ptr<Impl> m_impl;
};

}
