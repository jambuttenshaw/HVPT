#include "Helpers.h"

#include "ShaderParameterStruct.h"
#include "RenderGraph.h"


/*-----------------------------------------------------
	This is based on GPUSort.h/cpp implemented in Unreal Engine
	However, there is two critical differences between the algorithm implemented there
	and the algorithm required in HVPT's ReSTIR implementation.
	1.	I only require sorting an array of keys, rather than two linked arrays of key-value pairs
	2.	This sort should be GPU-driven - i.e., the number of items to sort is not known to the CPU
		but instead determined by prior GPU work.
-----------------------------------------------------*/

// --- Global State --- //

#define GPUSORT_BITCOUNT 32
#define RADIX_BITS 4
#define DIGIT_COUNT (1 << RADIX_BITS)
#define KEYS_PER_LOOP 8
#define THREAD_COUNT 128
#define TILE_SIZE (THREAD_COUNT * KEYS_PER_LOOP)
#define MAX_GROUP_COUNT 64
#define MAX_PASS_COUNT (32 / RADIX_BITS)


void SetRadixSortShaderCompilerEnvironment(FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("RADIX_BITS"), RADIX_BITS);
	OutEnvironment.SetDefine(TEXT("THREAD_COUNT"), THREAD_COUNT);
	OutEnvironment.SetDefine(TEXT("KEYS_PER_LOOP"), KEYS_PER_LOOP);
	OutEnvironment.SetDefine(TEXT("MAX_GROUP_COUNT"), MAX_GROUP_COUNT);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}

// --- Required Structures --- //

// This is populated on the GPU prior to the sort beginning
struct FRadixSortParameters
{
	uint32 TilesPerGroup;
	uint32 ExtraTileCount;
	uint32 ExtraKeyCount;
	uint32 GroupCount;
};


// --- Kernels --- //

class FHVPT_RadixSortPopulateParametersCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_RadixSortPopulateParametersCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_RadixSortPopulateParametersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(RWStructuredBuffer<uint>, Counter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FRadixSortParameters>, RWRadixSortParameterBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("RADIX_SORT_POPULATE_PARAMETERS"), 1);
		SetRadixSortShaderCompilerEnvironment(OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHVPT_RadixSortPopulateParametersCS, "/Plugin/HVPT/Private/Utils/RadixSort.usf", "HVPT_RadixSortPopulateParametersCS", SF_Compute)

class FHVPT_RadixSortClearOffsetsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_RadixSortClearOffsetsCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_RadixSortClearOffsetsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutOffsets)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("RADIX_SORT_CLEAR_OFFSETS"), 1);
		SetRadixSortShaderCompilerEnvironment(OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHVPT_RadixSortClearOffsetsCS, "/Plugin/HVPT/Private/Utils/RadixSort.usf", "HVPT_RadixSortClearOffsetsCS", SF_Compute)

class FHVPT_RadixSortUpsweepCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_RadixSortUpsweepCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_RadixSortUpsweepCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, RadixShift)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRadixSortParameters>, RadixSortParameterBuffer)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InKeys)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutOffsets)

		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("RADIX_SORT_UPSWEEP"), 1);
		SetRadixSortShaderCompilerEnvironment(OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHVPT_RadixSortUpsweepCS, "/Plugin/HVPT/Private/Utils/RadixSort.usf", "HVPT_RadixSortUpsweepCS", SF_Compute)

class FHVPT_RadixSortSpineCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_RadixSortSpineCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_RadixSortSpineCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InOffsets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutOffsets)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("RADIX_SORT_SPINE"), 1);
		SetRadixSortShaderCompilerEnvironment(OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHVPT_RadixSortSpineCS, "/Plugin/HVPT/Private/Utils/RadixSort.usf", "HVPT_RadixSortSpineCS", SF_Compute)

class FHVPT_RadixSortDownsweepCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHVPT_RadixSortDownsweepCS);
	SHADER_USE_PARAMETER_STRUCT(FHVPT_RadixSortDownsweepCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, RadixShift)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRadixSortParameters>, RadixSortParameterBuffer)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InKeys)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InOffsets)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutKeys)

		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("RADIX_SORT_DOWNSWEEP"), 1);
		SetRadixSortShaderCompilerEnvironment(OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHVPT_RadixSortDownsweepCS, "/Plugin/HVPT/Private/Utils/RadixSort.usf", "HVPT_RadixSortDownsweepCS", SF_Compute)


// --- Public interface --- //

