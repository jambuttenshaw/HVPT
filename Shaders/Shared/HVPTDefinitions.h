#pragma once


#ifdef __cplusplus
#include "HLSLTypeAliases.h"

namespace UE::HLSL
{
#endif


struct FHVPT_Reservoir
{
	// TODO: Data packing
	// TODO: Pack pixel index and num extra bounces together (into one int? would give max resolution of 16k x 16k and 15 max bounces)
	// TODO: Also infer bEmissionPath from LightId. LightId doens't need 32 bits of precision, could it be packed with another field?

	float RunningSum;	// The sum of all weights of all samples processed by this reservoir
	float M;			// The effective sample account of this reservoir - the number of candidate samples processed
	float P_y;			// The target function evaluation for the sample in this reservoir

	// Data to recover the path stored in the reservoir
	bool bEmissionPath;   // Whether this path is a scattering or emission path
	uint NumExtraBounces; // The number of bounces stored in the extra bounces buffer
	uint2 SampledPixel;	  // The pixel that this reservoir samples in (useful for retrieving where to read/write extra bounces from/to)
	float Depth;		  // Distance along ray that first scattering event occurs
	int LightId;		  // If scattering path, the light source that the path should sample
	float2 LightSample;   // RNG for the light sample to re-eval the same point on the light
};


// (omega, z) tuples describing subsequent bounces after the first scattering event
struct FHVPT_Bounce
{
	// TODO: Pack to float 3
	float3 Direction;
	float Distance;
};


#ifdef __cplusplus
}

using FHVPT_Reservoir = UE::HLSL::FHVPT_Reservoir;
using FHVPT_Bounce = UE::HLSL::FHVPT_Bounce;

#endif
