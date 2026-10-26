// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "SurfaceStateFile.h"

#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "SurfaceAccumulationSubsystem.h"

namespace
{
	constexpr uint32 StateFileTagSpellingRSSF = 'R' | ('S' << 8) | ('S' << 16) | ('F' << 24);

	constexpr int32 StateFileVersion = 1;

	void SerializeLayout(FArchive& Archive, FSurfaceStateLayout& Layout)
	{
		Archive << Layout.AtlasWidth;
		Archive << Layout.AtlasHeight;
		Archive << Layout.RowCount;
		Archive << Layout.TargetFormat;
		Archive << Layout.TrackLength;
		Archive << Layout.TrackWidth;
		Archive << Layout.CentimetresPerTexel;
		Archive << Layout.bClosedLoop;
	}

	int64 TexelBytes(int64 TexelCount)
	{
		return TexelCount * static_cast<int64>(sizeof(FLinearColor));
	}
}

bool RacetrackSurfaceStateFile::Write(const FString& FilePath, const FSurfaceStateFile& State)
{
	if (State.Texels.Num() != State.Layout.TexelCount())
	{
		UE_LOG(LogRacetrackSurface, Warning,
			TEXT("Surface state for %s holds %d texels, but its layout is %d x %d"),
			*FilePath,
			State.Texels.Num(),
			State.Layout.AtlasWidth,
			State.Layout.AtlasHeight);
		return false;
	}

	int64 UncompressedBytes = TexelBytes(State.Texels.Num());
	int64 CompressedBound = 0;

	if (!FCompression::CompressMemoryBound(NAME_Oodle, CompressedBound, UncompressedBytes)
		|| CompressedBound > MAX_int32)
	{
		return false;
	}

	TArray<uint8> CompressedTexels;
	CompressedTexels.SetNumUninitialized(static_cast<int32>(CompressedBound));

	int64 CompressedBytes = CompressedBound;

	if (!FCompression::CompressMemory(
		NAME_Oodle,
		CompressedTexels.GetData(),
		CompressedBytes,
		State.Texels.GetData(),
		UncompressedBytes))
	{
		return false;
	}

	CompressedTexels.SetNum(static_cast<int32>(CompressedBytes));

	uint32 Tag = StateFileTagSpellingRSSF;
	int32 Version = StateFileVersion;
	FSurfaceStateLayout Layout = State.Layout;
	float DecayProgressInHalfLives = State.DecayProgressInHalfLives;

	TArray<uint8> FileBytes;
	FMemoryWriter Writer(FileBytes, true);

	Writer << Tag;
	Writer << Version;
	SerializeLayout(Writer, Layout);
	Writer << DecayProgressInHalfLives;
	Writer << UncompressedBytes;
	Writer << CompressedTexels;

	return FFileHelper::SaveArrayToFile(FileBytes, *FilePath);
}

bool RacetrackSurfaceStateFile::Read(const FString& FilePath, FSurfaceStateFile& OutState)
{
	TArray<uint8> FileBytes;

	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath, FILEREAD_Silent))
	{
		UE_LOG(LogRacetrackSurface, Warning, TEXT("No surface state file could be read at %s"), *FilePath);
		return false;
	}

	FMemoryReader Reader(FileBytes, true);

	uint32 Tag = 0;
	int32 Version = 0;

	Reader << Tag;
	Reader << Version;

	if (Reader.IsError() || Tag != StateFileTagSpellingRSSF || Version != StateFileVersion)
	{
		UE_LOG(LogRacetrackSurface, Warning,
			TEXT("%s is not a version %d surface state file"),
			*FilePath,
			StateFileVersion);
		return false;
	}

	int64 UncompressedBytes = 0;
	TArray<uint8> CompressedTexels;

	SerializeLayout(Reader, OutState.Layout);
	Reader << OutState.DecayProgressInHalfLives;
	Reader << UncompressedBytes;
	Reader << CompressedTexels;

	const int64 TexelCount = OutState.Layout.TexelCount();

	if (Reader.IsError()
		|| TexelCount <= 0
		|| TexelCount > MAX_int32
		|| UncompressedBytes != TexelBytes(TexelCount))
	{
		UE_LOG(LogRacetrackSurface, Warning,
			TEXT("%s is truncated, or its texel count does not match its %d x %d layout"),
			*FilePath,
			OutState.Layout.AtlasWidth,
			OutState.Layout.AtlasHeight);
		return false;
	}

	OutState.Texels.SetNumUninitialized(static_cast<int32>(TexelCount));

	if (!FCompression::UncompressMemory(
		NAME_Oodle,
		OutState.Texels.GetData(),
		UncompressedBytes,
		CompressedTexels.GetData(),
		CompressedTexels.Num()))
	{
		UE_LOG(LogRacetrackSurface, Warning, TEXT("The texels in %s could not be decompressed"), *FilePath);
		return false;
	}

	return true;
}
