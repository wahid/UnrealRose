// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class UnrealRoseTarget : TargetRules
{
	public UnrealRoseTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V1;
		ExtraModuleNames.AddRange( new string[] { "UnrealRose" } );
	}
}
