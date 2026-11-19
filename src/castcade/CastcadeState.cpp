// license:GPLv3+

#include "stdafx.h"
#include "CastcadeState.h"

#include <algorithm>
#include <cstdio>

// What is carried, per class, and how a watcher makes Render show it:
//
//   Flipper    mover angle                 Flipper::Render reads it directly
//   Gate       mover angle                 Gate::Render rebuilds its wire mesh when the angle changes
//   Spinner    mover angle                 Spinner::Render (UpdatePlate) rebuilds the plate when the angle changes
//   Plunger    mover position              Plunger::Render picks the animation frame from it
//   Bumper     ring offset, skirt tilt     ring vertex buffer and skirt vertex buffer rebuilt here, the way
//                                          Bumper::UpdateAnimation and Bumper::UpdateSkirt do
//   HitTarget  animation offset, dropped   HitTarget::UpdateTarget called here
//   Trigger    animation height offset     Trigger::Render rebuilds its mesh when the offset changes
//   Slingshot  "drawn" (a wall with        Surface::RenderSlingshots draws the whole slingshot mesh while any of
//              slingshot segments)          its segments has m_iframe or m_doHitEvent
//   Balls      id, position, velocity, orientation, radius, locked in kicker
//   Nudge      table displacement and analog nudge; replayed through the watcher's own nudge strength and gains
//
// Kicker, Rubber, Ramp, Primitive, Light, DispReel, Textbox, Flasher, Decal, Timer and LightSeq have nothing a
// physics step or a ball hit changes on screen: their animations are driven by the script (replicated as script
// calls) or by time.
//
// Wire format, version 1 (little endian; both ends run the same build on the same table file):
//   u8   version
//   u32  physics time of the capture, msec
//   u8   number of part classes (PC_Count)
//   varu number of parts of each class, in PartClass order; parts are taken in PinTable::m_vedit order
//   f32  every class's values, class after class, kFloats[class] per part
//   u8[] for each class with a flag, one bit per part, (count + 7) / 8 bytes
//   f32  x4 nudge: table displacement x, y, analog nudge x, y
//   varu number of balls; per ball: varu id, f32 position[3], velocity[3], orientation[9] (row major),
//        radius, u8 flags (bit 0: locked in kicker)
//
// The watcher's StateApplier must be Reset() while the Player is still alive if its balls should be destroyed;
// its destructor only frees its own memory, because it may run after the Player is gone.

namespace castcade
{

enum PartClass : int
{
   PC_Flipper,
   PC_Gate,
   PC_Spinner,
   PC_Plunger,
   PC_Bumper,
   PC_HitTarget,
   PC_Trigger,
   PC_Slingshot,
   PC_Count
};

static constexpr uint8_t kFormatVersion = 1;
static constexpr int kFloats[PC_Count] = { 1, 1, 1, 1, 3, 1, 1, 0 };
static constexpr bool kHasFlag[PC_Count] = { false, false, false, false, false, true, false, true };
// Bounds against malformed input, far above any real table
static constexpr uint32_t kMaxParts = 1u << 16;
static constexpr uint32_t kMaxBalls = 1024;
static constexpr size_t kMinBallBytes = 1 + 16 * sizeof(float) + 1;

struct StateSnapshot
{
   struct BallState
   {
      uint32_t id;
      Vertex3Ds pos;
      Vertex3Ds vel;
      float orientation[3][3];
      float radius;
      bool lockedInKicker;
   };

   uint32_t timeMsec = 0;
   uint32_t counts[PC_Count] = {};
   std::vector<float> values[PC_Count];
   std::vector<uint8_t> flags[PC_Count]; // one byte (0 / 1) per part, for the classes with a flag
   float nudge[4] = {};
   std::vector<BallState> balls;
};

using BallState = StateSnapshot::BallState;

static bool IsFinite(const float v)
{
   // By the bit pattern: /fp:fast lets the compiler assume no NaN in a comparison
   uint32_t bits;
   memcpy(&bits, &v, sizeof(bits));
   return (bits & 0x7F800000u) != 0x7F800000u;
}

// Exact at both ends, so that a == b or t == 1 shows the snapshot unchanged
static float Lerp(const float a, const float b, const float t)
{
   if (t <= 0.f)
      return a;
   if (t >= 1.f)
      return b;
   return a + (b - a) * t;
}

// A free spinner's angle lives in [0, 2pi] and wraps: go the short way round
static float LerpWrapped(const float a, const float b, const float t)
{
   if (t <= 0.f)
      return a;
   if (t >= 1.f)
      return b;
   constexpr float twoPi = (float)(2.0 * M_PI);
   float d = b - a;
   if (d > (float)M_PI)
      d -= twoPi;
   else if (d < (float)-M_PI)
      d += twoPi;
   float v = a + d * t;
   if (v < 0.f)
      v += twoPi;
   else if (v > twoPi)
      v -= twoPi;
   return v;
}

// Discrete state (a flag, the skirt tilt) is not blended: show the snapshot nearer in time
template <class T> static const T& Nearest(const T& a, const T& b, const float t)
{
   return t < 0.5f ? a : b;
}

static const BallState* FindBallState(const std::vector<BallState>& balls, const uint32_t id)
{
   for (const BallState& s : balls)
      if (s.id == id)
         return &s;
   return nullptr;
}

// Interpolated ball state. teleport is set when a and b are too far apart for the ball to have rolled there, in
// which case a is shown as is: a ball moved by the script (kicker to trough, a VUK) must not be drawn sliding
// across the playfield. The limit is three times the distance the faster of the two velocities covers in the
// physics time between the snapshots (velocities are in VP units per 10 ms), plus a ball diameter for the slow
// cases (a ball resting in a kicker, collisions that reverse the ball).
static void InterpolateBall(const BallState& a, const BallState* const b, const float t, const float dtMsec, BallState& out, bool& teleport)
{
   teleport = false;
   if (b == nullptr || t <= 0.f)
   {
      out = a;
      return;
   }
   const Vertex3Ds d(b->pos.x - a.pos.x, b->pos.y - a.pos.y, b->pos.z - a.pos.z);
   const float speed = sqrtf(max(a.vel.LengthSquared(), b->vel.LengthSquared()));
   const float limit = 3.f * speed * (dtMsec * 0.1f) + 2.f * max(a.radius, b->radius);
   if (d.LengthSquared() > limit * limit)
   {
      teleport = true;
      out = a;
      return;
   }
   if (t >= 1.f)
   {
      out = *b;
      return;
   }
   out.id = a.id;
   out.pos = Vertex3Ds(Lerp(a.pos.x, b->pos.x, t), Lerp(a.pos.y, b->pos.y, t), Lerp(a.pos.z, b->pos.z, t));
   out.vel = Vertex3Ds(Lerp(a.vel.x, b->vel.x, t), Lerp(a.vel.y, b->vel.y, t), Lerp(a.vel.z, b->vel.z, t));
   out.radius = Lerp(a.radius, b->radius, t);
   out.lockedInKicker = Nearest(a.lockedInKicker, b->lockedInKicker, t);

   // Element-wise blend, then re-orthonormalise. A fast spinning ball can turn by half a revolution between
   // snapshots, where the blend degenerates: then keep the nearer orientation, nobody can see the spin anyway.
   Matrix3 m;
   for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
         m.m_d[i][j] = Lerp(a.orientation[i][j], b->orientation[i][j], t);
   const Vertex3Ds vx(m.m_d[0][0], m.m_d[1][0], m.m_d[2][0]);
   const Vertex3Ds vy(m.m_d[0][1], m.m_d[1][1], m.m_d[2][1]);
   if (vx.LengthSquared() > 0.25f && CrossProduct(vx, vy).LengthSquared() > 0.0625f)
   {
      m.OrthoNormalize();
      memcpy(out.orientation, m.m_d, sizeof(out.orientation));
   }
   else
      memcpy(out.orientation, Nearest(a, *b, t).orientation, sizeof(out.orientation));
}

static void WriteBall(Ball* const ball, const BallState& s)
{
   ball->m_d.m_pos = s.pos;
   ball->m_d.m_vel = s.vel;
   ball->m_d.m_radius = s.radius;
   ball->m_d.m_lockedInKicker = s.lockedInKicker;
   memcpy(ball->m_orientation.m_d, s.orientation, sizeof(s.orientation));
}

// The part classes and Player befriend this struct (CastcadeDispatch.h). Everything that touches their private
// members is here.
struct StateAccess
{
   // The one walk both ends make: same table file, same m_vedit order, same classification
   template <class F> static void ForEachPart(PinTable* const table, F&& f)
   {
      for (IEditable* const pe : table->m_vedit)
      {
         switch (pe->GetItemType())
         {
         case eItemFlipper: f(PC_Flipper, pe); break;
         case eItemGate: f(PC_Gate, pe); break;
         case eItemSpinner: f(PC_Spinner, pe); break;
         case eItemPlunger: f(PC_Plunger, pe); break;
         case eItemBumper: f(PC_Bumper, pe); break;
         case eItemHitTarget: f(PC_HitTarget, pe); break;
         case eItemTrigger: f(PC_Trigger, pe); break;
         case eItemSurface:
            // m_vlinesling comes from the table's drag points (GetHitShapes), so both ends agree on it
            if (!static_cast<Surface*>(pe)->m_vlinesling.empty())
               f(PC_Slingshot, pe);
            break;
         default: break;
         }
      }
   }

