#include "SurfaceDecayPass.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"

class FSurfaceDecayCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfaceDecayCS);
	SHADER_USE_PARAMETER_STRUCT(FSurfaceDecayCS, FGlobalShader);

	static constexpr int32 ThreadGroupSize = 8;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, Accumulation)
		SHADER_PARAMETER(FIntPoint, AccumulationSize)
		SHADER_PARAMETER(float, HalfLivesElapsed)
		SHADER_PARAMETER(float, SaturationConstant)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSurfaceDecayCS, "/Plugin/RacetrackSurface/Private/SurfaceDecay.usf", "MainCS", SF_Compute);

void RacetrackSurfaceDecayPass::Enqueue(
	FTextureRenderTargetResource* Target,
	float HalfLivesElapsed,
	float SaturationConstant)
{
	if (Target == nullptr)
	{
		return;
	}

	ENQUEUE_RENDER_COMMAND(RacetrackSurfaceDecay)(
		[Target, HalfLivesElapsed, SaturationConstant](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("RacetrackSurfaceDecay"));

			const FRDGTextureRef Accumulation = Target->GetRenderTargetTexture(GraphBuilder);
			const FIntPoint Size = Accumulation->Desc.Extent;

			FSurfaceDecayCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSurfaceDecayCS::FParameters>();
			Parameters->Accumulation = GraphBuilder.CreateUAV(Accumulation);
			Parameters->AccumulationSize = Size;
			Parameters->HalfLivesElapsed = HalfLivesElapsed;
			Parameters->SaturationConstant = SaturationConstant;

			const TShaderMapRef<FSurfaceDecayCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("CoupledSkidDecay %dx%d", Size.X, Size.Y),
				ComputeShader,
				Parameters,
				FComputeShaderUtils::GetGroupCount(Size, FSurfaceDecayCS::ThreadGroupSize));

			GraphBuilder.Execute();
		});
}
