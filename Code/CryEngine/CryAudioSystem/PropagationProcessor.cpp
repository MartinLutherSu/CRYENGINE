// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#include "stdafx.h"
#include "PropagationProcessor.h"
#include "Common.h"
#include "Managers.h"
#include "ListenerManager.h"
#include "CVars.h"
#include "Object.h"
#include "System.h"
#include "ObjectRequestData.h"
#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/ISurfaceType.h>

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	#include "Debug.h"
	#include <CryRenderer/IRenderAuxGeom.h>
	#include <CryMath/Random.h>
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

namespace CryAudio
{
size_t constexpr g_numberLow = 7;
size_t constexpr g_numberMedium = 9;
size_t constexpr g_numberHigh = 11;
size_t constexpr g_numRaySamplePositionsLow = g_numberLow * g_numberLow;
size_t constexpr g_numRaySamplePositionsMedium = g_numberMedium * g_numberMedium;
size_t constexpr g_numRaySamplePositionsHigh = g_numberHigh * g_numberHigh;
size_t constexpr g_numConcurrentRaysLow = 1;
size_t constexpr g_numConcurrentRaysMedium = 2;
size_t constexpr g_numConcurrentRaysHigh = 4;

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
uint32 constexpr g_numIndices = 6;
vtx_idx constexpr g_auxIndices[g_numIndices] = { 2, 1, 0, 2, 3, 1 };
uint32 constexpr g_numPoints = 4;
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

float g_listenerHeadSize = 0.0f;
float g_listenerHeadSizeHalf = 0.0f;

enum class EOcclusionCollisionType : EnumFlagsType
{
	None    = 0,
	Static  = BIT(6), // a,
	Rigid   = BIT(7), // b,
	Water   = BIT(8), // c,
	Terrain = BIT(9), // d,
};
CRY_CREATE_ENUM_FLAG_OPERATORS(EOcclusionCollisionType);

struct SAudioRayOffset
{
	SAudioRayOffset(float const x_, float const z_)
		: x(x_)
		, z(z_)
	{}

