// license:GPLv3+

#include "stdafx.h"
#include "CastcadeState.h"

// Placeholder: captures and applies nothing yet. The real implementation replaces this file.

namespace castcade
{

struct StateSnapshot
{
};

void CaptureState(Player* player, const std::unordered_map<const Ball*, uint32_t>& ballIds, ByteWriter& w)
{
}

std::shared_ptr<const StateSnapshot> ParseState(ByteReader& r)
{
   return std::make_shared<const StateSnapshot>();
}

struct StateApplier::Impl
{
};

StateApplier::StateApplier(Player* player)
   : m_impl(std::make_unique<Impl>())
{
}

StateApplier::~StateApplier() = default;

void StateApplier::Apply(const StateSnapshot& a, const StateSnapshot& b, float t)
{
}

Ball* StateApplier::FindBall(uint32_t ballId) const
{
   return nullptr;
}

void StateApplier::Reset()
{
}

}
