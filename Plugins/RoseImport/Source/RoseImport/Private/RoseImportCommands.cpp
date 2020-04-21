// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "RoseImportCommands.h"

#define LOCTEXT_NAMESPACE "FRoseImportModule"

void FRoseImportCommands::RegisterCommands()
{
	UI_COMMAND(PluginAction, "RoseImport", "Execute RoseImport action", EUserInterfaceActionType::Button, FInputGesture());
}

#undef LOCTEXT_NAMESPACE