	float const x;
	float const z;
};

using RaySamplePositions = std::vector<SAudioRayOffset>;
RaySamplePositions g_raySamplePositionsLow;
RaySamplePositions g_raySamplePositionsMedium;
RaySamplePositions g_raySamplePositionsHigh;

int CPropagationProcessor::s_occlusionRayFlags = 0;

///////////////////////////////////////////////////////////////////////////
void CRayInfo::Reset()
{
	totalSoundOcclusion = 0.0f;
	numHits = 0;
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	startPosition.zero();
	direction.zero();
	distanceToFirstObstacle = FLT_MAX;
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
}

bool CPropagationProcessor::s_bCanIssueRWIs = false;

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
size_t CPropagationProcessor::s_totalSyncPhysRays = 0;
size_t CPropagationProcessor::s_totalAsyncPhysRays = 0;
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

///////////////////////////////////////////////////////////////////////////
int CPropagationProcessor::OnObstructionTest(EventPhys const* pEvent)
{
	auto const pRWIResult = static_cast<EventPhysRWIResult const*>(pEvent);

	if (pRWIResult->iForeignData == PHYS_FOREIGN_ID_SOUND_OBSTRUCTION)
	{
		auto const pRayInfo = static_cast<CRayInfo*>(pRWIResult->pForeignData);

		if (pRayInfo != nullptr)
		{
			pRayInfo->numHits = std::min(static_cast<size_t>(pRWIResult->nHits) + 1, s_maxRayHits);
			SObjectRequestData<EObjectRequestType::ProcessPhysicsRay> requestData(pRayInfo->pObject, pRayInfo);
			CRequest const request(&requestData);
			g_system.PushRequest(request);
		}
		else
		{
			CRY_ASSERT(false);
		}
	}
	else
	{
		CRY_ASSERT(false);
	}

	return 1;
}

///////////////////////////////////////////////////////////////////////////
CPropagationProcessor::CPropagationProcessor(CObject& object)
	: m_lastQuerriedOcclusion(0.0f)
	, m_occlusion(0.0f)
	, m_remainingRays(0)
	, m_rayIndex(0)
	, m_object(object)
	, m_currentListenerDistance(0.0f)
	, m_occlusionRayOffset(0.1f)
	, m_occlusionType(EOcclusionType::None)
	, m_originalOcclusionType(EOcclusionType::None)
	, m_occlusionTypeWhenAdaptive(EOcclusionType::Low) //will be updated in the first Update
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	, m_rayDebugInfos(g_numConcurrentRaysHigh)
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
{
	UpdateOcclusionRayFlags();
	UpdateOcclusionPlanes();
	m_raysInfo.resize(g_numConcurrentRaysHigh);
	m_raysOcclusion.resize(g_numRaySamplePositionsHigh, 0.0f);
}

//////////////////////////////////////////////////////////////////////////
CPropagationProcessor::~CPropagationProcessor()
{
	stl::free_container(m_raysInfo);
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::Init()
{
	for (auto& rayInfo : m_raysInfo)
	{
		rayInfo.pObject = &m_object;
	}

	m_currentListenerDistance = g_listenerManager.GetActiveListenerTransformation().GetPosition().GetDistance(m_object.GetTransformation().GetPosition());

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	m_listenerOcclusionPlaneColor.set(cry_random<uint8>(0, 255), cry_random<uint8>(0, 255), cry_random<uint8>(0, 255), uint8(64));
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::UpdateOcclusionRayFlags()
{
	if ((g_cvars.m_occlusionCollisionTypes & EOcclusionCollisionType::Static) != 0)
	{
		s_occlusionRayFlags |= ent_static;
	}
	else
	{
		s_occlusionRayFlags &= ~ent_static;
	}

	if ((g_cvars.m_occlusionCollisionTypes & EOcclusionCollisionType::Rigid) != 0)
	{
		s_occlusionRayFlags |= (ent_sleeping_rigid | ent_rigid);
	}
	else
	{
		s_occlusionRayFlags &= ~(ent_sleeping_rigid | ent_rigid);
	}

	if ((g_cvars.m_occlusionCollisionTypes & EOcclusionCollisionType::Water) != 0)
	{
		s_occlusionRayFlags |= ent_water;
	}
	else
	{
		s_occlusionRayFlags &= ~ent_water;
	}

	if ((g_cvars.m_occlusionCollisionTypes & EOcclusionCollisionType::Terrain) != 0)
	{
		s_occlusionRayFlags |= ent_terrain;
	}
	else
	{
		s_occlusionRayFlags &= ~ent_terrain;
	}
}

///////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::Update()
{
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	if (g_cvars.m_objectsRayType > 0)
	{
		m_occlusionType = clamp_tpl<EOcclusionType>(static_cast<EOcclusionType>(g_cvars.m_objectsRayType), EOcclusionType::Ignore, EOcclusionType::High);
	}
	else
	{
		m_occlusionType = m_originalOcclusionType;
	}
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

	if (CanRunOcclusion())
	{
		if (m_currentListenerDistance < g_cvars.m_occlusionHighDistance)
		{
			m_occlusionTypeWhenAdaptive = EOcclusionType::High;
		}
		else if (m_currentListenerDistance < g_cvars.m_occlusionMediumDistance)
		{
			m_occlusionTypeWhenAdaptive = EOcclusionType::Medium;
		}
		else
		{
			m_occlusionTypeWhenAdaptive = EOcclusionType::Low;
		}

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
		UpdateOcclusionPlanes();
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

		RunObstructionQuery();
	}
	else
	{
		m_occlusion = 0.0f;
	}
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::SetOcclusionType(EOcclusionType const occlusionType)
{
	m_occlusionType = occlusionType;
	m_originalOcclusionType = occlusionType;
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::UpdateOcclusion()
{
	if (CanRunOcclusion())
	{
		Vec3 const& listenerPosition = g_listenerManager.GetActiveListenerTransformation().GetPosition();

		// First time run is synchronous and center ray only to get a quick initial value to start from.
		Vec3 const direction(m_object.GetTransformation().GetPosition() - listenerPosition);
		Vec3 directionNormalized(direction / m_currentListenerDistance);
		Vec3 const finalDirection(direction - (directionNormalized* m_occlusionRayOffset));

		CRayInfo& rayInfo = m_raysInfo[0];

		// We use "rwi_max_piercing" to allow audio rays to always pierce surfaces regardless of the "pierceability" attribute.
		// Note: The very first entry of rayInfo.hits (solid slot) is always empty.
		rayInfo.numHits = static_cast<size_t>(gEnv->pPhysicalWorld->RayWorldIntersection(
																						listenerPosition,
																						finalDirection,
																						s_occlusionRayFlags,
																						rwi_max_piercing,
																						rayInfo.hits,
																						static_cast<int>(s_maxRayHits),
																						nullptr,
																						0,
																						&rayInfo,
																						PHYS_FOREIGN_ID_SOUND_OBSTRUCTION));

		rayInfo.numHits = std::min(rayInfo.numHits + 1, s_maxRayHits);
		float finalOcclusion = 0.0f;

		if ((g_cvars.m_setFullOcclusionOnMaxHits > 0) && (rayInfo.numHits == s_maxRayHits))
		{
			finalOcclusion = 1.0f;
		}
		else if (rayInfo.numHits > 0)
		{
			ISurfaceTypeManager* const pSurfaceTypeManager = gEnv->p3DEngine->GetMaterialManager()->GetSurfaceTypeManager();
			CRY_ASSERT(rayInfo.numHits <= s_maxRayHits);
			bool const accumulate = g_cvars.m_accumulateOcclusion > 0;

			for (size_t i = 0; i < rayInfo.numHits; ++i)
			{
				float const distance = rayInfo.hits[i].dist;

				if (distance > 0.0f)
				{
					ISurfaceType* const pMat = pSurfaceTypeManager->GetSurfaceType(rayInfo.hits[i].surface_idx);

					if (pMat != nullptr)
					{
						ISurfaceType::SPhysicalParams const& physParams = pMat->GetPhyscalParams();

						if (accumulate)
						{
							finalOcclusion += physParams.sound_obstruction; // Not clamping b/w 0 and 1 for performance reasons.
						}
						else
						{
							finalOcclusion = std::max(finalOcclusion, physParams.sound_obstruction);
						}

						if (finalOcclusion >= 1.0f)
						{
							break;
						}
					}
				}
			}
		}

		m_occlusion = clamp_tpl(finalOcclusion, 0.0f, 1.0f);

		for (auto& rayOcclusion : m_raysOcclusion)
		{
			rayOcclusion = m_occlusion;
		}
	}
	else
	{
		m_occlusion = 0.0f;
		m_lastQuerriedOcclusion = 0.0f;
	}
}

//////////////////////////////////////////////////////////////////////////
bool CPropagationProcessor::CanRunOcclusion()
{
	bool canRun = false;

	if ((m_occlusionType != EOcclusionType::None) &&
	    (m_occlusionType != EOcclusionType::Ignore) &&
	    ((m_object.GetFlags() & EObjectFlags::Virtual) == 0) &&
	    s_bCanIssueRWIs)
	{
		m_currentListenerDistance = m_object.GetTransformation().GetPosition().GetDistance(g_listenerManager.GetActiveListenerTransformation().GetPosition());

		if ((m_currentListenerDistance > g_cvars.m_occlusionMinDistance) && (m_currentListenerDistance < g_cvars.m_occlusionMaxDistance))
		{
			canRun = true;
		}
	}

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	canRun ? m_object.SetFlag(EObjectFlags::CanRunOcclusion) : m_object.RemoveFlag(EObjectFlags::CanRunOcclusion);
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

	return canRun;
}

///////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::ProcessPhysicsRay(CRayInfo* const pRayInfo)
{
	CRY_ASSERT((0 <= pRayInfo->samplePosIndex) && (pRayInfo->samplePosIndex < g_numRaySamplePositionsHigh));

	float finalOcclusion = 0.0f;
	std::size_t numRealHits = 0;

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	float minDistance = FLT_MAX;
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

	if ((g_cvars.m_setFullOcclusionOnMaxHits > 0) && (pRayInfo->numHits == s_maxRayHits))
	{
		finalOcclusion = 1.0f;
		numRealHits = s_maxRayHits;
	}
	else if (pRayInfo->numHits > 0)
	{
		ISurfaceTypeManager* const pSurfaceTypeManager = gEnv->p3DEngine->GetMaterialManager()->GetSurfaceTypeManager();
		CRY_ASSERT(pRayInfo->numHits <= s_maxRayHits);
		bool const accumulate = g_cvars.m_accumulateOcclusion > 0;

		for (std::size_t i = 0; i < pRayInfo->numHits; ++i)
		{
			float const distance = pRayInfo->hits[i].dist;

			if (distance > 0.0f)
			{
				ISurfaceType* const pMat = pSurfaceTypeManager->GetSurfaceType(pRayInfo->hits[i].surface_idx);

				if (pMat != nullptr)
				{
					ISurfaceType::SPhysicalParams const& physParams = pMat->GetPhyscalParams();

					if (accumulate)
					{
						finalOcclusion += physParams.sound_obstruction; // Not clamping b/w 0 and 1 for performance reasons.
					}
					else
					{
						finalOcclusion = std::max(finalOcclusion, physParams.sound_obstruction);
					}

					++numRealHits;

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
					minDistance = std::min(minDistance, distance);
#endif      // INCLUDE_AUDIO_PRODUCTION_CODE

					if (finalOcclusion >= 1.0f)
					{
						break;
					}
				}
			}
		}
	}

	pRayInfo->numHits = numRealHits;
	pRayInfo->totalSoundOcclusion = clamp_tpl(finalOcclusion, 0.0f, 1.0f);

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	pRayInfo->distanceToFirstObstacle = minDistance;

	if (m_remainingRays == 0)
	{
		CryFatalError("Negative ref or ray count on audio object");
	}
#endif

	if (--m_remainingRays == 0)
	{
		ProcessObstructionOcclusion();
	}
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::ReleasePendingRays()
{
	if (m_remainingRays > 0)
	{
		m_remainingRays = 0;
	}
}

//////////////////////////////////////////////////////////////////////////
bool CPropagationProcessor::HasNewOcclusionValues()
{
	bool hasNewValues = false;

	if (fabs_tpl(m_lastQuerriedOcclusion - m_occlusion) > FloatEpsilon)
	{
		m_lastQuerriedOcclusion = m_occlusion;
		hasNewValues = true;
	}

	return hasNewValues;
}

///////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::ProcessObstructionOcclusion()
{
	m_occlusion = 0.0f;
	CRY_ASSERT_MESSAGE(m_currentListenerDistance > 0.0f, "Distance to Listener is 0 during %s", __FUNCTION__);

	size_t const numSamplePositions = GetNumSamplePositions();
	size_t const numConcurrentRays = GetNumConcurrentRays();

	if (numSamplePositions > 0 && numConcurrentRays > 0)
	{
		for (size_t i = 0; i < numConcurrentRays; ++i)
		{
			CRayInfo const& rayInfo = m_raysInfo[i];
			m_raysOcclusion[rayInfo.samplePosIndex] = rayInfo.totalSoundOcclusion;
		}

		// Calculate the new occlusion average.
		for (size_t i = 0; i < numSamplePositions; ++i)
		{
			m_occlusion += m_raysOcclusion[i];
		}

		m_occlusion = (m_occlusion / numSamplePositions);
	}

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	if ((m_object.GetFlags() & EObjectFlags::CanRunOcclusion) != 0) // only re-sample the rays about 10 times per second for a smoother debug drawing
	{
		for (size_t i = 0; i < g_numConcurrentRaysHigh; ++i)
		{
			CRayInfo const& rayInfo = m_raysInfo[i];
			SRayDebugInfo& rayDebugInfo = m_rayDebugInfos[i];

			rayDebugInfo.begin = rayInfo.startPosition;
			rayDebugInfo.end = rayInfo.startPosition + rayInfo.direction;

			if (rayDebugInfo.stableEnd.IsZeroFast())
			{
				// to be moved to the PropagationProcessor Reset method
				rayDebugInfo.stableEnd = rayDebugInfo.end;
			}
			else
			{
				rayDebugInfo.stableEnd += (rayDebugInfo.end - rayDebugInfo.stableEnd) * 0.1f;
			}

			rayDebugInfo.distanceToNearestObstacle = rayInfo.distanceToFirstObstacle;
			rayDebugInfo.numHits = rayInfo.numHits;
			rayDebugInfo.occlusionValue = rayInfo.totalSoundOcclusion;
		}
	}
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
}

///////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::CastObstructionRay(
	Vec3 const& origin,
	size_t const rayIndex,
	size_t const samplePosIndex,
	bool const bSynch)
{
	CRayInfo& rayInfo = m_raysInfo[rayIndex];
	rayInfo.samplePosIndex = samplePosIndex;
	Vec3 const direction(m_object.GetTransformation().GetPosition() - origin);
	Vec3 directionNormalized(direction);
	directionNormalized.Normalize();
	Vec3 const finalDirection(direction - (directionNormalized* m_occlusionRayOffset));

	// We use "rwi_max_piercing" to allow audio rays to always pierce surfaces regardless of the "pierceability" attribute.
	// Note: The very first entry of rayInfo.hits (solid slot) is always empty.
	int const numHits = gEnv->pPhysicalWorld->RayWorldIntersection(
		origin,
		finalDirection,
		s_occlusionRayFlags,
		bSynch ? rwi_max_piercing : rwi_max_piercing | rwi_queue,
		rayInfo.hits,
		static_cast<int>(s_maxRayHits),
		nullptr,
		0,
		&rayInfo,
		PHYS_FOREIGN_ID_SOUND_OBSTRUCTION);

	++m_remainingRays;

	if (bSynch)
	{
		rayInfo.numHits = std::min(static_cast<size_t>(numHits) + 1, s_maxRayHits);
		ProcessPhysicsRay(&rayInfo);
	}

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	rayInfo.startPosition = origin;
	rayInfo.direction = finalDirection;

	if (bSynch)
	{
		++s_totalSyncPhysRays;
	}
	else
	{
		++s_totalAsyncPhysRays;
	}
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
}

///////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::RunObstructionQuery()
{
	if (m_remainingRays == 0)
	{
		// Make the physics ray cast call synchronous or asynchronous depending on the distance to the listener.
		bool const bSynch = (m_currentListenerDistance <= g_cvars.m_occlusionMaxSyncDistance);

		Vec3 const& listenerPosition = g_listenerManager.GetActiveListenerTransformation().GetPosition();

		// TODO: this breaks if listener and object x and y coordinates are exactly the same.
		Vec3 const side((listenerPosition - m_object.GetTransformation().GetPosition()).Cross(Vec3Constants<float>::fVec3_OneZ).normalize());
		Vec3 const up((listenerPosition - m_object.GetTransformation().GetPosition()).Cross(side).normalize());

		switch (m_occlusionType)
		{
		case EOcclusionType::Adaptive:
			{
				switch (m_occlusionTypeWhenAdaptive)
				{
				case EOcclusionType::Low:
					ProcessLow(up, side, bSynch);
					break;
				case EOcclusionType::Medium:
					ProcessMedium(up, side, bSynch);
					break;
				case EOcclusionType::High:
					ProcessHigh(up, side, bSynch);
					break;
				default:
					CRY_ASSERT_MESSAGE(false, "Calculated Adaptive Occlusion Type invalid during %s", __FUNCTION__);
					break;
				}
			}
			break;
		case EOcclusionType::Low:
			ProcessLow(up, side, bSynch);
			break;
		case EOcclusionType::Medium:
			ProcessMedium(up, side, bSynch);
			break;
		case EOcclusionType::High:
			ProcessHigh(up, side, bSynch);
			break;
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::ProcessLow(
	Vec3 const& up,
	Vec3 const& side,
	bool const bSynch)
{
	for (size_t i = 0; i < g_numConcurrentRaysLow; ++i)
	{
		if (m_rayIndex >= g_numRaySamplePositionsLow)
		{
			m_rayIndex = 0;
		}

		Vec3 const origin(g_listenerManager.GetActiveListenerTransformation().GetPosition() + up* g_raySamplePositionsLow[m_rayIndex].z + side* g_raySamplePositionsLow[m_rayIndex].x);
		CastObstructionRay(origin, i, m_rayIndex, bSynch);
		++m_rayIndex;
	}
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::ProcessMedium(
	Vec3 const& up,
	Vec3 const& side,
	bool const bSynch)
{
	for (size_t i = 0; i < g_numConcurrentRaysMedium; ++i)
	{
		if (m_rayIndex >= g_numRaySamplePositionsMedium)
		{
			m_rayIndex = 0;
		}

		Vec3 const origin(g_listenerManager.GetActiveListenerTransformation().GetPosition() + up* g_raySamplePositionsMedium[m_rayIndex].z + side* g_raySamplePositionsMedium[m_rayIndex].x);
		CastObstructionRay(origin, i, m_rayIndex, bSynch);
		++m_rayIndex;
	}
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::ProcessHigh(
	Vec3 const& up,
	Vec3 const& side,
	bool const bSynch)
{
	for (size_t i = 0; i < g_numConcurrentRaysHigh; ++i)
	{
		if (m_rayIndex >= g_numRaySamplePositionsHigh)
		{
			m_rayIndex = 0;
		}

		Vec3 const origin(g_listenerManager.GetActiveListenerTransformation().GetPosition() + up* g_raySamplePositionsHigh[m_rayIndex].z + side* g_raySamplePositionsHigh[m_rayIndex].x);
		CastObstructionRay(origin, i, m_rayIndex, bSynch);
		++m_rayIndex;
	}
}

//////////////////////////////////////////////////////////////////////////
size_t CPropagationProcessor::GetNumConcurrentRays() const
{
	size_t numConcurrentRays = 0;

	switch (m_occlusionType)
	{
	case EOcclusionType::Adaptive:
		{
			switch (m_occlusionTypeWhenAdaptive)
			{
			case EOcclusionType::Low:
				numConcurrentRays = g_numConcurrentRaysLow;
				break;
			case EOcclusionType::Medium:
				numConcurrentRays = g_numConcurrentRaysMedium;
				break;
			case EOcclusionType::High:
				numConcurrentRays = g_numConcurrentRaysHigh;
				break;
			default:
				CRY_ASSERT_MESSAGE(false, "Calculated Adaptive Occlusion Type invalid during %s", __FUNCTION__);
				break;
			}
		}
		break;
	case EOcclusionType::Low:
		numConcurrentRays = g_numConcurrentRaysLow;
		break;
	case EOcclusionType::Medium:
		numConcurrentRays = g_numConcurrentRaysMedium;
		break;
	case EOcclusionType::High:
		numConcurrentRays = g_numConcurrentRaysHigh;
		break;
	}

	return numConcurrentRays;
}

//////////////////////////////////////////////////////////////////////////
size_t CPropagationProcessor::GetNumSamplePositions() const
{
	size_t numSamplePositions = 0;

	switch (m_occlusionType)
	{
	case EOcclusionType::Adaptive:
		{
			switch (m_occlusionTypeWhenAdaptive)
			{
			case EOcclusionType::Low:
				numSamplePositions = g_numRaySamplePositionsLow;
				break;
			case EOcclusionType::Medium:
				numSamplePositions = g_numRaySamplePositionsMedium;
				break;
			case EOcclusionType::High:
				numSamplePositions = g_numRaySamplePositionsHigh;
				break;
			default:
				CRY_ASSERT_MESSAGE(false, "Calculated Adaptive Occlusion Type invalid during %s", __FUNCTION__);
				break;
			}
		}
		break;
	case EOcclusionType::Low:
		numSamplePositions = g_numRaySamplePositionsLow;
		break;
	case EOcclusionType::Medium:
		numSamplePositions = g_numRaySamplePositionsMedium;
		break;
	case EOcclusionType::High:
		numSamplePositions = g_numRaySamplePositionsHigh;
		break;
	}

	return numSamplePositions;
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::UpdateOcclusionPlanes()
{
	if (g_listenerHeadSize != g_cvars.m_listenerOcclusionPlaneSize)
	{
		g_listenerHeadSize = g_cvars.m_listenerOcclusionPlaneSize;
		g_listenerHeadSizeHalf = g_listenerHeadSize * 0.5f;

		g_raySamplePositionsLow.clear();
		g_raySamplePositionsMedium.clear();
		g_raySamplePositionsHigh.clear();

		// Low
		g_raySamplePositionsLow.reserve(g_numRaySamplePositionsLow);
		float step = g_listenerHeadSize / (g_numberLow - 1);

		for (size_t i = 0; i < g_numberLow; ++i)
		{
			float const z = g_listenerHeadSizeHalf - i * step;

			for (size_t j = 0; j < g_numberLow; ++j)
			{
				g_raySamplePositionsLow.emplace_back(-g_listenerHeadSizeHalf + j * step, z);
			}
		}

		// Medium
		g_raySamplePositionsMedium.reserve(g_numRaySamplePositionsMedium);
		step = g_listenerHeadSize / (g_numberMedium - 1);

		for (size_t i = 0; i < g_numberMedium; ++i)
		{
			float const z = g_listenerHeadSizeHalf - i * step;

			for (size_t j = 0; j < g_numberMedium; ++j)
			{
				g_raySamplePositionsMedium.emplace_back(-g_listenerHeadSizeHalf + j * step, z);
			}
		}

		// High
		g_raySamplePositionsHigh.reserve(g_numRaySamplePositionsHigh);
		step = g_listenerHeadSize / (g_numberHigh - 1);

		for (size_t i = 0; i < g_numberHigh; ++i)
		{
			float const z = g_listenerHeadSizeHalf - i * step;

			for (size_t j = 0; j < g_numberHigh; ++j)
			{
				g_raySamplePositionsHigh.emplace_back(-g_listenerHeadSizeHalf + j * step, z);
			}
		}
	}
}

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::DrawDebugInfo(IRenderAuxGeom& auxGeom)
{
	if ((m_object.GetFlags() & EObjectFlags::CanRunOcclusion) != 0)
	{
		if ((g_cvars.m_drawDebug & Debug::EDrawFilter::OcclusionRays) != 0)
		{
			size_t const numConcurrentRays = GetNumConcurrentRays();

			for (size_t i = 0; i < numConcurrentRays; ++i)
			{
				DrawRay(auxGeom, i);
			}
		}

		if ((g_cvars.m_drawDebug & Debug::EDrawFilter::ListenerOcclusionPlane) != 0)
		{
			SAuxGeomRenderFlags const previousRenderFlags = auxGeom.GetRenderFlags();
			SAuxGeomRenderFlags newRenderFlags;
			newRenderFlags.SetDepthTestFlag(e_DepthTestOff);
			newRenderFlags.SetAlphaBlendMode(e_AlphaBlended);
			newRenderFlags.SetCullMode(e_CullModeNone);
			auxGeom.SetRenderFlags(newRenderFlags);

			Vec3 const& listenerPosition = g_listenerManager.GetActiveListenerTransformation().GetPosition();

			// TODO: this breaks if listener and object x and y coordinates are exactly the same.
			Vec3 const side((listenerPosition - m_object.GetTransformation().GetPosition()).Cross(Vec3Constants<float>::fVec3_OneZ).normalize());
			Vec3 const up((listenerPosition - m_object.GetTransformation().GetPosition()).Cross(side).normalize());

			Vec3 const quadVertices[g_numPoints] =
			{
				Vec3(listenerPosition + up * g_listenerHeadSizeHalf + side * g_listenerHeadSizeHalf),
				Vec3(listenerPosition + up * -g_listenerHeadSizeHalf + side * g_listenerHeadSizeHalf),
				Vec3(listenerPosition + up * g_listenerHeadSizeHalf + side * -g_listenerHeadSizeHalf),
				Vec3(listenerPosition + up * -g_listenerHeadSizeHalf + side * -g_listenerHeadSizeHalf) };

			auxGeom.DrawTriangles(quadVertices, g_numPoints, g_auxIndices, g_numIndices, m_listenerOcclusionPlaneColor);
			auxGeom.SetRenderFlags(previousRenderFlags);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::DrawRay(IRenderAuxGeom& auxGeom, size_t const rayIndex) const
{
	SAuxGeomRenderFlags const previousRenderFlags = auxGeom.GetRenderFlags();
	SAuxGeomRenderFlags newRenderFlags(e_Def3DPublicRenderflags);
	newRenderFlags.SetCullMode(e_CullModeNone);

	bool const isRayObstructed = (m_rayDebugInfos[rayIndex].numHits > 0);
	Vec3 const rayEnd = isRayObstructed ?
	                    m_rayDebugInfos[rayIndex].begin + (m_rayDebugInfos[rayIndex].end - m_rayDebugInfos[rayIndex].begin).GetNormalized() * m_rayDebugInfos[rayIndex].distanceToNearestObstacle :
	                    m_rayDebugInfos[rayIndex].end; // Only draw the ray to the first collision point.

	ColorF const& rayColor = isRayObstructed ? Debug::s_rayColorObstructed : Debug::s_rayColorFree;

	auxGeom.SetRenderFlags(newRenderFlags);

	if (isRayObstructed)
	{
		// Mark the nearest collision with a small sphere.
		auxGeom.DrawSphere(rayEnd, Debug::s_rayRadiusCollisionSphere, Debug::s_rayColorCollisionSphere);
	}

	auxGeom.DrawLine(m_rayDebugInfos[rayIndex].begin, rayColor, rayEnd, rayColor, 1.0f);
	auxGeom.SetRenderFlags(previousRenderFlags);
}

///////////////////////////////////////////////////////////////////////////
void CPropagationProcessor::ResetRayData()
{
	if (m_occlusionType != EOcclusionType::None && m_occlusionType != EOcclusionType::Ignore)
	{
		for (size_t i = 0; i < g_numConcurrentRaysHigh; ++i)
		{
			m_raysInfo[i].Reset();
			m_rayDebugInfos[i] = SRayDebugInfo();
		}
	}
}
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
}      // namespace CryAudio
