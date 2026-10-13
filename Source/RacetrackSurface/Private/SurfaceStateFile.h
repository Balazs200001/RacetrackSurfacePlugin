#pragma once

#include "CoreMinimal.h"

struct FSurfaceStateLayout
{
	int32 AtlasWidth = 0;

	int32 AtlasHeight = 0;

	int32 RowCount = 0;

	int32 TargetFormat = 0;

	double TrackLength = 0.0;

	double TrackWidth = 0.0;

	double CentimetresPerTexel = 0.0;

	bool bClosedLoop = false;

	bool operator==(const FSurfaceStateLayout& Other) const = default;

	int64 TexelCount() const
	{
		return static_cast<int64>(AtlasWidth) * static_cast<int64>(AtlasHeight);
	}
};

struct FSurfaceStateFile
{
	FSurfaceStateLayout Layout;

	float DecayProgressInHalfLives = 0.0f;

	TArray<FLinearColor> Texels;
};

namespace RacetrackSurfaceStateFile
{
	bool Write(const FString& FilePath, const FSurfaceStateFile& State);

	bool Read(const FString& FilePath, FSurfaceStateFile& OutState);
}