   static uint64_t PhysicsClock(const Player* const player) { return player->m_curPhysicsFrameTime; }

   static void CountParts(PinTable* const table, uint32_t counts[PC_Count])
   {
      for (int c = 0; c < PC_Count; ++c)
         counts[c] = 0;
      ForEachPart(table, [&](const PartClass c, IEditable*) { counts[c]++; });
   }

   // Values (kFloats[c] of them) and flag of one part, as Render currently shows it
   static void Read(const PartClass c, IEditable* const pe, float* const v, bool& flag)
   {
      flag = false;
      switch (c)
      {
      case PC_Flipper:
      {
         const Flipper* const p = static_cast<Flipper*>(pe);
         v[0] = p->m_phitflipper ? p->m_phitflipper->m_flipperMover.m_angleCur : 0.f;
         break;
      }
      case PC_Gate:
      {
         const Gate* const p = static_cast<Gate*>(pe);
         v[0] = p->m_phitgate ? p->m_phitgate->m_gateMover.m_angle : 0.f;
         break;
      }
      case PC_Spinner:
      {
         const Spinner* const p = static_cast<Spinner*>(pe);
         v[0] = p->m_phitspinner ? p->m_phitspinner->m_spinnerMover.m_angle : 0.f;
         break;
      }
      case PC_Plunger:
      {
         const Plunger* const p = static_cast<Plunger*>(pe);
         v[0] = p->m_phitplunger ? p->m_phitplunger->m_plungerMover.m_pos : 0.f;
         break;
      }
      case PC_Bumper:
      {
         const Bumper* const p = static_cast<Bumper*>(pe);
         v[0] = p->m_pbumperhitcircle ? p->m_pbumperhitcircle->m_bumperanim_ringAnimOffset : 0.f;
         v[1] = p->m_rotx;
         v[2] = p->m_roty;
         break;
      }
      case PC_HitTarget:
      {
         const HitTarget* const p = static_cast<HitTarget*>(pe);
         v[0] = p->m_moveAnimationOffset;
         flag = p->m_d.m_isDropped;
         break;
      }
      case PC_Trigger:
      {
         const Trigger* const p = static_cast<Trigger*>(pe);
         v[0] = p->m_animHeightOffset;
         break;
      }
      case PC_Slingshot:
      {
         const Surface* const p = static_cast<Surface*>(pe);
         for (const LineSegSlingshot* const ls : p->m_vlinesling)
            flag |= ls->m_iframe || ls->m_doHitEvent;
         break;
      }
      default: break;
      }
   }

   static void Capture(Player* const player, const std::unordered_map<const Ball*, uint32_t>& ballIds, ByteWriter& w)
   {
      PinTable* const table = player->m_ptable;
      uint32_t counts[PC_Count];
      CountParts(table, counts);

      w.u8(kFormatVersion);
      w.u32(player->m_time_msec);
      w.u8((uint8_t)PC_Count);
      for (int c = 0; c < PC_Count; ++c)
         w.varu(counts[c]);

      // Values are grouped by class but found in one walk: each class's block is placed up front, then filled
      size_t valueAt[PC_Count], flagAt[PC_Count];
      size_t at = w.size();
      for (int c = 0; c < PC_Count; ++c)
      {
         valueAt[c] = at;
         at += (size_t)counts[c] * kFloats[c] * sizeof(float);
      }
      for (int c = 0; c < PC_Count; ++c)
      {
         flagAt[c] = at;
         if (kHasFlag[c])
            at += (counts[c] + 7) / 8;
      }
      std::vector<uint8_t>& buf = w.vec();
      buf.resize(at, 0);
      uint32_t index[PC_Count] = {};
      ForEachPart(table, [&](const PartClass c, IEditable* const pe)
      {
         float v[3];
         bool flag;
         Read(c, pe, v, flag);
         const uint32_t i = index[c]++;
         memcpy(buf.data() + valueAt[c] + (size_t)i * kFloats[c] * sizeof(float), v, kFloats[c] * sizeof(float));
         if (kHasFlag[c] && flag)
            buf[flagAt[c] + i / 8] |= (uint8_t)(1u << (i % 8));
      });

      w.f32(player->m_tableDisplacement.x);
      w.f32(player->m_tableDisplacement.y);
      // GetNudgeX() / GetNudgeY() in player.cpp
      w.f32((float)player->m_curAccel[0].x * (float)(2.0 / JOYRANGE));
      w.f32((float)player->m_curAccel[0].y * (float)(2.0 / JOYRANGE));

      uint32_t ballCount = 0;
      for (const Ball* const ball : player->m_vball)
         ballCount += ballIds.count(ball) ? 1 : 0;
      w.varu(ballCount);
      for (const Ball* const ball : player->m_vball)
      {
         const auto it = ballIds.find(ball);
         if (it == ballIds.end())
            continue;
         w.varu(it->second);
         w.f32(ball->m_d.m_pos.x);
         w.f32(ball->m_d.m_pos.y);
         w.f32(ball->m_d.m_pos.z);
         w.f32(ball->m_d.m_vel.x);
         w.f32(ball->m_d.m_vel.y);
         w.f32(ball->m_d.m_vel.z);
         w.raw(ball->m_orientation.m_d, 9 * sizeof(float));
         w.f32(ball->m_d.m_radius);
         w.u8(ball->m_d.m_lockedInKicker ? 1 : 0);
      }
   }

   static void RebuildBumperRing(Bumper* const p)
   {
      // As Bumper::UpdateAnimation: the ring height lives in the vertex buffer, not in a transform
      if (p->m_ringMeshBuffer == nullptr || p->m_ringVertices == nullptr)
         return;
      const float offset = p->m_pbumperhitcircle->m_bumperanim_ringAnimOffset;
      const unsigned int n = p->m_ringMeshBuffer->m_vb->m_count;
      Vertex3D_NoTex2* buf;
      p->m_ringMeshBuffer->m_vb->lock(0, 0, (void**)&buf, VertexBuffer::DISCARDCONTENTS);
      for (unsigned int i = 0; i < n; i++)
      {
         buf[i] = p->m_ringVertices[i];
         buf[i].z += offset;
      }
      p->m_ringMeshBuffer->m_vb->unlock();
   }

