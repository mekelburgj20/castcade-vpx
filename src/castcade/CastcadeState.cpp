// license:GPLv3+

#include "stdafx.h"
#include "CastcadeState.h"

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
};

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