uint32 HVPT::Private::SortBufferIndirect(
	FRDGBuilder& GraphBuilder, 
	TArrayView<FRDGBufferRef> InBuffers, 
	int32 BufferIndex, 
	FRDGBufferRef Counter, 
	uint32 CounterOffset, 
	uint32 KeyMask, 
	ERHIFeatureLevel::Type FeatureLevel)
{
	check(InBuffers.Num() >= 2); // Only element 0 and 1 will ever be used, but it's not invalid to have a larger array
	check(InBuffers[0] && InBuffers[1]);
	check(BufferIndex >= 0 && BufferIndex < 2);

	auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FHVPT_RadixSortPopulateParametersCS> PopulateParametersCS(ShaderMap);
	TShaderMapRef<FHVPT_RadixSortClearOffsetsCS> ClearOffsetsCS(ShaderMap);
	TShaderMapRef<FHVPT_RadixSortUpsweepCS> UpsweepCS(ShaderMap);
	TShaderMapRef<FHVPT_RadixSortSpineCS> SpineCS(ShaderMap);
	TShaderMapRef<FHVPT_RadixSortDownsweepCS> DownsweepCS(ShaderMap);

	// Create parameter and indirect args buffers
	FRDGBufferRef ParameterBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FRadixSortParameters), 1), TEXT("HVPT.RadixSort.Parameters")
	);
	FRDGBufferRef IndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("HVPT.RadixSort.IndirectArgs"));
	{
		FHVPT_RadixSortPopulateParametersCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RadixSortPopulateParametersCS::FParameters>();
		PassParameters->Counter = GraphBuilder.CreateSRV(FRDGBufferSRVDesc{ Counter, static_cast<uint32>(CounterOffset * sizeof(uint32)), 1 });
		PassParameters->RWRadixSortParameterBuffer = GraphBuilder.CreateUAV(ParameterBuffer);
		PassParameters->RWIndirectArgs = GraphBuilder.CreateUAV(IndirectArgs);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupParameters"),
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			PopulateParametersCS,
			PassParameters,
			FIntVector(1, 1, 1)
		);
	}

	// Allocate transients for offsets
	TStaticArray<FRDGBufferRef, 2> OffsetBuffers;
	auto OffsetBufferDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), DIGIT_COUNT * MAX_GROUP_COUNT);
	for (uint32 Buffer = 0; Buffer < 2; Buffer++)
	{
		OffsetBuffers[Buffer] = GraphBuilder.CreateBuffer(OffsetBufferDesc, Buffer == 0 ? TEXT("HVPT.RadixSort.OffsetBuffer0") : TEXT("HVPT.RadixSort.OffsetBuffer1"));
	}

	// Determine how many passes are required.
	constexpr int32 BitCount = GPUSORT_BITCOUNT;
	constexpr int32 PassCount = BitCount / RADIX_BITS;

	// Execute sort passes
	uint32 RadixShift = 0;
	uint32 PassBits = DIGIT_COUNT - 1;
	for (int32 PassIndex = 0; PassIndex < PassCount; PassIndex++)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "RadixSort(Pass=%d)", PassIndex);

		if ((PassBits & KeyMask) != 0)
		{
			// Step 1: Clear offsets
			{
				FHVPT_RadixSortClearOffsetsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RadixSortClearOffsetsCS::FParameters>();
				PassParameters->OutOffsets = GraphBuilder.CreateUAV(OffsetBuffers[0], PF_R32_UINT);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ClearOffsets"),
					ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
					ClearOffsetsCS,
					PassParameters,
					FIntVector{ 1, 1, 1}
				);
			}
			
			// Step 2: Upsweep
			{
				FHVPT_RadixSortUpsweepCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RadixSortUpsweepCS::FParameters>();
				PassParameters->RadixShift = RadixShift;
				PassParameters->RadixSortParameterBuffer = GraphBuilder.CreateSRV(ParameterBuffer);

				PassParameters->InKeys = GraphBuilder.CreateSRV(InBuffers[BufferIndex], PF_R32_UINT);
				PassParameters->OutOffsets = GraphBuilder.CreateUAV(OffsetBuffers[0], PF_R32_UINT);

				PassParameters->IndirectArgs = IndirectArgs;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Upsweep"),
					PassParameters,
					ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
					[PassParameters, UpsweepCS, IndirectArgs](FRHICommandList& RHICmdList)
					{
						FComputeShaderUtils::DispatchIndirect(
							RHICmdList,
							UpsweepCS,
							*PassParameters,
							IndirectArgs,
							0
						);
					});
			}

			// Step 3: Spine
			{
				FHVPT_RadixSortSpineCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RadixSortSpineCS::FParameters>();
				PassParameters->InOffsets = GraphBuilder.CreateSRV(OffsetBuffers[0], PF_R32_UINT);
				PassParameters->OutOffsets = GraphBuilder.CreateUAV(OffsetBuffers[1], PF_R32_UINT);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("Spine"),
					ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
					SpineCS,
					PassParameters,
					FIntVector{1, 1, 1}
				);
			}

			// Step 4: Downsweep
			{
				FHVPT_RadixSortDownsweepCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHVPT_RadixSortDownsweepCS::FParameters>();
				PassParameters->RadixShift = RadixShift;
				PassParameters->RadixSortParameterBuffer = GraphBuilder.CreateSRV(ParameterBuffer);

				PassParameters->InKeys = GraphBuilder.CreateSRV(InBuffers[BufferIndex], PF_R32_UINT);
				PassParameters->InOffsets = GraphBuilder.CreateSRV(OffsetBuffers[1], PF_R32_UINT);
				PassParameters->OutKeys = GraphBuilder.CreateUAV(InBuffers[BufferIndex ^ 0x1], PF_R32_UINT);

				PassParameters->IndirectArgs = IndirectArgs;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Downsweep"),
					PassParameters,
					ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
					[PassParameters, DownsweepCS, IndirectArgs](FRHICommandList& RHICmdList)
					{
						FComputeShaderUtils::DispatchIndirect(
							RHICmdList,
							DownsweepCS,	
							*PassParameters,
							IndirectArgs,
							0
						);
					});
			}
			
			BufferIndex ^= 0x1;
		}

		// Update the radix shift for the next pass
		RadixShift += RADIX_BITS;
		PassBits <<= RADIX_BITS;
	}

	// Return the buffer containing the sorted results
	return BufferIndex;
}