   static void SetBumperSkirt(Bumper* const p, const float rotx, const float roty)
   {
      if (rotx == 0.f && roty == 0.f)
      {
         p->UpdateSkirt(false);
         return;
      }
      // UpdateSkirt(true) derives the tilt from where the ball hit: rotx = 5 cos(atan(dx / dy)) and
      // roty = 5 sin(atan(dx / dy)) with dx, dy the distances from the centre, signed by the side of the hit.
      // A hit at |roty| across and |rotx| along, on the sides the signs say, gives back this tilt (to ~1e-5).
      Vertex3Ds& hit = p->m_pbumperhitcircle->m_bumperanim_hitBallPosition;
      const Vertex3Ds saved = hit;
      hit.x = p->m_d.m_vCenter.x + (roty < 0.f ? -fabsf(roty) : fabsf(roty));
      hit.y = p->m_d.m_vCenter.y + (rotx < 0.f ? fabsf(rotx) : -fabsf(rotx));
      p->UpdateSkirt(true);
      hit = saved;
   }

   // Besides writing the streamed values, each part's own hit animation is stopped: no ball hits anything on a
   // watcher, but the bumpers start with a pending hit (m_bumperanim_hitEvent = m_d.m_hitEvent) and a replayed
   // PlayHit or IsDropped starts one too, and a local animation would run against the streamed values (forever,
   // for a bumper ring that is reset every frame).
   static void ApplyPart(const PartClass c, IEditable* const pe, const float* const va, const float* const vb, const bool fa, const bool fb, const float t)
   {
      switch (c)
      {
      case PC_Flipper:
      {
         Flipper* const p = static_cast<Flipper*>(pe);
         if (p->m_phitflipper)
            p->m_phitflipper->m_flipperMover.m_angleCur = Lerp(va[0], vb[0], t);
         break;
      }
      case PC_Gate:
      {
         // Bounded by the gate's limits (two-way gates: +-angleMax), no wrap
         Gate* const p = static_cast<Gate*>(pe);
         if (p->m_phitgate)
            p->m_phitgate->m_gateMover.m_angle = Lerp(va[0], vb[0], t);
         break;
      }
      case PC_Spinner:
      {
         // Limited spinners stay within [angleMin, angleMax]; free ones (min == max) wrap in [0, 2pi]
         Spinner* const p = static_cast<Spinner*>(pe);
         if (p->m_phitspinner)
            p->m_phitspinner->m_spinnerMover.m_angle = p->m_d.m_angleMin == p->m_d.m_angleMax ? LerpWrapped(va[0], vb[0], t) : Lerp(va[0], vb[0], t);
         break;
      }
      case PC_Plunger:
      {
         Plunger* const p = static_cast<Plunger*>(pe);
         if (p->m_phitplunger)
            p->m_phitplunger->m_plungerMover.m_pos = Lerp(va[0], vb[0], t);
         break;
      }
      case PC_Bumper:
      {
         Bumper* const p = static_cast<Bumper*>(pe);
         BumperHitCircle* const hc = p->m_pbumperhitcircle;
         if (hc == nullptr)
            break;
         // The bumper's own UpdateAnimation may have rewritten the ring buffer this frame (m_ringAnimate)
         const float ring = Lerp(va[0], vb[0], t);
         if (hc->m_bumperanim_ringAnimOffset != ring || p->m_ringAnimate)
         {
            hc->m_bumperanim_ringAnimOffset = ring;
            RebuildBumperRing(p);
         }
         const float* const skirt = Nearest(va, vb, t);
         if (fabsf(p->m_rotx - skirt[1]) > 1e-3f || fabsf(p->m_roty - skirt[2]) > 1e-3f)
            SetBumperSkirt(p, skirt[1], skirt[2]);
         p->m_ringAnimate = false;
         p->m_doSkirtAnimation = false;
         break;
      }
      case PC_HitTarget:
      {
         HitTarget* const p = static_cast<HitTarget*>(pe);
         const float offset = Lerp(va[0], vb[0], t);
         p->m_d.m_isDropped = Nearest(fa, fb, t);
         if (p->m_moveAnimationOffset != offset || p->m_moveAnimation)
         {
            p->m_moveAnimationOffset = offset;
            if (p->m_meshBuffer)
               p->UpdateTarget();
         }
         p->m_moveAnimation = false;
         p->m_hitEvent = false;
         break;
      }
      case PC_Trigger:
      {
         Trigger* const p = static_cast<Trigger*>(pe);
         p->m_animHeightOffset = Lerp(va[0], vb[0], t);
         p->m_doAnimation = false;
         p->m_hitEvent = false;
         p->m_unhitEvent = false;
         break;
      }
      case PC_Slingshot:
      {
         // A watcher's own LineSegSlingshot::Animate clears m_iframe once per frame (its m_TimeReset is never set
         // there, no ball hits it), before this runs again; so setting one segment is enough to draw the mesh.
         Surface* const p = static_cast<Surface*>(pe);
         if (Nearest(fa, fb, t))
         {
            bool drawn = false;
            for (const LineSegSlingshot* const ls : p->m_vlinesling)
               drawn |= ls->m_iframe || ls->m_doHitEvent;
            if (!drawn)
               p->m_vlinesling[0]->m_iframe = true;
         }
         else
            for (LineSegSlingshot* const ls : p->m_vlinesling)
            {
               ls->m_iframe = false;
               ls->m_doHitEvent = false;
               ls->m_EventTimeReset = 0;
            }
         break;
      }
      default: break;
      }
   }

   static void ApplyParts(Player* const player, const StateSnapshot& a, const StateSnapshot& b, const float t)
   {
      uint32_t index[PC_Count] = {};
      ForEachPart(player->m_ptable, [&](const PartClass c, IEditable* const pe)
      {
         const uint32_t i = index[c]++;
         const float* const va = kFloats[c] ? &a.values[c][(size_t)i * kFloats[c]] : nullptr;
         const float* const vb = kFloats[c] ? &b.values[c][(size_t)i * kFloats[c]] : nullptr;
         const bool fa = kHasFlag[c] && a.flags[c][i];
         const bool fb = kHasFlag[c] && b.flags[c][i];
         ApplyPart(c, pe, va, vb, fa, fb, t);
      });
   }

   // The non-legacy branch of the nudge shake in Player::UpdatePhysics, with this player's own strength and
   // gains; a watcher skips that code, it runs no physics
   static void ApplyNudge(Player* const player, const float dispX, const float dispY, const float nudgeX, const float nudgeY, const int gainX, const int gainY)
   {
      if (player->m_NudgeShake <= 0.0f)
         return;
#ifdef ENABLE_VR
      if (player->m_stereo3D == STEREO_VR)
      {
         RenderDevice* const rd = player->m_pin3d.m_pd3dPrimaryDevice;
         if (rd == nullptr)
            return;
         if (nudgeX != 0 || nudgeY != 0)
            rd->updateTableMatrix(player->m_NudgeShake * gainX * nudgeX, player->m_NudgeShake * -gainY * nudgeY, 0);
         else
            rd->updateTableMatrix(player->m_NudgeShake * dispX * 100, -player->m_NudgeShake * dispY * 100, 0);
         return;
      }
#endif
      player->SetScreenOffset(player->m_NudgeShake * dispX, -player->m_NudgeShake * dispY);
   }

   // ------------------------------------------------------------------------------------------------------
   // Match role: the saves and restores of a watch phase (CastcadeState.h)

