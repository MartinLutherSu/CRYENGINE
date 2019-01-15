// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include "Common.h"

#include <IObject.h>
#include <CryAudio/IAudioInterfacesCommonData.h>
#include <cri_atom_ex.h>

namespace CryAudio
{
namespace Impl
{
namespace Adx2
{
class CEvent;
class CTrigger;
class CParameter;
class CSwitchState;

enum class EObjectFlags : EnumFlagsType
{
	None                    = 0,
	MovingOrDecaying        = BIT(0),
	TrackAbsoluteVelocity   = BIT(1),
	TrackVelocityForDoppler = BIT(2),
	UpdateVirtualStates     = BIT(3),
	IsVirtual               = BIT(4),
};
CRY_CREATE_ENUM_FLAG_OPERATORS(EObjectFlags);

class CBaseObject : public IObject
{
public:

	CBaseObject(CBaseObject const&) = delete;
	CBaseObject(CBaseObject&&) = delete;
	CBaseObject& operator=(CBaseObject const&) = delete;
	CBaseObject& operator=(CBaseObject&&) = delete;

	virtual ~CBaseObject() override;

	// CryAudio::Impl::IObject
	virtual void                   Update(float const deltaTime) override;
	virtual void                   SetTransformation(CTransformation const& transformation) override {}
	virtual CTransformation const& GetTransformation() const override                                { return CTransformation::GetEmptyObject(); }
	virtual void                   StopAllTriggers() override;
	virtual ERequestStatus         SetName(char const* const szName) override;
	virtual void                   ToggleFunctionality(EObjectFunctionality const type, bool const enable) override {}

	// Below data is only used when INCLUDE_ADX2_IMPL_PRODUCTION_CODE is defined!
	virtual void DrawDebugInfo(IRenderAuxGeom& auxGeom, float const posX, float posY, char const* const szTextFilter) override {}
	// ~CryAudio::Impl::IObject

	bool              StartEvent(CTrigger const* const pTrigger, CEvent* const pEvent);
	void              StopEvent(uint32 const triggerId);
	void              PauseEvent(uint32 const triggerId);
	void              ResumeEvent(uint32 const triggerId);
	void              MutePlayer(CriBool const shouldPause);
	void              PausePlayer(CriBool const shouldPause);

	CriAtomExPlayerHn GetPlayer() const { return m_pPlayer; }

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	char const* GetName() const { return m_name.c_str(); }
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

protected:

	CBaseObject();

	void AddEvent(CEvent* const pEvent);
	void UpdateVelocityTracking();

	EObjectFlags        m_flags;
	S3DAttributes       m_3dAttributes;
	CriAtomEx3dSourceHn m_p3dSource;
	CriAtomExPlayerHn   m_pPlayer;
	Events              m_events;

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	float m_absoluteVelocity;
	float m_absoluteVelocityNormalized;
	CryFixedStringT<MaxObjectNameLength> m_name;
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
};
} // namespace Adx2
} // namespace Impl
} // namespace CryAudio
