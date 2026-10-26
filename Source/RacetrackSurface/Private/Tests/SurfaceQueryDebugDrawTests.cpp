// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "SurfaceQueryDebugDraw.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const EAccumulationChannel EveryChannel[] =
	{
		EAccumulationChannel::RubberBuildup,
		EAccumulationChannel::SkidMark,
		EAccumulationChannel::Marbles
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryOverlayBarsScaleAndStatusColoursStayDistinct,
	"RacetrackSurface.Query.OverlayBarsScaleAndStatusColoursStayDistinct",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryOverlayBarsScaleAndStatusColoursStayDistinct::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceQueryOverlay;

	TestEqual(TEXT("A zero coverage bar is empty rather than absent"), BarFillFraction(0.0f, 1.0f), 0.0f);

	TestEqual(TEXT("Coverage at the full scale fills the bar"), BarFillFraction(1.0f, 1.0f), 1.0f);

	TestEqual(
		TEXT("Coverage beyond the full scale clamps rather than drawing past the frame"),
		BarFillFraction(4.0f, 1.0f),
		1.0f);

	TestEqual(
		TEXT("A negative coverage cannot draw a bar pointing backwards"),
		BarFillFraction(-0.5f, 1.0f),
		0.0f);

	TestEqual(
		TEXT("Lowering the full scale lengthens the bar in proportion, because the scaling is linear"),
		BarFillFraction(0.002637f, 0.01f),
		0.2637f,
		1.0e-5f);

	TestEqual(
		TEXT("A full scale of zero cannot divide by zero into an infinite bar"),
		BarFillFraction(0.002637f, 0.0f),
		1.0f);

	for (const EAccumulationChannel Channel : EveryChannel)
	{
		TestEqual(
			TEXT("A valid sample frames each bar in its own channel colour"),
			FrameColour(ESurfaceSampleStatus::Valid, Channel),
			ChannelColour(Channel));

		TestNotEqual(
			TEXT("An unresolved sample is never drawn in a channel colour, so it cannot be read as a "
				"channel sitting at zero"),
			FrameColour(ESurfaceSampleStatus::NotYetResolved, Channel),
			ChannelColour(Channel));

		TestNotEqual(
			TEXT("A position off the ribbon is never drawn in a channel colour either"),
			FrameColour(ESurfaceSampleStatus::OutsideRegion, Channel),
			ChannelColour(Channel));
	}

	TestNotEqual(
		TEXT("Unresolved and outside the ribbon are drawn differently from each other, because one is "
			"clean tarmac and the other is an answer that has not arrived"),
		FrameColour(ESurfaceSampleStatus::NotYetResolved, EAccumulationChannel::RubberBuildup),
		FrameColour(ESurfaceSampleStatus::OutsideRegion, EAccumulationChannel::RubberBuildup));

	for (const EAccumulationChannel First : EveryChannel)
	{
		for (const EAccumulationChannel Second : EveryChannel)
		{
			if (First == Second)
			{
				continue;
			}

			TestNotEqual(
				TEXT("No two channels share a bar colour"),
				ChannelColour(First),
				ChannelColour(Second));
		}
	}

	return true;
}

#endif