   struct LightRt
   {
      float inPlayState, currentIntensity, finalLightState;
      double filamentTemperature;
      int duration;
      U32 timeNextBlink, timerDurationEndTime, blinkFrame;
      bool lockedByLS;
   };
   struct SeqRt
   {
      U32 timeNextUpdate;
      int blinkSequence;
      _tracer th1, th2, tt1, tt2;
      LightSeqQueue queue;
      SequencerState playAnimation;
      int tailLength, pauseValue, replayCount, updateRate;
      bool playInProgress, pauseInProgress;
   };
   struct PrimRt
   {
      float currentFrame, speed;
      bool doAnimation, endless, lockedByLS, inPlayState;
   };
   struct FlasherRt
   {
      bool lockedByLS, inPlayState;
   };
   struct ReelRt
   {
      DispReel::ReelInfo info[MAX_REELS];
      U32 timeNextUpdate;
   };

   static void SaveRuntime(Player* const player, TableRuntime& out);
   static void RestoreRuntime(Player* const player, const TableRuntime& in, const bool stampsFromNow);

   struct FlipperPh
   {
      float angularMomentum, angularAcceleration, angleSpeed, angleCur, curTorque, contactTorque, angleStart, angleEnd;
      short enableRotateEvent;
      bool direction, solState, isInContact, enabled, visible, lastHitFace;
   };
   struct GatePh
   {
      float anglespeed, angle, angleMin, angleMax;
      bool open, forcedMove, hitDirection;
   };
   struct SpinnerPh
   {
      float anglespeed, angle;
   };
   struct PlungerPh
   {
      float pos, speed, travelLimit, pullForce, reverseImpulse, fireSpeed, fireBounce, restPos, mech0, mech1, mech2, scatterVelocity, initialSpeed;
      int fireTimer, autoFireTimer, retractWaitLoop;
      bool strokeEventsArmed, addRetractMotion, retractMotion;
   };
   struct BumperPh
   {
      float ringAnimOffset, rotx, roty, skirtCounter;
      Vertex3Ds hitBallPosition;
      bool hitEvent, updateSkirt, doSkirtAnimation, ringDown, ringAnimate;
   };
   struct TargetPh
   {
      float moveAnimationOffset;
      U32 timeStamp;
      bool isDropped, moveAnimation, moveDown, hitEvent;
   };
   struct TriggerPh
   {
      float animHeightOffset;
      bool hitEvent, unhitEvent, doAnimation, moveDown;
   };
   struct SlingPh
   {
      unsigned int eventTimeReset;
      U32 timeReset;
      bool doHitEvent, iframe;
   };

   static void SavePhysics(Player* const player, OwnPhysics& out);
   static void RestorePhysics(Player* const player, const OwnPhysics& in);

