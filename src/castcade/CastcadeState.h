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

// Watch role: how many sound buffers play now (sounds' own buffers and copied ones)
int CountPlayingSounds(Player* player);

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

// ---------------------------------------------------------------------------------------------------------
// Match role (CASTCADE_STREAM=match): a table that plays its own game shows another player's for a while and
// then gets its own back. What a script can read back as a property is put back by the session's undo list;
// the rest of what a watch phase changes is saved and restored here, field by field.

// What the script's methods, the light sequencers and time put into the parts and no property returns: every
// light's play state, fade and blink phase, a flasher's and a primitive's sequencer state, a primitive's
// animation, a light sequencer's whole run state, a reel's digits. Saved once before the script starts (the state
// the table file gives, which is what a stream's keyframe builds on) and again when a watch phase begins.
struct TableRuntime;
std::shared_ptr<const TableRuntime> SaveTableRuntime(Player* player);
// stampsFromNow: the saved time stamps (a blinker's next step, a reel's next update) are of another time: they start
// again from the game clock as it is now. False when the game clock itself is put back to when they were saved.
void RestoreTableRuntime(Player* player, const TableRuntime& saved, bool stampsFromNow);

// Everything a StateApplier writes on the local parts, and what a replayed put can move behind it: the movers of
// flippers, gates, spinners and plungers, and the hit animations of bumpers, targets, triggers and slingshots.
struct OwnPhysics;
std::shared_ptr<const OwnPhysics> SaveOwnPhysics(Player* player);
void RestoreOwnPhysics(Player* player, const OwnPhysics& saved);

// The player's own side of a watch phase: its clocks, its balls, its music.
struct OwnPlayer;
// Start of a watch phase: remembers the game clock, the balls and the ball id counter; the player no longer pauses
std::shared_ptr<OwnPlayer> BeginWatchPhase(Player* player);
// Once the other player's table is about to be drawn: the local balls are hidden (never destroyed: the script holds
// them) and the local music is set aside
void HideOwnTable(Player* player, OwnPlayer& own);
// End of a watch phase, after the watcher's balls are gone. Shows the local balls and gives the music back when they
// were set aside, and puts the game clock back to where it was when the phase began, without a burst of physics for
// the time in between (as VPX itself resumes from a pause). Returns the milliseconds the clock was moved back.
unsigned int EndWatchPhase(Player* player, OwnPlayer& own);
// Logs: "2: #1 (431.0, 1622.5, 25.0) v (0.00, -3.10, 0.00); #2 ..." for the local balls of a watch phase
std::string DescribeOwnBalls(const OwnPlayer& own);
// The local time (Player::m_time_msec) and the local balls, for the test log (CASTCADE_TEST_BALLLOG)
std::string DescribeLiveBalls(Player* player);

// PinTable::GetMaterial(name): the material a replayed UpdateMaterial / MaterialColor is about to change, copied; and
// the copy written back. An unknown name gives nullptr.
struct SavedMaterial;
std::shared_ptr<const SavedMaterial> SaveMaterial(Player* player, const std::wstring& name);
void RestoreMaterial(Player* player, const SavedMaterial& saved);

}
