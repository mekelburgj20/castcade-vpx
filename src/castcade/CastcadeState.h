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
#include <unordered_map>

#include "CastcadeWire.h"

class Player;
class Ball;

namespace castcade
{

// Play role. Appends the current physics-driven state of the table to w. ballIds gives the network id of
// every live ball (the session assigns one in Player::CreateBall and forgets it in Player::DestroyBall);
// a ball that is not in the map is not sent.
void CaptureState(Player* player, const std::unordered_map<const Ball*, uint32_t>& ballIds, ByteWriter& w);

// One captured state, parsed. Opaque outside CastcadeState.cpp.
struct StateSnapshot;

// Watch role. Parses what CaptureState wrote. Returns nullptr on malformed input.
std::shared_ptr<const StateSnapshot> ParseState(ByteReader& r);

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

   // Destroys every local ball this applier created (a new keyframe, or the session ending)
   void Reset();

private:
   struct Impl;
   std::unique_ptr<Impl> m_impl;
};

}