   static void BeginWatch(Player* const player, OwnPlayer& own);
   static void HideOwn(Player* const player, OwnPlayer& own);
   static unsigned int EndWatch(Player* const player, OwnPlayer& own);
   static std::string LiveBalls(Player* const player);
};

struct TableRuntime
{
   std::vector<StateAccess::LightRt> lights;
   std::vector<StateAccess::SeqRt> seqs;
   std::vector<StateAccess::PrimRt> prims;
   std::vector<StateAccess::FlasherRt> flashers;
   std::vector<StateAccess::ReelRt> reels;
};

struct OwnPhysics
{
   std::vector<StateAccess::FlipperPh> flippers;
   std::vector<StateAccess::GatePh> gates;
   std::vector<StateAccess::SpinnerPh> spinners;
   std::vector<StateAccess::PlungerPh> plungers;
   std::vector<StateAccess::BumperPh> bumpers;
   std::vector<StateAccess::TargetPh> targets;
   std::vector<StateAccess::TriggerPh> triggers;
   std::vector<StateAccess::SlingPh> slings; // every segment of every slingshot wall, in order
};

struct OwnPlayer
{
   struct OwnBall
   {
      Ball* ball;
      unsigned int id;
      bool visible;
      Vertex3Ds pos, vel;
   };
   U64 elapsedUsec = 0; // physics clock minus the game's start: what the script's GameTime counts
   U32 timeMsec = 0, lastFrameTimeMsec = 0;
   std::vector<OwnBall> balls;
   Ball* active = nullptr;
   Ball* activeDebug = nullptr;
   Ball* activeBC = nullptr;
   unsigned int nextBallId = 0;
   AudioPlayer* audio = nullptr;
   bool hidden = false;
};

struct SavedMaterial
{
   Material* target;
   Material copy;
};

void StateAccess::SaveRuntime(Player* const player, TableRuntime& out)
{
   for (IEditable* const pe : player->m_ptable->m_vedit)
   {
      switch (pe->GetItemType())
      {
      case eItemLight:
      {
         const Light* const p = static_cast<Light*>(pe);
         out.lights.push_back(LightRt { p->m_inPlayState, p->m_currentIntensity, p->m_finalLightState, p->m_currentFilamentTemperature, p->m_duration, p->m_timenextblink,
            p->m_timerDurationEndTime, p->m_iblinkframe, p->m_lockedByLS });
         break;
      }
      case eItemLightSeq:
      {
         const LightSeq* const p = static_cast<LightSeq*>(pe);
         out.seqs.push_back(SeqRt { p->m_timeNextUpdate, p->m_blinkSequence, p->m_th1, p->m_th2, p->m_tt1, p->m_tt2, p->m_queue, p->m_playAnimation, p->m_tailLength,
            p->m_pauseValue, p->m_replayCount, p->m_updateRate, p->m_playInProgress, p->m_pauseInProgress });
         break;
      }
      case eItemPrimitive:
      {
         const Primitive* const p = static_cast<Primitive*>(pe);
         out.prims.push_back(PrimRt { p->m_currentFrame, p->m_speed, p->m_doAnimation, p->m_endless, p->m_lockedByLS, p->m_inPlayState });
         break;
      }
      case eItemFlasher:
      {
         const Flasher* const p = static_cast<Flasher*>(pe);
         out.flashers.push_back(FlasherRt { p->m_lockedByLS, p->m_inPlayState });
         break;
      }
      case eItemDispReel:
      {
         const DispReel* const p = static_cast<DispReel*>(pe);
         ReelRt r;
         memcpy(r.info, p->m_reelInfo, sizeof(r.info));
         r.timeNextUpdate = p->m_timeNextUpdate;
         out.reels.push_back(r);
         break;
      }
      default: break;
      }
   }
}

void StateAccess::RestoreRuntime(Player* const player, const TableRuntime& in, const bool stampsFromNow)
{
   const U32 now = player->m_time_msec;
   size_t il = 0, is = 0, ip = 0, ifl = 0, ir = 0;
   for (IEditable* const pe : player->m_ptable->m_vedit)
   {
      switch (pe->GetItemType())
      {
      case eItemLight:
      {
         if (il >= in.lights.size())
            break;
         const LightRt& s = in.lights[il++];
         Light* const p = static_cast<Light*>(pe);
         p->m_inPlayState = s.inPlayState;
         p->m_currentIntensity = s.currentIntensity;
         p->m_finalLightState = s.finalLightState;
         p->m_currentFilamentTemperature = s.filamentTemperature;
         p->m_duration = s.duration;
         p->m_timenextblink = s.timeNextBlink;
         p->m_timerDurationEndTime = s.timerDurationEndTime;
         // The pattern itself is a property: if the pattern now in place is shorter, start it again
         p->m_iblinkframe = s.blinkFrame < (U32)p->m_d.m_rgblinkpattern.length() ? s.blinkFrame : 0;
         p->m_lockedByLS = s.lockedByLS;
         if (stampsFromNow)
         {
            p->m_timenextblink = now + p->m_d.m_blinkinterval;
            p->m_timerDurationEndTime = now + s.duration;
         }
         break;
      }
      case eItemLightSeq:
      {
         if (is >= in.seqs.size())
            break;
         const SeqRt& s = in.seqs[is++];
         LightSeq* const p = static_cast<LightSeq*>(pe);
         p->m_timeNextUpdate = s.timeNextUpdate;
         p->m_blinkSequence = s.blinkSequence;
         p->m_th1 = s.th1;
         p->m_th2 = s.th2;
         p->m_tt1 = s.tt1;
         p->m_tt2 = s.tt2;
         p->m_queue = s.queue;
         p->m_playAnimation = s.playAnimation;
         p->m_tailLength = s.tailLength;
         p->m_pauseValue = s.pauseValue;
         p->m_replayCount = s.replayCount;
         p->m_updateRate = s.updateRate;
         p->m_playInProgress = s.playInProgress;
         p->m_pauseInProgress = s.pauseInProgress;
         if (stampsFromNow)
            p->m_timeNextUpdate = now + (U32)(std::max)(s.updateRate, 0);
         break;
      }
      case eItemPrimitive:
      {
         if (ip >= in.prims.size())
            break;
         const PrimRt& s = in.prims[ip++];
         Primitive* const p = static_cast<Primitive*>(pe);
         if (p->m_currentFrame != s.currentFrame && !p->m_mesh.m_animationFrames.empty() && !p->m_d.m_staticRendering)
            p->m_vertexBufferRegenerate = true; // Primitive::Render rebuilds the mesh of the frame put back
         p->m_currentFrame = s.currentFrame;
         p->m_speed = s.speed;
         p->m_doAnimation = s.doAnimation;
         p->m_endless = s.endless;
         p->m_lockedByLS = s.lockedByLS;
         p->m_inPlayState = s.inPlayState;
         break;
      }
      case eItemFlasher:
      {
         if (ifl >= in.flashers.size())
            break;
         const FlasherRt& s = in.flashers[ifl++];
         Flasher* const p = static_cast<Flasher*>(pe);
         p->m_lockedByLS = s.lockedByLS;
         p->m_inPlayState = s.inPlayState;
         break;
      }
      case eItemDispReel:
      {
         if (ir >= in.reels.size())
            break;
         const ReelRt& s = in.reels[ir++];
         DispReel* const p = static_cast<DispReel*>(pe);
         memcpy(p->m_reelInfo, s.info, sizeof(s.info));
         p->m_timeNextUpdate = stampsFromNow ? now + (U32)(std::max)(p->m_d.m_updateinterval, 0) : s.timeNextUpdate;
         break;
      }
      default: break;
      }
   }
}

void StateAccess::SavePhysics(Player* const player, OwnPhysics& out)
{
   ForEachPart(player->m_ptable, [&](const PartClass c, IEditable* const pe)
   {
      switch (c)
      {
      case PC_Flipper:
      {
         const Flipper* const p = static_cast<Flipper*>(pe);
         FlipperPh s = {};
         if (p->m_phitflipper)
         {
            const FlipperMoverObject& m = p->m_phitflipper->m_flipperMover;
            s = FlipperPh { m.m_angularMomentum, m.m_angularAcceleration, m.m_angleSpeed, m.m_angleCur, m.m_curTorque, m.m_contactTorque, m.m_angleStart, m.m_angleEnd,
               m.m_enableRotateEvent, m.m_direction, m.m_solState, m.m_isInContact, m.m_enabled, m.m_visible, m.m_lastHitFace };
         }
         out.flippers.push_back(s);
         break;
      }
      case PC_Gate:
      {
         const Gate* const p = static_cast<Gate*>(pe);
         GatePh s = {};
         if (p->m_phitgate)
         {
            const GateMoverObject& m = p->m_phitgate->m_gateMover;
            s = GatePh { m.m_anglespeed, m.m_angle, m.m_angleMin, m.m_angleMax, m.m_open, m.m_forcedMove, m.m_hitDirection };
         }
         out.gates.push_back(s);
         break;
      }
      case PC_Spinner:
      {
         const Spinner* const p = static_cast<Spinner*>(pe);
         SpinnerPh s = {};
         if (p->m_phitspinner)
            s = SpinnerPh { p->m_phitspinner->m_spinnerMover.m_anglespeed, p->m_phitspinner->m_spinnerMover.m_angle };
         out.spinners.push_back(s);
         break;
      }
      case PC_Plunger:
      {
         const Plunger* const p = static_cast<Plunger*>(pe);
         PlungerPh s = {};
         if (p->m_phitplunger)
         {
            const PlungerMoverObject& m = p->m_phitplunger->m_plungerMover;
            s = PlungerPh { m.m_pos, m.m_speed, m.m_travelLimit, m.m_pullForce, m.m_reverseImpulse, m.m_fireSpeed, m.m_fireBounce, m.m_restPos, m.m_mech0, m.m_mech1, m.m_mech2,
               m.m_scatterVelocity, m.m_initialSpeed, m.m_fireTimer, m.m_autoFireTimer, m.m_retractWaitLoop, m.m_strokeEventsArmed, m.m_addRetractMotion, m.m_retractMotion };
         }
         out.plungers.push_back(s);
         break;
      }
      case PC_Bumper:
      {
         const Bumper* const p = static_cast<Bumper*>(pe);
         BumperPh s = {};
         s.rotx = p->m_rotx;
         s.roty = p->m_roty;
         s.skirtCounter = p->m_skirtCounter;
         s.updateSkirt = p->m_updateSkirt;
         s.doSkirtAnimation = p->m_doSkirtAnimation;
         s.ringDown = p->m_ringDown;
         s.ringAnimate = p->m_ringAnimate;
         if (p->m_pbumperhitcircle)
         {
            s.ringAnimOffset = p->m_pbumperhitcircle->m_bumperanim_ringAnimOffset;
            s.hitBallPosition = p->m_pbumperhitcircle->m_bumperanim_hitBallPosition;
            s.hitEvent = p->m_pbumperhitcircle->m_bumperanim_hitEvent;
         }
         out.bumpers.push_back(s);
         break;
      }
      case PC_HitTarget:
      {
         const HitTarget* const p = static_cast<HitTarget*>(pe);
         out.targets.push_back(TargetPh { p->m_moveAnimationOffset, p->m_timeStamp, p->m_d.m_isDropped, p->m_moveAnimation, p->m_moveDown, p->m_hitEvent });
         break;
      }
      case PC_Trigger:
      {
         const Trigger* const p = static_cast<Trigger*>(pe);
         out.triggers.push_back(TriggerPh { p->m_animHeightOffset, p->m_hitEvent, p->m_unhitEvent, p->m_doAnimation, p->m_moveDown });
         break;
      }
      case PC_Slingshot:
      {
         const Surface* const p = static_cast<Surface*>(pe);
         for (const LineSegSlingshot* const ls : p->m_vlinesling)
            out.slings.push_back(SlingPh { ls->m_EventTimeReset, ls->m_TimeReset, ls->m_doHitEvent, ls->m_iframe });
         break;
      }
      default: break;
      }
   });
}

void StateAccess::RestorePhysics(Player* const player, const OwnPhysics& in)
{
   size_t i[PC_Count] = {};
   size_t sling = 0;
   ForEachPart(player->m_ptable, [&](const PartClass c, IEditable* const pe)
   {
      const size_t at = i[c]++;
      switch (c)
      {
      case PC_Flipper:
      {
         Flipper* const p = static_cast<Flipper*>(pe);
         if (at >= in.flippers.size() || !p->m_phitflipper)
            break;
         const FlipperPh& s = in.flippers[at];
         FlipperMoverObject& m = p->m_phitflipper->m_flipperMover;
         m.m_angularMomentum = s.angularMomentum;
         m.m_angularAcceleration = s.angularAcceleration;
         m.m_angleSpeed = s.angleSpeed;
         m.m_angleCur = s.angleCur;
         m.m_curTorque = s.curTorque;
         m.m_contactTorque = s.contactTorque;
         m.m_angleStart = s.angleStart;
         m.m_angleEnd = s.angleEnd;
         m.m_enableRotateEvent = s.enableRotateEvent;
         m.m_direction = s.direction;
         m.m_solState = s.solState;
         m.m_isInContact = s.isInContact;
         m.m_enabled = s.enabled;
         m.m_visible = s.visible;
         m.m_lastHitFace = s.lastHitFace;
         break;
      }
      case PC_Gate:
      {
         Gate* const p = static_cast<Gate*>(pe);
         if (at >= in.gates.size() || !p->m_phitgate)
            break;
         const GatePh& s = in.gates[at];
         GateMoverObject& m = p->m_phitgate->m_gateMover;
         m.m_anglespeed = s.anglespeed;
         m.m_angle = s.angle;
         m.m_angleMin = s.angleMin;
         m.m_angleMax = s.angleMax;
         m.m_open = s.open;
         m.m_forcedMove = s.forcedMove;
         m.m_hitDirection = s.hitDirection;
         break;
      }
      case PC_Spinner:
      {
         Spinner* const p = static_cast<Spinner*>(pe);
         if (at >= in.spinners.size() || !p->m_phitspinner)
            break;
         p->m_phitspinner->m_spinnerMover.m_anglespeed = in.spinners[at].anglespeed;
         p->m_phitspinner->m_spinnerMover.m_angle = in.spinners[at].angle;
         break;
      }
      case PC_Plunger:
      {
         Plunger* const p = static_cast<Plunger*>(pe);
         if (at >= in.plungers.size() || !p->m_phitplunger)
            break;
         const PlungerPh& s = in.plungers[at];
         PlungerMoverObject& m = p->m_phitplunger->m_plungerMover;
         m.m_pos = s.pos;
         m.m_speed = s.speed;
         m.m_travelLimit = s.travelLimit;
         m.m_pullForce = s.pullForce;
         m.m_reverseImpulse = s.reverseImpulse;
         m.m_fireSpeed = s.fireSpeed;
         m.m_fireBounce = s.fireBounce;
         m.m_restPos = s.restPos;
         m.m_mech0 = s.mech0;
         m.m_mech1 = s.mech1;
         m.m_mech2 = s.mech2;
         m.m_scatterVelocity = s.scatterVelocity;
         m.m_initialSpeed = s.initialSpeed;
         m.m_fireTimer = s.fireTimer;
         m.m_autoFireTimer = s.autoFireTimer;
         m.m_retractWaitLoop = s.retractWaitLoop;
         m.m_strokeEventsArmed = s.strokeEventsArmed;
         m.m_addRetractMotion = s.addRetractMotion;
         m.m_retractMotion = s.retractMotion;
         break;
      }
      case PC_Bumper:
      {
         Bumper* const p = static_cast<Bumper*>(pe);
         if (at >= in.bumpers.size())
            break;
         const BumperPh& s = in.bumpers[at];
         if (p->m_pbumperhitcircle)
         {
            p->m_pbumperhitcircle->m_bumperanim_ringAnimOffset = s.ringAnimOffset;
            RebuildBumperRing(p);
            if (fabsf(p->m_rotx - s.rotx) > 1e-6f || fabsf(p->m_roty - s.roty) > 1e-6f)
               SetBumperSkirt(p, s.rotx, s.roty); // rebuilds the skirt's vertex buffer
            p->m_pbumperhitcircle->m_bumperanim_hitBallPosition = s.hitBallPosition;
            p->m_pbumperhitcircle->m_bumperanim_hitEvent = s.hitEvent;
         }
         p->m_rotx = s.rotx;
         p->m_roty = s.roty;
         p->m_skirtCounter = s.skirtCounter;
         p->m_updateSkirt = s.updateSkirt;
         p->m_doSkirtAnimation = s.doSkirtAnimation;
         p->m_ringDown = s.ringDown;
         p->m_ringAnimate = s.ringAnimate;
         break;
      }
      case PC_HitTarget:
      {
         HitTarget* const p = static_cast<HitTarget*>(pe);
         if (at >= in.targets.size())
            break;
         const TargetPh& s = in.targets[at];
         const bool moved = p->m_moveAnimationOffset != s.moveAnimationOffset;
         p->m_moveAnimationOffset = s.moveAnimationOffset;
         p->m_timeStamp = s.timeStamp;
         p->m_d.m_isDropped = s.isDropped;
         p->m_moveAnimation = s.moveAnimation;
         p->m_moveDown = s.moveDown;
         p->m_hitEvent = s.hitEvent;
         if (moved && p->m_meshBuffer)
            p->UpdateTarget();
         break;
      }
      case PC_Trigger:
      {
         Trigger* const p = static_cast<Trigger*>(pe);
         if (at >= in.triggers.size())
            break;
         const TriggerPh& s = in.triggers[at];
         p->m_animHeightOffset = s.animHeightOffset;
         p->m_hitEvent = s.hitEvent;
         p->m_unhitEvent = s.unhitEvent;
         p->m_doAnimation = s.doAnimation;
         p->m_moveDown = s.moveDown;
         break;
      }
      case PC_Slingshot:
      {
         Surface* const p = static_cast<Surface*>(pe);
         for (LineSegSlingshot* const ls : p->m_vlinesling)
         {
            if (sling >= in.slings.size())
               break;
            const SlingPh& s = in.slings[sling++];
            ls->m_EventTimeReset = s.eventTimeReset;
            ls->m_TimeReset = s.timeReset;
            ls->m_doHitEvent = s.doHitEvent;
            ls->m_iframe = s.iframe;
         }
         break;
      }
      default: break;
      }
   });
}

void StateAccess::BeginWatch(Player* const player, OwnPlayer& own)
{
   own.elapsedUsec = player->m_curPhysicsFrameTime - player->m_StartTime_usec;
   own.timeMsec = player->m_time_msec;
   own.lastFrameTimeMsec = player->m_last_frame_time_msec;
   for (Ball* const ball : player->m_vball)
      own.balls.push_back(OwnPlayer::OwnBall { ball, ball->m_id, ball->m_visible, ball->m_d.m_pos, ball->m_d.m_vel });
   own.active = player->m_pactiveball;
   own.activeDebug = player->m_pactiveballDebug;
   own.activeBC = player->m_pactiveballBC;
   own.nextBallId = Ball::ballID;
   // As a watcher: its window is rarely the focused one, and the stream must go on being shown
   player->m_pause = false;
}

void StateAccess::HideOwn(Player* const player, OwnPlayer& own)
{
   if (own.hidden)
      return;
   own.hidden = true;
   for (const OwnPlayer::OwnBall& b : own.balls)
      b.ball->m_visible = false;
   // The music is paused already (Player::PauseMusic). Set aside, a replayed PlayMusic or EndMusic cannot end it.
   own.audio = player->m_audio;
   player->m_audio = nullptr;
}

unsigned int StateAccess::EndWatch(Player* const player, OwnPlayer& own)
{
   if (own.hidden)
   {
      for (const OwnPlayer::OwnBall& b : own.balls)
         if (std::find(player->m_vball.begin(), player->m_vball.end(), b.ball) != player->m_vball.end())
            b.ball->m_visible = b.visible;
      delete player->m_audio; // music the watched player's stream started here
      player->m_audio = own.audio;
      own.hidden = false;
      // The screen offset of the watched player's nudges, back to this table's own
      const int gainX = player->m_ptable->m_settings.LoadValueWithDefault(Settings::Player, "AnalogNudgeGainX"s, 100);
      const int gainY = player->m_ptable->m_settings.LoadValueWithDefault(Settings::Player, "AnalogNudgeGainY"s, 100);
      ApplyNudge(player, player->m_tableDisplacement.x, player->m_tableDisplacement.y, (float)player->m_curAccel[0].x * (float)(2.0 / JOYRANGE),
         (float)player->m_curAccel[0].y * (float)(2.0 / JOYRANGE), gainX, gainY);
   }
   const auto live = [player](Ball* const b) { return b && std::find(player->m_vball.begin(), player->m_vball.end(), b) != player->m_vball.end(); };
   player->m_pactiveball = live(own.active) ? own.active : nullptr;
   player->m_pactiveballDebug = live(own.activeDebug) ? own.activeDebug : (player->m_vball.empty() ? nullptr : player->m_vball.front());
   player->m_pactiveballBC = live(own.activeBC) ? own.activeBC : nullptr;
   // The watcher's balls took ids from the counter the script's own balls are numbered with
   Ball::ballID = own.nextBallId;

   // The game clock (the script's GameTime, the timers' next fire times, the blink and animation stamps of the parts)
   // ran on while the other player's table was shown, so that its lights faded and blinked. Back to where this
   // game stopped; m_noTimeCorrect then makes the next UpdatePhysics start from now, as after VPX's own pause.
   const U64 elapsedNow = player->m_curPhysicsFrameTime - player->m_StartTime_usec;
   player->m_StartTime_usec = player->m_curPhysicsFrameTime - own.elapsedUsec;
   player->m_time_msec = own.timeMsec;
   player->m_last_frame_time_msec = own.lastFrameTimeMsec;
   player->m_noTimeCorrect = true;
   return elapsedNow > own.elapsedUsec ? (unsigned int)((elapsedNow - own.elapsedUsec) / 1000) : 0u;
}

std::string StateAccess::LiveBalls(Player* const player)
{
   char buf[160];
   snprintf(buf, sizeof(buf), "t=%u n=%zu", player->m_time_msec, player->m_vball.size());
   std::string s = buf;
   for (const Ball* const b : player->m_vball)
   {
      snprintf(buf, sizeof(buf), " #%u %.1f %.1f %.1f %.2f %.2f", b->m_id, b->m_d.m_pos.x, b->m_d.m_pos.y, b->m_d.m_pos.z, b->m_d.m_vel.x, b->m_d.m_vel.y);
      s += buf;
   }
   return s;
}

std::shared_ptr<const TableRuntime> SaveTableRuntime(Player* const player)
{
   auto s = std::make_shared<TableRuntime>();
   StateAccess::SaveRuntime(player, *s);
   return s;
}

void RestoreTableRuntime(Player* const player, const TableRuntime& saved, const bool stampsFromNow)
{
   StateAccess::RestoreRuntime(player, saved, stampsFromNow);
}

std::shared_ptr<const OwnPhysics> SaveOwnPhysics(Player* const player)
{
   auto s = std::make_shared<OwnPhysics>();
   StateAccess::SavePhysics(player, *s);
   return s;
}

void RestoreOwnPhysics(Player* const player, const OwnPhysics& saved)
{
   StateAccess::RestorePhysics(player, saved);
}

std::shared_ptr<OwnPlayer> BeginWatchPhase(Player* const player)
{
   auto s = std::make_shared<OwnPlayer>();
   StateAccess::BeginWatch(player, *s);
   return s;
}

void HideOwnTable(Player* const player, OwnPlayer& own)
{
   StateAccess::HideOwn(player, own);
}

unsigned int EndWatchPhase(Player* const player, OwnPlayer& own)
{
   return StateAccess::EndWatch(player, own);
}

std::string DescribeOwnBalls(const OwnPlayer& own)
{
   std::string s = std::to_string(own.balls.size());
   char buf[160];
   for (const OwnPlayer::OwnBall& b : own.balls)
   {
      snprintf(buf, sizeof(buf), "%s #%u (%.1f, %.1f, %.1f) v (%.2f, %.2f, %.2f)", &b == &own.balls.front() ? ":" : ";", b.id, b.pos.x, b.pos.y, b.pos.z, b.vel.x, b.vel.y,
         b.vel.z);
      s += buf;
   }
   return s;
}

std::string DescribeLiveBalls(Player* const player)
{
   return StateAccess::LiveBalls(player);
}

std::shared_ptr<const SavedMaterial> SaveMaterial(Player* const player, const std::wstring& name)
{
   if (!player || !player->m_ptable || !g_pvp)
      return nullptr;
   char sz[MAX_PATH];
   WideCharToMultiByteNull(CP_ACP, 0, name.c_str(), -1, sz, MAX_PATH, nullptr, nullptr);
   Material* const mat = player->m_ptable->GetMaterial(sz);
   if (!mat || mat == &g_pvp->m_dummyMaterial)
      return nullptr;
   return std::make_shared<SavedMaterial>(SavedMaterial { mat, *mat });
}

void RestoreMaterial(Player* const player, const SavedMaterial& saved)
{
   // The table's materials live as long as the table: the pointer taken during this watch phase is still theirs
   if (player && player->m_ptable)
      *saved.target = saved.copy;
}

void CaptureState(Player* player, const std::unordered_map<const Ball*, uint32_t>& ballIds, ByteWriter& w)
{
   StateAccess::Capture(player, ballIds, w);
}

uint32_t SnapshotPhysicsMsec(const StateSnapshot& s)
{
   return s.timeMsec;
}

uint64_t PhysicsClockUsec(Player* const player)
{
   return StateAccess::PhysicsClock(player);
}

void ReelSoundNames(Player* const player, std::vector<std::wstring>& out)
{
   out.clear();
   if (!player || !player->m_ptable)
      return;
   for (IEditable* const pe : player->m_ptable->m_vedit)
   {
      if (pe->GetItemType() != eItemDispReel)
         continue;
      const string& s = static_cast<DispReel*>(pe)->m_d.m_szSound;
      if (s.empty() || s == "<None>") // DispReel::UpdateAnimation plays nothing for these
         continue;
      WCHAR w[MAXTOKEN];
      MultiByteToWideCharNull(CP_ACP, 0, s.c_str(), -1, w, MAXTOKEN);
      out.emplace_back(w);
   }
}

bool StopSoundWouldStop(Player* const player, const std::wstring& name)
{
   if (!player || !player->m_ptable || !g_pvp)
      return false;
   char sz[MAXSTRING];
   WideCharToMultiByteNull(CP_ACP, 0, name.c_str(), -1, sz, MAXSTRING, nullptr, nullptr);
   // As PinTable::StopSound: the first sound of that name, then the first copy of it
   for (PinSound* const pps : player->m_ptable->m_vsound)
      if (!lstrcmpi(pps->m_szName.c_str(), sz))
      {
         if (pps->IsWav())
         {
            DWORD status = 0;
            if (pps->m_pDSBuffer && SUCCEEDED(pps->m_pDSBuffer->GetStatus(&status)) && (status & DSBSTATUS_PLAYING))
               return true;
         }
         else if (pps->m_BASSstream && BASS_ChannelIsActive(pps->m_BASSstream) != BASS_ACTIVE_STOPPED)
            return true;
         break;
      }
   return g_pvp->m_ps.IsCopiedWavPlaying(sz);
}

int CountPlayingSounds(Player* const player)
{
   if (!player || !player->m_ptable || !g_pvp)
      return 0;
   int n = 0;
   for (PinSound* const pps : player->m_ptable->m_vsound)
   {
      if (pps->IsWav())
      {
         DWORD status = 0;
         if (pps->m_pDSBuffer && SUCCEEDED(pps->m_pDSBuffer->GetStatus(&status)) && (status & DSBSTATUS_PLAYING))
            ++n;
      }
      else if (pps->m_BASSstream && BASS_ChannelIsActive(pps->m_BASSstream) != BASS_ACTIVE_STOPPED)
         ++n;
   }
   return n + g_pvp->m_ps.CountPlayingCopies();
}

std::shared_ptr<const StateSnapshot> ParseState(ByteReader& r)
{
   if (r.u8() != kFormatVersion)
      return nullptr;
   auto s = std::make_shared<StateSnapshot>();
   s->timeMsec = r.u32();
   if (r.u8() != (uint8_t)PC_Count)
      return nullptr;
   for (int c = 0; c < PC_Count; ++c)
   {
      s->counts[c] = r.varu();
      if (s->counts[c] > kMaxParts)
         return nullptr;
   }
   for (int c = 0; c < PC_Count; ++c)
   {
      const size_t n = (size_t)s->counts[c] * kFloats[c];
      if (!r.ok() || n * sizeof(float) > r.remaining())
         return nullptr;
      s->values[c].resize(n);
      r.raw(s->values[c].data(), n * sizeof(float));
      for (const float v : s->values[c])
         if (!IsFinite(v))
            return nullptr;
   }
   for (int c = 0; c < PC_Count; ++c)
   {
      if (!kHasFlag[c])
         continue;
      const uint8_t* const bits = r.span((s->counts[c] + 7) / 8);
      if (bits == nullptr && s->counts[c] != 0)
         return nullptr;
      s->flags[c].resize(s->counts[c]);
      for (uint32_t i = 0; i < s->counts[c]; ++i)
         s->flags[c][i] = (bits[i / 8] >> (i % 8)) & 1;
   }
   for (float& v : s->nudge)
   {
      v = r.f32();
      if (!IsFinite(v))
         return nullptr;
   }
   const uint32_t ballCount = r.varu();
   if (!r.ok() || ballCount > kMaxBalls || ballCount * kMinBallBytes > r.remaining())
      return nullptr;
   s->balls.resize(ballCount);
   for (BallState& b : s->balls)
   {
      b.id = r.varu();
      float f[16];
      r.raw(f, sizeof(f));
      for (const float v : f)
         if (!IsFinite(v))
            return nullptr;
      b.pos = Vertex3Ds(f[0], f[1], f[2]);
      b.vel = Vertex3Ds(f[3], f[4], f[5]);
      memcpy(b.orientation, &f[6], sizeof(b.orientation));
      b.radius = f[15];
      b.lockedInKicker = (r.u8() & 1) != 0;
      if (!(b.radius > 0.f))
         return nullptr;
   }
   for (size_t i = 0; i < s->balls.size(); ++i)
      for (size_t j = i + 1; j < s->balls.size(); ++j)
         if (s->balls[i].id == s->balls[j].id)
            return nullptr;
   if (!r.ok())
      return nullptr;
   return s;
}

struct StateApplier::Impl
{
   Player* const player;
   std::unordered_map<uint32_t, Ball*> balls; // network id -> local ball, all created here
   std::unordered_map<uint32_t, Vertex3Ds> shownAt; // network id -> position written by the previous Apply
   float maxStep = 0.f;
   uint32_t localCounts[PC_Count] = {};
   bool countsKnown = false;
   int gainX = 100, gainY = 100;
   bool mismatchLogged = false;

