// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include "Environment.h"
#include <PoolObject.h>
#include <CryAudio/IAudioInterfacesCommonData.h>

namespace CryAudio
{
namespace Impl
{
namespace Fmod
{
class CEnvironmentParameter final : public CEnvironment, public CPoolObject<CEnvironmentParameter, stl::PSyncNone>
{
public:

	CEnvironmentParameter() = delete;
	CEnvironmentParameter(CEnvironmentParameter const&) = delete;
	CEnvironmentParameter(CEnvironmentParameter&&) = delete;
	CEnvironmentParameter& operator=(CEnvironmentParameter const&) = delete;
	CEnvironmentParameter& operator=(CEnvironmentParameter&&) = delete;

	explicit CEnvironmentParameter(
		uint32 const id,
		float const multiplier,
		float const shift)
		: CEnvironment(EEnvironmentType::Parameter)
		, m_id(id)
		, m_multiplier(multiplier)
		, m_shift(shift)
	{}

	virtual ~CEnvironmentParameter() override = default;

	uint32 GetId() const              { return m_id; }
	float  GetValueMultiplier() const { return m_multiplier; }
	float  GetValueShift() const      { return m_shift; }

private:

	uint32 const m_id;
	float const  m_multiplier;
	float const  m_shift;
};
} // namespace Fmod
} // namespace Impl
} // namespace CryAudio
