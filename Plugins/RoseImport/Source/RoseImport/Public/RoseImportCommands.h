// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "RoseImportStyle.h"

class FRoseImportCommands : public TCommands<FRoseImportCommands>
{
public:

	FRoseImportCommands()
		: TCommands<FRoseImportCommands>(TEXT("RoseImport"), NSLOCTEXT("Contexts", "RoseImport", "RoseImport Plugin"), NAME_None, FRoseImportStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > PluginAction;
};