   explicit Impl(Player* const p) : player(p) { }

   bool IsLive(const Ball* const ball) const
   {
      for (const Ball* const b : player->m_vball)
         if (b == ball)
            return true;
      return false;
   }

   void ApplyBalls(const StateSnapshot& a, const StateSnapshot& b, const float t)
   {
      // Only this applier creates or destroys balls on a watcher, but a pointer the Player has let go of must never
      // be written through
      for (auto it = balls.begin(); it != balls.end();)
      {
         if (IsLive(it->second))
            ++it;
         else
            it = balls.erase(it);
      }

      const int32_t dt = (int32_t)(b.timeMsec - a.timeMsec);
      const float dtMsec = dt > 0 ? (float)dt : 0.f;
      for (const BallState& sa : a.balls)
      {
         BallState s;
         bool teleport;
         InterpolateBall(sa, FindBallState(b.balls, sa.id), t, dtMsec, s, teleport);
         auto it = balls.find(sa.id);
         if (it == balls.end())
            // CreateBall puts the centre one radius above z; WriteBall sets the exact position right after
            it = balls.emplace(sa.id, player->CreateBall(s.pos.x, s.pos.y, s.pos.z - s.radius, s.vel.x, s.vel.y, s.vel.z, s.radius)).first;
         WriteBall(it->second, s);
         const auto at = shownAt.find(sa.id);
         if (at != shownAt.end())
         {
            const Vertex3Ds d(s.pos.x - at->second.x, s.pos.y - at->second.y, s.pos.z - at->second.z);
            maxStep = max(maxStep, sqrtf(d.LengthSquared()));
            at->second = s.pos;
         }
         else
            shownAt.emplace(sa.id, s.pos);
      }
      for (auto it = shownAt.begin(); it != shownAt.end();)
      {
         if (FindBallState(a.balls, it->first))
            ++it;
         else
            it = shownAt.erase(it);
      }

      for (auto it = balls.begin(); it != balls.end();)
      {
         if (FindBallState(a.balls, it->first) || FindBallState(b.balls, it->first))
            ++it;
         else
         {
            player->DestroyBall(it->second);
            it = balls.erase(it);
         }
      }
   }
};

StateApplier::StateApplier(Player* player)
   : m_impl(std::make_unique<Impl>(player))
{
}

StateApplier::~StateApplier() = default;

void StateApplier::Apply(const StateSnapshot& a, const StateSnapshot& b, float t)
{
   Impl& m = *m_impl;
   if (m.player == nullptr || m.player->m_ptable == nullptr)
      return;
   if (!m.countsKnown)
   {
      // The editables of a table being played do not change, and the settings are read once like Player does
      StateAccess::CountParts(m.player->m_ptable, m.localCounts);
      m.gainX = m.player->m_ptable->m_settings.LoadValueWithDefault(Settings::Player, "AnalogNudgeGainX"s, 100);
      m.gainY = m.player->m_ptable->m_settings.LoadValueWithDefault(Settings::Player, "AnalogNudgeGainY"s, 100);
      m.countsKnown = true;
   }
   // A snapshot of another table (or a corrupt one) is dropped whole, never half applied
   if (memcmp(a.counts, m.localCounts, sizeof(m.localCounts)) != 0 || memcmp(b.counts, m.localCounts, sizeof(m.localCounts)) != 0)
   {
      if (!m.mismatchLogged)
      {
         PLOGE << "Castcade state: snapshot does not match the local table, ignored";
         m.mismatchLogged = true;
      }
      return;
   }
   if (!(t >= 0.f)) // also NaN
      t = 0.f;
   else if (t > 1.f)
      t = 1.f;

   m.maxStep = 0.f;
   StateAccess::ApplyParts(m.player, a, b, t);
   StateAccess::ApplyNudge(m.player, Lerp(a.nudge[0], b.nudge[0], t), Lerp(a.nudge[1], b.nudge[1], t), Lerp(a.nudge[2], b.nudge[2], t),
      Lerp(a.nudge[3], b.nudge[3], t), m.gainX, m.gainY);
   m.ApplyBalls(a, b, t);
}

Ball* StateApplier::FindBall(uint32_t ballId) const
{
   const auto it = m_impl->balls.find(ballId);
   return it == m_impl->balls.end() ? nullptr : it->second;
}

float StateApplier::LastMaxBallStep() const
{
   return m_impl->maxStep;
}

void StateApplier::Reset()
{
   Impl& m = *m_impl;
   for (const auto& [id, ball] : m.balls)
      if (m.IsLive(ball))
         m.player->DestroyBall(ball);
   m.balls.clear();
   m.shownAt.clear();
   m.maxStep = 0.f;
}

}
