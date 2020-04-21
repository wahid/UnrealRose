// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "RoseImport.h"
#include "RoseImportStyle.h"
#include "RoseImportCommands.h"
#include "Misc/MessageDialog.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "EditorSupportDelegates.h"
#include "Modules/ModuleManager.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "AssetToolsModule.h"
#include "PackageTools.h"
#include "Misc/FileHelper.h"
#include "Factories/TextureFactory.h"
#include "Factories/Factory.h"
#include "Factories/MaterialFactoryNew.h"
#include "LevelEditor.h"
#include "AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "UObject/UObjectGlobals.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ComponentReregisterContext.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "MeshUtilities.h"
#include "RawMesh.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "PhysicsAssetUtils.h"
#include "Animation/AnimSequence.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Timeline.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "Builders/CubeBuilder.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "Engine/Polys.h"
#include "Engine/BlockingVolume.h"
#include "GameFramework/Volume.h"
#include "Components/BrushComponent.h"
#include "BSPOps.h"
#include "Landscape.h"
#include "LandscapeInfo.h"

#include "Common.h"
#include "Zmd.h"
#include "Zms.h"
#include "Zmo.h"
#include "Zsc.h"
#include "Chr.h"
#include "Him.h"
#include "Ifo.h"
#include "Til.h"

static const FName RoseImportTabName("RoseImport");

#define LOCTEXT_NAMESPACE "FRoseImportModule"

void FRoseImportModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FRoseImportStyle::Initialize();
	FRoseImportStyle::ReloadTextures();

	FRoseImportCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FRoseImportCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FRoseImportModule::PluginButtonClicked),
		FCanExecuteAction());
		
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	
	{
		TSharedPtr<FExtender> MenuExtender = MakeShareable(new FExtender());
		MenuExtender->AddMenuExtension("WindowLayout", EExtensionHook::After, PluginCommands, FMenuExtensionDelegate::CreateRaw(this, &FRoseImportModule::AddMenuExtension));

		LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(MenuExtender);
	}
	
	{
		TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
		ToolbarExtender->AddToolBarExtension("Settings", EExtensionHook::After, PluginCommands, FToolBarExtensionDelegate::CreateRaw(this, &FRoseImportModule::AddToolbarExtension));
		
		LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
	}
}

void FRoseImportModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	FRoseImportStyle::Shutdown();

	FRoseImportCommands::Unregister();
}

void RefreshCollisionChange(const UStaticMesh* StaticMesh)
{
	for (FObjectIterator Iter(UStaticMeshComponent::StaticClass()); Iter; ++Iter)
	{
		UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(*Iter);

		if (StaticMeshComponent->GetStaticMesh() == StaticMesh)
		{
			// it needs to recreate IF it already has been created
			if (StaticMeshComponent->IsPhysicsStateCreated())
			{
				StaticMeshComponent->RecreatePhysicsState();
			}
		}
	}

	FEditorSupportDelegates::RedrawAllViewports.Broadcast();
}

void BuildAssetPath(FString& PackageName, FString& AssetName, const FString& RosePath, const FString& Postfix = TEXT(""))
{
	FString NormPath = RosePath.ToUpper();
	FPaths::NormalizeFilename(NormPath);
	TArray<FString> PathParts;
	NormPath.ParseIntoArray(PathParts, TEXT("/"), true);
	FString FileName = PathParts.Pop();

	AssetName = FPaths::GetBaseFilename(FileName);
	if (!Postfix.IsEmpty()) {
		AssetName.Append(Postfix);
	}

	if (PathParts.Num() <= 0) {
		UE_DEBUG_BREAK();
	}

	if (PathParts[0].Compare(TEXT("3DDATA")) != 0) {
		UE_DEBUG_BREAK();
	}
	PathParts.RemoveAt(0);

	if (PathParts[0].Compare(TEXT("JUNON")) == 0 ||
		PathParts[0].Compare(TEXT("LUNAR")) == 0 ||
		PathParts[0].Compare(TEXT("ORO")) == 0) {
		PathParts.Insert(TEXT("MAPS"), 0);
	}

	PackageName = FString(TEXT("/")) + FString::Join(PathParts, TEXT("/"));
}

UPackage* GetOrMakePackage(const FString& PackageName, FString& AssetName) {
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	FString FinalPackageName;
	FString BasePackageName = RosePackageName + PackageName / AssetName;
	BasePackageName = PackageTools::SanitizePackageName(BasePackageName);

	AssetToolsModule.Get().CreateUniqueAssetName(BasePackageName, TEXT(""), FinalPackageName, AssetName);

	UE_LOG(LogTemp, Log, TEXT("Making Package - %s, %s, %s, %s"), *RosePackageName, *PackageName, *AssetName, *BasePackageName, *FinalPackageName);

	UPackage* Package = CreatePackage(NULL, *FinalPackageName);

	if (Package == NULL) {
		UE_LOG(LogTemp, Error, TEXT("Failed to create package - %s"), *FinalPackageName);
	}
	return Package;
}

template<typename T>
T* GetExistingAsset(const FString& PackageName, const FString& AssetName) {
	FString BasePackageName = RosePackageName + PackageName / AssetName;
	BasePackageName = PackageTools::SanitizePackageName(BasePackageName);
	BasePackageName.Append(TEXT("."));
	BasePackageName.Append(AssetName);
	T* ExistingAsset = LoadObject<T>(NULL, *BasePackageName);
	if (ExistingAsset) {
		return ExistingAsset;
	}
	return NULL;
}

UTexture* ImportTexture(const FString& PackageName, FString& AssetName, const FString& SourcePath)
{
	UTexture* ExistingTexture = GetExistingAsset<UTexture>(PackageName, AssetName);
	if (ExistingTexture != NULL) {
		return ExistingTexture;
	}

	UPackage* Package = GetOrMakePackage(PackageName, AssetName);
	if (Package == NULL) {
		return NULL;
	}

	TArray<uint8> DataBinary;

	const FString newPath = SourcePath.Replace(TEXT("DDS"), TEXT("png"));

	if (!FFileHelper::LoadFileToArray(DataBinary, *newPath)) {
		//UE_LOG(RosePlugin, Warning, TEXT("Unable to read texture from source."));
		return NULL;
	}

	
	const uint8* PtrTexture = DataBinary.GetData();

	UTextureFactory* TextureFact = NewObject<UTextureFactory>();

	TextureFact->AddToRoot();

	UTexture* Texture = (UTexture*)TextureFact->FactoryCreateBinary(
		UTexture2D::StaticClass(), Package, *AssetName,
		RF_Standalone | RF_Public, NULL, TEXT("png"),
		PtrTexture, PtrTexture + DataBinary.Num(), GWarn);

	if (Texture != NULL)
	{
		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(Texture);

		// Set the dirty flag so this package will get saved later
		Texture->MarkPackageDirty();
	}

	TextureFact->RemoveFromRoot();

	return Texture;
}

UMaterial* GetOrMakeBaseMaterial(const Zsc::Texture& MatInfo) {
	FString MaterialName;
	if (MatInfo.alphaTestEnabled) {
		MaterialName = "AlphaRefMaterial";
	}
	else if (MatInfo.alphaEnabled) {
		MaterialName = "AlphaMaterial";
	}
	else {
		MaterialName = "BaseMaterial";
	}

	if (MatInfo.twoSided) {
		MaterialName.Append("_DS");
	}

	FString MaterialFullName = FString::Printf(TEXT("%s/%s.%s"), *RosePackageName, *MaterialName, *MaterialName);
	UMaterial* Material = LoadObject<UMaterial>(NULL, *MaterialFullName, NULL, LOAD_None, NULL);
	if (Material != NULL) {
		//UE_LOG(RosePlugin, Log, TEXT("Skipped Base Creation - Found It!"));
		return Material;
	}

	//UE_LOG(RosePlugin, Log, TEXT("Creating Base Material!"));

	UMaterialFactoryNew* MaterialFactory = NewObject<UMaterialFactoryNew>(); // (UMaterialFactoryNew*)UMaterialFactoryNew::StaticClass();

	UPackage* Package = GetOrMakePackage(TEXT("/"), MaterialName);

	Material = (UMaterial*)MaterialFactory->FactoryCreateNew(UMaterial::StaticClass(), Package, *MaterialName, RF_Standalone | RF_Public, NULL, GWarn);

	if (Material == NULL) {
		return NULL;
	}

	// Notify the asset registry
	FAssetRegistryModule::AssetCreated(Material);

	// Set the dirty flag so this package will get saved later
	Material->MarkPackageDirty();

	// make sure that any static meshes, etc using this material will stop using the FMaterialResource of the original 
	// material, and will use the new FMaterialResource created when we make a new UMaterial in place
	FGlobalComponentReregisterContext RecreateComponents;

	// let the material update itself if necessary
	Material->PreEditChange(NULL);

	if (MatInfo.alphaTestEnabled) {
		Material->BlendMode = BLEND_Masked;
		Material->OpacityMaskClipValue = 0.5f;
	}
	else if (MatInfo.alphaEnabled) {
		Material->BlendMode = BLEND_Translucent;
	}
	else {
		Material->BlendMode = BLEND_Opaque;
	}

	if (MatInfo.twoSided) {
		Material->TwoSided = 1;
	}

	UMaterialExpressionTextureSampleParameter2D* UnrealTextureExpression = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
		
	Material->Expressions.Add(UnrealTextureExpression);
	UnrealTextureExpression->ConnectExpression(&Material->BaseColor, 0);

	if (MatInfo.alphaTestEnabled) {
		UnrealTextureExpression->ConnectExpression(&Material->OpacityMask, 4);
	}

	UnrealTextureExpression->ParameterName = TEXT("Texture");
	UnrealTextureExpression->SetDefaultTexture();
	UnrealTextureExpression->SamplerType = SAMPLERTYPE_Color;
	UnrealTextureExpression->MaterialExpressionEditorX = -320;
	UnrealTextureExpression->MaterialExpressionEditorY = 240;

	Material->bUsedWithSkeletalMesh = true;

	Material->PostEditChange();

	return Material;
}

UMaterialInterface* ImportMaterial(const FString& PackageName, FString& MaterialName, const Zsc::Texture& TexData, UTexture* Texture) {
	UPackage* Package = GetOrMakePackage(PackageName, MaterialName);
	if (Package == NULL) {
		return NULL;
	}

	UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(Package, *MaterialName, RF_Standalone | RF_Public);

	if (Material == NULL) {
		return NULL;
	}

	// Notify the asset registry
	FAssetRegistryModule::AssetCreated(Material);

	// Set the dirty flag so this package will get saved later
	Material->MarkPackageDirty();

	UMaterial* BaseMaterial = GetOrMakeBaseMaterial(TexData);

	// make sure that any static meshes, etc using this material will stop using the FMaterialResource of the original 
	// material, and will use the new FMaterialResource created when we make a new UMaterial in place
	FGlobalComponentReregisterContext RecreateComponentsX;

	// let the material update itself if necessary
	Material->PreEditChange(NULL);

	Material->SetParentEditorOnly(BaseMaterial);
	Material->SetTextureParameterValueEditorOnly(TEXT("Texture"), Texture);

	if (TexData.alphaTestEnabled && TexData.alphaReference != 128) {
		//Material->bOverrideBaseProperties = true;
		Material->BasePropertyOverrides.bOverride_OpacityMaskClipValue = true;
		Material->BasePropertyOverrides.OpacityMaskClipValue = (float)TexData.alphaReference / 255.0f;
	}

	Material->PostEditChange();

	return Material;
}

struct ImportSkelData {
	ImportSkelData(Zmd& _data, const FString& _SkelPackage, FString& _SkelName)
		: data(_data), skeleton(0), SkelPackage(_SkelPackage), SkelName(_SkelName) {}
	Zmd& data;

	USkeleton* skeleton;
	const FString& SkelPackage;
	FString& SkelName;
};

struct ImportMeshData {
	struct Item {
		Item(Zms& _data, uint32 _matIdx)
			: data(_data), matIdx(_matIdx),
			vertOffset(0), indexOffset(0), faceOffset(0) {}

		Zms& data;
		uint32 matIdx;

		uint32 vertOffset;
		uint32 indexOffset;
		uint32 faceOffset;
	};

	TArray<Item> meshes;
	TArray<UMaterialInterface*> materials;
};

USkeleton* ApplySkeletonToMesh(const FString& PackageName, FString& SkeletonName, USkeletalMesh* Mesh, ImportSkelData& skelData) {
	FReferenceSkeleton& RefSkeleton = Mesh->RefSkeleton;
	FReferenceSkeletonModifier modifier(RefSkeleton, Mesh->Skeleton);

	for (int i = 0; i < skelData.data.bones.Num(); ++i) {
		const Zmd::Bone& bone = skelData.data.bones[i];

		int32 ueParent = (i > 0) ? bone.parent : INDEX_NONE;
		const FMeshBoneInfo BoneInfo(FName(bone.name, FNAME_Add), bone.name, ueParent);
		const FTransform BoneTransform(bone.rotation, bone.translation);
		modifier.Add(BoneInfo, BoneTransform);
	}

	Mesh->CalculateInvRefMatrices();

	if (!skelData.skeleton) {
		UPackage* Package = GetOrMakePackage(PackageName, SkeletonName);
		if (Package == NULL) {
			return NULL;
		}

		USkeleton* Skeleton = CastChecked<USkeleton>(NewObject<USkeleton>());
		if (Skeleton == NULL) {
			return NULL;
		}

		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(Skeleton);

		// Set the dirty flag so this package will get saved later
		Skeleton->MarkPackageDirty();

		skelData.skeleton = Skeleton;
	}

	if (skelData.skeleton) {
		skelData.skeleton->MergeAllBonesToBoneTree(Mesh);
	}

	return Mesh->Skeleton;
}

USkeletalMesh* ImportSkeletalMesh(const FString& PackageName, FString& MeshName, ImportMeshData meshData, ImportSkelData& skelData) {
	UPackage* Package = GetOrMakePackage(PackageName, MeshName);
	if (Package == NULL) {
		return NULL;
	}

	USkeletalMesh* SkeletalMesh = CastChecked<USkeletalMesh>(NewObject<USkeletalMesh>());
	//StaticConstructObject(USkeletalMesh::StaticClass(), Package, *MeshName, RF_Standalone | RF_Public));
	if (SkeletalMesh == NULL) {
		return NULL;
	}

	// Notify the asset registry
	FAssetRegistryModule::AssetCreated(SkeletalMesh);

	// Set the dirty flag so this package will get saved later
	SkeletalMesh->MarkPackageDirty();

	SkeletalMesh->PreEditChange(NULL);

	for (int i = 0; i < meshData.materials.Num(); ++i) {
		SkeletalMesh->Materials.Add(FSkeletalMaterial(meshData.materials[i]));
	}

	FString SkeletonName = MeshName + "_Skeleton";
	USkeleton* Skeleton = ApplySkeletonToMesh(PackageName, SkeletonName, SkeletalMesh, skelData);
	if (Skeleton == NULL) {
		return NULL;
	}

	FSkeletalMeshModel* ImportedResource = SkeletalMesh->GetImportedModel(); //SkeletalMesh->GetImportedResource();
	check(ImportedResource->LODModels.Num() == 0);
	ImportedResource->LODModels.Empty();
	//new(ImportedResource->LODModels)FStaticLODModel();

	SkeletalMesh->GetLODInfoArray().Empty();
	SkeletalMesh->GetLODInfoArray().AddZeroed();
	SkeletalMesh->GetLODInfoArray()[0].LODHysteresis = 0.02f;
	FSkeletalMeshOptimizationSettings Settings;
	// set default reduction settings values
	SkeletalMesh->GetLODInfoArray()[0].ReductionSettings = Settings;

	SkeletalMesh->bHasVertexColors = false;

	FSkeletalMeshLODModel& LODModel = (FSkeletalMeshLODModel&)ImportedResource->LODModels[0];
	LODModel.NumTexCoords = 1;

	IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");

	auto meshList = meshData.meshes;

	TArray<FVector> LODPoints;
	TArray<SkeletalMeshImportData::FMeshWedge> LODWedges;
	TArray<SkeletalMeshImportData::FMeshFace> LODFaces;
	TArray<SkeletalMeshImportData::FVertInfluence> LODInfluences;
	TArray<int32> LODPointToRawMap;

	int32 totalVertCount = 0;
	int32 totalIndexCount = 0;
	int32 totalFaceCount = 0;
	for (int i = 0; i < meshList.Num(); ++i) {
		meshList[i].vertOffset = totalVertCount;
		meshList[i].indexOffset = totalIndexCount;
		meshList[i].faceOffset = totalFaceCount;
		totalVertCount += meshList[i].data.vertexPositions.Num();
		totalIndexCount += meshList[i].data.indexes.Num();
		totalFaceCount += meshList[i].data.indexes.Num() / 3;
	}

	LODPoints.AddZeroed(totalVertCount);
	LODPointToRawMap.AddZeroed(totalVertCount);
	LODWedges.AddZeroed(totalIndexCount);
	LODFaces.AddZeroed(totalFaceCount);

	bool hasNormals = true;

	for (int i = 0; i < meshList.Num(); ++i) {
		Zms& tmesh = meshList[i].data;

		if (tmesh.vertexNormals.Num() == 0) {
			hasNormals = false;
		}

		for (int j = 0; j < tmesh.vertexPositions.Num(); ++j) {
			int32 vertIdx = meshList[i].vertOffset + j;
			LODPoints[vertIdx] = tmesh.vertexPositions[j];
			LODPointToRawMap[vertIdx] = vertIdx;
		}

		for (int j = 0; j < tmesh.indexes.Num(); ++j) {
			int32 wedgeIdx = meshList[i].indexOffset + j;
			LODWedges[wedgeIdx].iVertex = meshList[i].vertOffset + tmesh.indexes[j];
			LODWedges[wedgeIdx].UVs[0] = tmesh.vertexUvs[0][tmesh.indexes[j]];

			if (LODWedges[wedgeIdx].iVertex >= (uint32)totalVertCount) {
				UE_DEBUG_BREAK();
			}
		}

		int32 faceCount = tmesh.indexes.Num() / 3;
		for (int j = 0; j < faceCount; ++j) {
			int32 faceIdx = meshList[i].faceOffset + j;
			LODFaces[faceIdx].iWedge[0] = meshList[i].indexOffset + (j * 3 + 0);
			LODFaces[faceIdx].iWedge[1] = meshList[i].indexOffset + (j * 3 + 1);
			LODFaces[faceIdx].iWedge[2] = meshList[i].indexOffset + (j * 3 + 2);
			if (hasNormals) {
				LODFaces[faceIdx].TangentZ[0] = tmesh.vertexNormals[tmesh.indexes[j * 3 + 0]];
				LODFaces[faceIdx].TangentZ[1] = tmesh.vertexNormals[tmesh.indexes[j * 3 + 1]];
				LODFaces[faceIdx].TangentZ[2] = tmesh.vertexNormals[tmesh.indexes[j * 3 + 2]];
			}
			LODFaces[faceIdx].MeshMaterialIndex = meshList[i].matIdx;
		}

		for (int j = 0; j < tmesh.vertexPositions.Num(); ++j) {
			int32 infBaseIdx = (meshList[i].vertOffset + j) * 4;
			for (int k = 0; k < 4; ++k) {
				SkeletalMeshImportData::FVertInfluence vi;
				vi.VertIndex = meshList[i].vertOffset + j;
				vi.BoneIndex = tmesh.boneWeights[j].boneIdx[k];
				vi.Weight = tmesh.boneWeights[j].weight[k];
				if (vi.Weight < 0.0001f) {
					continue;
				}
				if (vi.VertIndex >= (uint32)totalVertCount) {
					UE_DEBUG_BREAK();
				}
				if (vi.BoneIndex >= skelData.data.bones.Num()) {
					UE_DEBUG_BREAK();
				}
				LODInfluences.Add(vi);
			}
		}
	}

	TArray<FText> WarningMessages;
	TArray<FName> WarningNames;
	// Create actual rendering data.

	IMeshUtilities::MeshBuildOptions meshBuildOptions;
	meshBuildOptions.bComputeWeightedNormals = !hasNormals;

	if (!MeshUtilities.BuildSkeletalMesh(
		ImportedResource->LODModels[0],
		SkeletalMesh->RefSkeleton,
		LODInfluences, 
		LODWedges, 
		LODFaces, 
		LODPoints, 
		LODPointToRawMap,
		meshBuildOptions,
		//false, 
		//!hasNormals, 
		//true, 
		&WarningMessages, 
		&WarningNames))
	{
		UE_DEBUG_BREAK();
	}
	else if (WarningMessages.Num() > 0)
	{
		UE_DEBUG_BREAK();
	}

	const int32 NumSections = LODModel.Sections.Num();
	for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
	{

		//SkeletalMesh->GetLODInfoArray()[0].TriangleSortSettings.AddZeroed();
	}


	SkeletalMesh->PostEditChange();

	FString PhysName = MeshName + "_PhysicsAsset";
	UPhysicsAsset* PhysicsAsset = CastChecked<UPhysicsAsset>(NewObject<UPhysicsAsset>());
	//StaticConstructObject(UPhysicsAsset::StaticClass(), Package, *PhysName, RF_Standalone | RF_Public));
	if (PhysicsAsset) {
		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(PhysicsAsset);

		// Set the dirty flag so this package will get saved later
		PhysicsAsset->MarkPackageDirty();

		// Create the data!
		FPhysAssetCreateParams NewBodyData;
		// NewBodyData.Initialize();
		FText CreationErrorMessage;
		FPhysicsAssetUtils::CreateFromSkeletalMesh(PhysicsAsset, SkeletalMesh, NewBodyData, CreationErrorMessage);
	}

	return NULL;
}

UAnimSequence* ImportSkeletalAnim(const FString& PackageName, FString& AnimName, ImportSkelData& skelData, const Zmo& anim) {
	UPackage* Package = GetOrMakePackage(PackageName, AnimName);
	if (Package == NULL) {
		return NULL;
	}

	UAnimSequence* AnimSeq = CastChecked<UAnimSequence>(NewObject<UAnimSequence>());
		//StaticConstructObject(UAnimSequence::StaticClass(), Package, *AnimName, RF_Standalone | RF_Public));
	if (AnimSeq == NULL) {
		return NULL;
	}

	// Notify the asset registry
	FAssetRegistryModule::AssetCreated(AnimSeq);

	// Set the dirty flag so this package will get saved later
	AnimSeq->MarkPackageDirty();

	AnimSeq->PreEditChange(NULL);

	AnimSeq->SetSkeleton(skelData.skeleton);

	AnimSeq->SequenceLength = (float)anim.frameCount / (float)anim.framesPerSecond;
	AnimSeq->SetRawNumberOfFrame(anim.frameCount);

	TArray<FRawAnimSequenceTrack> tracks;
	for (int i = 0; i < skelData.data.bones.Num(); ++i) {
		const Zmd::Bone& bone = skelData.data.bones[i];
		FRawAnimSequenceTrack track;
		// All keys must be ABSOLUTE for Unreal!
		track.PosKeys.Add(bone.translation);
		track.RotKeys.Add(bone.rotation);
		track.ScaleKeys.Add(FVector(1, 1, 1));
		tracks.Add(track);
	}

	for (int i = 0; i < anim.channels.Num(); ++i) {
		Zmo::Channel* channel = anim.channels[i];
		FRawAnimSequenceTrack& track = tracks[channel->index];
		if (channel->type() == Zmo::ChannelType::Position) {
			auto posChannel = (Zmo::PositionChannel*)channel;
			track.PosKeys.Empty();
			for (int j = 0; j < posChannel->frames.Num(); ++j) {
				track.PosKeys.Add(posChannel->frames[j]);
			}
		}
		else if (channel->type() == Zmo::ChannelType::Rotation) {
			auto rotChannel = (Zmo::RotationChannel*)channel;
			track.RotKeys.Empty();
			for (int j = 0; j < rotChannel->frames.Num(); ++j) {
				track.RotKeys.Add(rotChannel->frames[j]);
			}
		}
		else if (channel->type() == Zmo::ChannelType::Scale) {
			auto scaleChannel = (Zmo::ScaleChannel*)channel;
			track.ScaleKeys.Empty();
			for (int j = 0; j < scaleChannel->frames.Num(); ++j) {
				track.ScaleKeys.Add(scaleChannel->frames[j]);
			}
		}
		else {
			UE_DEBUG_BREAK();
		}
	}

	//AnimSeq->GetRawAnimationData().Empty();
	//AnimSeq->AnimationTrackNames.Empty();
	//AnimSeq->TrackToSkeletonMapTable.Empty();

	for (int i = 0; i < tracks.Num(); ++i) {
		AnimSeq->AddNewRawTrack(skelData.data.bones[i].name, &tracks[i]);

		//AnimSeq->BakeOutVirtualBoneTracks(tracks[i], skelData.data.bones[i].name, FTrackToSkeletonMap(i));
		/* AnimSeq->RawAnimationData.Add(tracks[i]);
		AnimSeq->AnimationTrackNames.Add(skelData.data.bones[i].name);
		AnimSeq->TrackToSkeletonMapTable.Add(FTrackToSkeletonMap(i)); */
	}

	AnimSeq->PostProcessSequence();

	AnimSeq->PostEditChange();

	return AnimSeq;
}

const TCHAR* animNames[] = {
	TEXT("Stop"),
	TEXT("Walk"),
	TEXT("Attack"),
	TEXT("Hit"),
	TEXT("Die"),
	TEXT("Run"),
	TEXT("Casting1"),
	TEXT("SkillAction1"),
	TEXT("Casting2"),
	TEXT("SkillAction2"),
	TEXT("Etc")
};
const int MaxAnims = 11;

void ImportChar(const Chr& chars, const Zsc& meshs, uint32 charIdx) {
	FString CharName = FString::Printf(TEXT("Char_%d"), charIdx);
	FString PackageName = FString(TEXT("/")) + CharName;

	const Chr::Character& mchar = chars.characters[charIdx];

	ImportMeshData meshData;
	Zmd meshZmd(*(RoseBasePath + chars.skeletons[mchar.skeletonIdx]));
	FString SkelPackage;
	FString SkelName;
	UE_DEBUG_BREAK();
	// ToDo Names
	ImportSkelData skelData(meshZmd, SkelPackage, SkelName);

	int texIdx = 0;
	for (int i = 0; i < mchar.models.Num(); ++i) {
		const Zsc::Model& model = meshs.models[mchar.models[i]];

		for (int j = 0; j < model.parts.Num(); ++j) {
			const Zsc::Part& part = model.parts[j];
			const Zsc::Texture& tex = meshs.textures[part.texIdx];

			if (part.dummyIdx != 0xFFFF || part.boneIdx != 0xFFFF) {
				continue;
			}

			FString TextureName = FString::Printf(TEXT("%s_%d_Texture"), *CharName, texIdx);
			UTexture* UnrealTexture = ImportTexture(PackageName, TextureName, RoseBasePath + tex.filePath);

			FString MaterialName = FString::Printf(TEXT("%s_%d_Material"), *CharName, texIdx);
			UMaterialInterface* UnrealMaterial = ImportMaterial(PackageName, MaterialName, tex, UnrealTexture);

			meshData.materials.Add(UnrealMaterial);

			auto meshZms = new Zms(*(RoseBasePath + meshs.meshes[part.meshIdx]));
			meshData.meshes.Add(ImportMeshData::Item(*meshZms, texIdx));

			texIdx++;
		}
	}

	USkeletalMesh* skelMesh = ImportSkeletalMesh(PackageName, CharName, meshData, skelData);

	for (int i = 0; i < mchar.animations.Num(); ++i) {
		const Chr::Animation& anim = mchar.animations[i];

		if (anim.type >= MaxAnims) {
			//UE_LOG(RosePlugin, Warning, TEXT("Skipped unknown animation %d"), anim.type);
			continue;
		}

		Zmo animZmo(*(RoseBasePath + chars.animations[anim.animationIdx]));
		FString AnimName = FString::Printf(TEXT("%s_%s"), *CharName, animNames[anim.type]);
		UAnimSequence* animSeq = ImportSkeletalAnim(PackageName, AnimName, skelData, animZmo);
	}
}

USkeletalMesh* ImportAvatarItem(const FString& ItemTypeName, const Zsc& meshs, ImportSkelData& skelData, int modelIdx, int boneIdx = -1) {
	const Zsc::Model& model = meshs.models[modelIdx];
	ImportMeshData meshData;

	if (model.parts.Num() == 0) {
		UE_DEBUG_BREAK();
	}

	for (int j = 0; j < model.parts.Num(); ++j) {
		const Zsc::Part& part = model.parts[j];
		const Zsc::Texture& tex = meshs.textures[part.texIdx];

		if (part.dummyIdx != 0xFFFF || part.boneIdx != 0xFFFF) {
			continue;
		}

		FString ZmsPath = meshs.meshes[part.meshIdx];

		FString TexturePackage, TextureName;
		BuildAssetPath(TexturePackage, TextureName, tex.filePath, "_Texture");
		UTexture* UnrealTexture = ImportTexture(TexturePackage, TextureName, RoseBasePath + tex.filePath);

		FString MaterialPackage, MaterialName;
		BuildAssetPath(MaterialPackage, MaterialName, ZmsPath);
		MaterialName = FString::Printf(TEXT("Model_%d_%d_Material"), modelIdx, j);
		UMaterialInterface* UnrealMaterial = ImportMaterial(MaterialPackage, MaterialName, tex, UnrealTexture);
		meshData.materials.Add(UnrealMaterial);

		auto meshZms = new Zms(*(RoseBasePath + ZmsPath));
		meshData.meshes.Add(ImportMeshData::Item(*meshZms, j));
	}

	FString ModelPackage, ModelName;
	ModelPackage = TEXT("/AVATAR");
	ModelName = FString::Printf(TEXT("%s_%d"), *ItemTypeName, modelIdx);
	USkeletalMesh* skelMesh = ImportSkeletalMesh(ModelPackage, ModelName, meshData, skelData);

	return skelMesh;
}

UK2Node_CallFunction* CreateCallFuncNode(UEdGraph* Graph, UFunction* Function) {

	UK2Node_CallFunction* NodeTemplate = NewObject<UK2Node_CallFunction>();
	FVector2D NodeLocation = Graph->GetGoodPlaceForNewNode();
	UK2Node_CallFunction* CallNode = FEdGraphSchemaAction_K2NewNode::SpawnNodeFromTemplate<UK2Node_CallFunction>(Graph, NodeTemplate, NodeLocation);
	CallNode->SetFromFunction(Function);
	CallNode->ReconstructNode();
	return CallNode;
}
UK2Node_CallFunction* CreateCallFuncNode(UEdGraph* Graph, const TCHAR* LibraryName, const TCHAR* FuncName) {
	UFunction* Function = FindObject<UClass>(ANY_PACKAGE, LibraryName)->FindFunctionByName(FuncName);
	return CreateCallFuncNode(Graph, Function);
}
template<typename FuncOwner>
UK2Node_CallFunction* CreateCallFuncNode(UEdGraph* Graph, const TCHAR* FuncName) {
	UFunction* Function = FuncOwner::StaticClass()->FindFunctionByName(FuncName);
	return CreateCallFuncNode(Graph, Function);
}

UK2Node_VariableGet* CreateVarGetNode(UEdGraph* Graph, const FName& VariableName)
{
	UK2Node_VariableGet* NodeTemplate = NewObject<UK2Node_VariableGet>();
	NodeTemplate->VariableReference.SetSelfMember(VariableName);
	FVector2D NodeLocation = Graph->GetGoodPlaceForNewNode();
	UK2Node_VariableGet* GetVarNode = FEdGraphSchemaAction_K2NewNode::SpawnNodeFromTemplate<UK2Node_VariableGet>(Graph, NodeTemplate, NodeLocation);
	return GetVarNode;
}

template<typename CurveType>
CurveType* CreateCurveObject(UObject* PackagePtr, FName& AssetName)
{
	return NewObject<CurveType>(PackagePtr, AssetName, RF_Transient);
}


UBlueprint* ImportWorldZscModel(const FString& MdlTypeName, const Zsc& meshs, int modelIdx) {
	const Zsc::Model& model = meshs.models[modelIdx];

	FString BPPackageName = TEXT("/MAPS");
	FString BPAssetName = FString::Printf(TEXT("%s_%d"), *MdlTypeName, modelIdx);

	UPackage* BPPackage = GetOrMakePackage(BPPackageName, BPAssetName);
	if (BPPackage == NULL) {
		return NULL;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), BPPackage, *BPAssetName,
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName("RosePluginWhat"));

	USCS_Node* RootNode = NULL;
	for (int j = 0; j < model.parts.Num(); ++j) {
		const Zsc::Part& part = model.parts[j];
		const Zsc::Texture& tex = meshs.textures[part.texIdx];
		const FString& mesh = meshs.meshes[part.meshIdx];

		FString ModelPackage, ModelName;
		BuildAssetPath(ModelPackage, ModelName, mesh);

		UPackage* Package = GetOrMakePackage(ModelPackage, ModelName);
		if (Package == NULL) {
			return NULL;
		}

		UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Package, *ModelName, RF_Standalone | RF_Public);

		if (StaticMesh == NULL) {
			return NULL;
		}

		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(StaticMesh);

		// Set the dirty flag so this package will get saved later
		StaticMesh->MarkPackageDirty();

		// make sure it has a new lighting guid
		StaticMesh->LightingGuid = FGuid::NewGuid();

		// Set it to use textured lightmaps. Note that Build Lighting will do the error-checking (texcoordindex exists for all LODs, etc).
		StaticMesh->LightMapResolution = 128;
		StaticMesh->LightMapCoordinateIndex = 1;

		// new(StaticMesh->GetSourceModels()) FStaticMeshSourceModel();
		new(StaticMesh->SourceModels) FStaticMeshSourceModel();

		FStaticMeshSourceModel& SrcModel = StaticMesh->GetSourceModels()[0];

		FRawMesh RawMesh;
		SrcModel.RawMeshBulkData->SaveRawMesh(RawMesh);
		{
			FString TexturePackage, TextureName;
			BuildAssetPath(TexturePackage, TextureName, tex.filePath, "_Texture");
			UTexture* UnrealTexture = ImportTexture(TexturePackage, TextureName, RoseBasePath + tex.filePath);

			FString MaterialPackage, MaterialName;
			BuildAssetPath(MaterialPackage, MaterialName, meshs.meshes[part.meshIdx]);
			MaterialName = FString::Printf(TEXT("Model_%d_%d_Material"), modelIdx, j);
			UMaterialInterface* UnrealMaterial = ImportMaterial(MaterialPackage, MaterialName, tex, UnrealTexture);

			//StaticMesh->Materials.Add(UnrealMaterial);
			StaticMesh->StaticMaterials.Add(UnrealMaterial);

			Zms meshZms(*(RoseBasePath + mesh));

			RawMesh.VertexPositions.AddZeroed(meshZms.vertexPositions.Num());
			for (int i = 0; i < meshZms.vertexPositions.Num(); ++i) {
				RawMesh.VertexPositions[i] = meshZms.vertexPositions[i];
			}

			RawMesh.WedgeIndices.AddZeroed(meshZms.indexes.Num());
			//RawMesh.WedgeTangentX.AddZeroed(meshZms.indexes.Num());
			//RawMesh.WedgeTangentY.AddZeroed(meshZms.indexes.Num());
			//RawMesh.WedgeTangentZ.AddZeroed(meshZms.indexes.Num());
			for (int i = 0; i < meshZms.indexes.Num(); ++i) {
				RawMesh.WedgeIndices[i] = meshZms.indexes[i];
				//RawMesh.WedgeTangentZ[indexOffset + i] = meshZms.vertexNormals[meshZms.indexes[i]];
			}

			for (int k = 0; k < 4; ++k) {
				if (meshZms.vertexUvs[k].Num() > 0) {
					RawMesh.WedgeTexCoords[k].AddZeroed(meshZms.indexes.Num());
					for (int i = 0; i < meshZms.indexes.Num(); ++i) {
						RawMesh.WedgeTexCoords[k][i] = meshZms.vertexUvs[k][meshZms.indexes[i]];
					}
				}
			}

			int faceCount = meshZms.indexes.Num() / 3;
			RawMesh.FaceMaterialIndices.AddZeroed(faceCount);
			RawMesh.FaceSmoothingMasks.AddZeroed(faceCount);
			for (int i = 0; i < faceCount; ++i) {
				RawMesh.FaceMaterialIndices[i] = 0;
				RawMesh.FaceSmoothingMasks[i] = 1;
			}
		}
		SrcModel.RawMeshBulkData->SaveRawMesh(RawMesh);

		SrcModel.BuildSettings.bRemoveDegenerates = true;
		SrcModel.BuildSettings.bRecomputeNormals = false;
		SrcModel.BuildSettings.bRecomputeTangents = false;

		StaticMesh->Build(true);

		// Set up the mesh collision
		StaticMesh->CreateBodySetup();

		// Create new GUID
		StaticMesh->BodySetup->InvalidatePhysicsData();

		// Per-poly collision for now
		StaticMesh->BodySetup->CollisionTraceFlag = ECollisionTraceFlag::CTF_UseComplexAsSimple;
		StaticMesh->BodySetup->bDoubleSidedGeometry = true;

		// refresh collision change back to staticmesh components
		RefreshCollisionChange(StaticMesh);

		for (int32 SectionIndex = 0; SectionIndex < StaticMesh->Materials_DEPRECATED.Num(); SectionIndex++)
		{
			FMeshSectionInfo Info = StaticMesh->SectionInfoMap.Get(0, SectionIndex);
			Info.bEnableCollision = true;
			StaticMesh->SectionInfoMap.Set(0, SectionIndex, Info);
		}

		FString MeshCompNameX = FString::Printf(TEXT("Part_%d_Component"), j);
		UStaticMeshComponent* MeshComp = NewObject< UStaticMeshComponent>();
		
		(UStaticMeshComponent*)NewObject<UStaticMeshComponent>(BPPackage, *MeshCompNameX, RF_Transient);

		//MeshComp->StaticMesh = StaticMesh;

		MeshComp->SetStaticMesh(StaticMesh);

		FString MeshCompName = FString::Printf(TEXT("Part_%d"), j);
		//USCS_Node* MeshNode = Blueprint->SimpleConstructionScript->CreateNode(MeshComp, *MeshCompName);

		USCS_Node* MeshNode = Blueprint->SimpleConstructionScript->CreateNodeAndRenameComponent(MeshComp);

		if (RootNode) {
			RootNode->AddChildNode(MeshNode);
		}
		else {
			Blueprint->SimpleConstructionScript->AddNode(MeshNode);
			RootNode = MeshNode;
		}

		MeshComp->SetRelativeLocationAndRotation(part.position, FRotator(part.rotation));
		MeshComp->SetRelativeScale3D(part.scale);

		if (part.animPath.IsEmpty()) {
			MeshComp->SetMobility(EComponentMobility::Static);
		}
		else {
			MeshComp->SetMobility(EComponentMobility::Movable);
		}

		if (part.collisionType & Zsc::CollisionType::ModeMask) {
			MeshComp->SetCollisionResponseToAllChannels(ECR_Block);
			if (part.collisionType & Zsc::CollisionType::NoCameraCollide) {
				MeshComp->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			}
		}
		else {
			MeshComp->SetCollisionResponseToAllChannels(ECR_Ignore);
		}

		// Import any animations
		if (!part.animPath.IsEmpty())
		{
			FString EGName = FString::Printf(TEXT("Part_%d_EG"), j);
			UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *EGName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, EventGraph);

			UK2Node_Timeline* NodeTemplate = NewObject<UK2Node_Timeline>(EventGraph);
			FVector2D NodeLocation = EventGraph->GetGoodPlaceForNewNode();
			UK2Node_Timeline* TLNode = FEdGraphSchemaAction_K2NewNode::SpawnNodeFromTemplate<UK2Node_Timeline>(EventGraph, NodeTemplate, NodeLocation);
			UK2Node* TLNodeX = Cast<UK2Node>(TLNode);
			TLNode->TimelineName = *FString::Printf(TEXT("Part_%d_Anim"), j);

			Zmo anim(*(RoseBasePath + part.animPath));

			UTimelineTemplate* TLTmpl = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TLNode->TimelineName);
			TLTmpl->bLoop = true;
			TLTmpl->bAutoPlay = true;
			TLTmpl->TimelineLength = (float)anim.frameCount / (float)anim.framesPerSecond;

			FName RCurveName = *FString::Printf(TEXT("Curve_%d_Rot"), j);
			FName PCurveName = *FString::Printf(TEXT("Curve_%d_Pos"), j);
			FName SCurveName = *FString::Printf(TEXT("Curve_%d_Scale"), j);
			UCurveVector* RCurve = CreateCurveObject<UCurveVector>(BPPackage, RCurveName);
			UCurveVector* PCurve = CreateCurveObject<UCurveVector>(BPPackage, PCurveName);
			UCurveVector* SCurve = CreateCurveObject<UCurveVector>(BPPackage, SCurveName);
			bool UsesRotation = false;
			bool UsesPosition = false;
			bool UsesScale = false;

			for (int i = 0; i < anim.channels.Num(); ++i) {
				Zmo::Channel* channel = anim.channels[i];
				if (channel->index != 0) {
					UE_DEBUG_BREAK();
				}

				if (channel->type() == Zmo::ChannelType::Position) {
					UsesPosition = true;
					auto posChannel = (Zmo::PositionChannel*)channel;
					for (int k = 0; k < posChannel->frames.Num(); ++k) {
						const FVector& frame = posChannel->frames[k];
						if (k == 0 || frame.X != posChannel->frames[k - 1].X) {
							PCurve->FloatCurves[0].AddKey((float)k / (float)anim.framesPerSecond, frame.X);
						}
						if (k == 0 || frame.Y != posChannel->frames[k - 1].Y) {
							PCurve->FloatCurves[1].AddKey((float)k / (float)anim.framesPerSecond, frame.Y);
						}
						if (k == 0 || frame.Z != posChannel->frames[k - 1].Z) {
							PCurve->FloatCurves[2].AddKey((float)k / (float)anim.framesPerSecond, frame.Z);
						}
					}
				}
				else if (channel->type() == Zmo::ChannelType::Rotation) {
					UsesRotation = true;
					auto rotChannel = (Zmo::RotationChannel*)channel;
					FRotator prevFrame;
					for (int k = 0; k < rotChannel->frames.Num(); ++k) {
						FRotator frame = rotChannel->frames[k].Rotator();
						//if (j == 0 || frame.Pitch != prevFrame.Pitch) {
						RCurve->FloatCurves[0].AddKey((float)k / (float)anim.framesPerSecond, frame.Pitch, true);
						//}
						//if (j == 0 || frame.Yaw != prevFrame.Yaw) {
						RCurve->FloatCurves[1].AddKey((float)k / (float)anim.framesPerSecond, frame.Yaw, true);
						//}
						//if (j == 0 || frame.Roll != prevFrame.Roll) {
						RCurve->FloatCurves[2].AddKey((float)k / (float)anim.framesPerSecond, frame.Roll, true);
						//}
						prevFrame = frame;
					}
				}
				else if (channel->type() == Zmo::ChannelType::Scale) {
					UsesScale = true;
					auto scaleChannel = (Zmo::ScaleChannel*)channel;
					for (int k = 0; k < scaleChannel->frames.Num(); ++k) {
						const FVector& frame = scaleChannel->frames[k];
						if (k == 0 || frame.X != scaleChannel->frames[k - 1].X) {
							SCurve->FloatCurves[0].AddKey((float)k / (float)anim.framesPerSecond, frame.X);
						}
						if (k == 0 || frame.Y != scaleChannel->frames[k - 1].Y) {
							SCurve->FloatCurves[1].AddKey((float)k / (float)anim.framesPerSecond, frame.Y);
						}
						if (k == 0 || frame.Z != scaleChannel->frames[k - 1].Z) {
							SCurve->FloatCurves[2].AddKey((float)k / (float)anim.framesPerSecond, frame.Z);
						}
					}
				}
				else {
					UE_DEBUG_BREAK();
				}
			}

			if (UsesRotation) {
				FTTVectorTrack VTrack;
				VTrack.SetTrackName("Rotation", TLTmpl);
				VTrack.CurveVector = RCurve;
				TLTmpl->VectorTracks.Add(VTrack);
			}
			if (UsesPosition) {
				FTTVectorTrack VTrack;
				VTrack.SetTrackName("Position", TLTmpl);
				VTrack.CurveVector = PCurve;
				TLTmpl->VectorTracks.Add(VTrack);
			}
			if (UsesScale) {
				FTTVectorTrack VTrack;
				VTrack.SetTrackName("Scale", TLTmpl);
				VTrack.CurveVector = SCurve;
				TLTmpl->VectorTracks.Add(VTrack);
			}

			TLNode->ReconstructNode();

			UK2Node_VariableGet* GetNode = CreateVarGetNode(EventGraph, MeshNode->GetVariableName());

			UEdGraphPin* PrevExecPin = TLNode->GetUpdatePin();
			if (UsesRotation) {
				UK2Node_CallFunction* MakeRotNode = CreateCallFuncNode(EventGraph, TEXT("KismetMathLibrary"), TEXT("MakeRot"));
				UK2Node_CallFunction* BreakVecNode = CreateCallFuncNode(EventGraph, TEXT("KismetMathLibrary"), TEXT("BreakVector"));
				UK2Node_CallFunction* SetRotNode = CreateCallFuncNode<USceneComponent>(EventGraph, TEXT("SetRelativeRotation"));

				PrevExecPin->MakeLinkTo(SetRotNode->GetExecPin());
				PrevExecPin = SetRotNode->GetThenPin();

				GetNode->GetValuePin()->MakeLinkTo(SetRotNode->FindPin(TEXT("self")));
				TLNode->FindPin(TEXT("Rotation"))->MakeLinkTo(BreakVecNode->FindPin(TEXT("InVec")));
				BreakVecNode->FindPin(TEXT("X"))->MakeLinkTo(MakeRotNode->FindPin(TEXT("Pitch")));
				BreakVecNode->FindPin(TEXT("Y"))->MakeLinkTo(MakeRotNode->FindPin(TEXT("Yaw")));
				BreakVecNode->FindPin(TEXT("Z"))->MakeLinkTo(MakeRotNode->FindPin(TEXT("Roll")));
				//MakeRotNode->GetReturnValuePin()->MakeLinkTo(SetRotNode->FindPin(TEXT("NewRotation")));
			}

			if (UsesPosition) {
				UK2Node_CallFunction* SetPosNode = CreateCallFuncNode<USceneComponent>(EventGraph, TEXT("SetRelativeLocation"));
				//PrevExecPin->MakeLinkTo(SetPosNode->GetExecPin());
				//PrevExecPin = SetPosNode->GetThenPin();

				GetNode->GetValuePin()->MakeLinkTo(SetPosNode->FindPin(TEXT("self")));
				TLNode->FindPin(TEXT("Position"))->MakeLinkTo(SetPosNode->FindPin(TEXT("NewLocation")));
			}

			if (UsesScale) {
				UK2Node_CallFunction* SetScaleNode = CreateCallFuncNode<USceneComponent>(EventGraph, TEXT("SetRelativeScale"));
				PrevExecPin->MakeLinkTo(SetScaleNode->GetExecPin());
				PrevExecPin = SetScaleNode->GetThenPin();

				GetNode->GetValuePin()->MakeLinkTo(SetScaleNode->FindPin(TEXT("self")));
				TLNode->FindPin(TEXT("Scale"))->MakeLinkTo(SetScaleNode->FindPin(TEXT("NewScale3D")));
			}
		}
	}

	return Blueprint;
}

AActor* SpawnWorldModel(const FString& NewName, const FString& PackageName, const FString& AssetName, const FQuat& Rot, const FVector& Pos, const FVector& Scale) {
	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Name = *NewName;

	auto Model = GetExistingAsset<UBlueprint>(PackageName, AssetName);
	if (Model != NULL) {
		AActor* ModelAct = GWorld->SpawnActor<AActor>(Model->GeneratedClass, Pos, FRotator(Rot), SpawnInfo);
		ModelAct->SetActorScale3D(Scale);
		return ModelAct;
	}

	return NULL;
}

void CreateBrushForVolumeActor(AVolume* NewActor, UBrushBuilder* BrushBuilder)
{
	if (NewActor != NULL)
	{
		// this code builds a brush for the new actor
		NewActor->PreEditChange(NULL);

		NewActor->PolyFlags = 0;
		NewActor->Brush = NewObject<UModel>(NewActor, NAME_None, RF_Transactional);
		NewActor->Brush->Initialize(NULL, true);
		NewActor->Brush->Polys = NewObject<UPolys>(NewActor->Brush, NAME_None, RF_Transactional);
		//NewActor->BrushComponent->Brush = NewActor->Brush;
		NewActor->GetBrushComponent()->Brush = NewActor->Brush;

		if (BrushBuilder != nullptr)
		{
			NewActor->BrushBuilder = DuplicateObject<UBrushBuilder>(BrushBuilder, NewActor);
		}

		BrushBuilder->Build(NewActor->GetWorld(), NewActor);

		FBSPOps::csgPrepMovingBrush(NewActor);

		// Set the texture on all polys to NULL.  This stops invisible textures
		// dependencies from being formed on volumes.
		if (NewActor->Brush)
		{
			for (int32 poly = 0; poly < NewActor->Brush->Polys->Element.Num(); ++poly)
			{
				FPoly* Poly = &(NewActor->Brush->Polys->Element[poly]);
				Poly->Material = NULL;
			}
		}

		NewActor->PostEditChange();
	}
}

void FRoseImportModule::PluginButtonClicked()
{
	// GWarn->BeginSlowTask(NSLOCTEXT("RosePlugin", "SlowWorking", "We are working on importing the map!"), true);

	// Put your "OnButtonClicked" stuff here
	FText DialogText = FText::Format(
							LOCTEXT("PluginButtonDialogText", "Add code to {0} in {1} to override this button's actions, henek"),
							FText::FromString(TEXT("FRoseImportModule::PluginButtonClicked()")),
							FText::FromString(TEXT("RoseImport.cpp"))
					   );
	FMessageDialog::Open(EAppMsgType::Ok, DialogText);

	const bool IMPORT_BUILDINGS = true;
	const bool IMPORT_OBJECTS = true;
	const bool IMPORT_COLLISIONS = false;

	if (IMPORT_BUILDINGS) {
		Zsc meshsc(*(RoseBasePath + TEXT("3DDATA/JUNON/LIST_CNST_JDT.ZSC")));
		for (int32 i = 0; i < meshsc.models.Num(); ++i) {
			if (meshsc.models[i].parts.Num() > 0) {
				ImportWorldZscModel("JDTC", meshsc, i);
			}
		}

		UE_LOG(LogTemp, Log, TEXT("[IMPORT_BUILDINGS] ZSC loaded: %d"), meshsc.models.Num());
	}
	if (IMPORT_OBJECTS) {
		Zsc meshsd(*(RoseBasePath + TEXT("3DDATA/JUNON/LIST_DECO_JDT.ZSC")));
		for (int32 i = 0; i < meshsd.models.Num(); ++i) {
			if (meshsd.models[i].parts.Num() > 0) {
				ImportWorldZscModel("JDTD", meshsd, i);
			}
		}

		UE_LOG(LogTemp, Log, TEXT("[IMPORT_OBJECTS] ZSC loaded: %d"), meshsd.models.Num());
	}	

	const FString CnstPackageName = TEXT("/MAPS");

	const float HIM_HEIGHT_MIN = -25600;
	const float HIM_HEIGHT_MAX = +25600;
	const float UEL_HEIGHT_WMIN = -25600;
	const float UEL_HEIGHT_WMAX = +25600;
	const float UEL_HEIGHT_MIN = 0x0000;
	const float UEL_HEIGHT_MAX = 0x10000;
	const float HIM_HEIGHT_MID = (HIM_HEIGHT_MAX - HIM_HEIGHT_MIN) / 2;
	const float HIM_HEIGHT_MUL = (UEL_HEIGHT_MAX - UEL_HEIGHT_MIN) / (HIM_HEIGHT_MAX - HIM_HEIGHT_MIN);
	const float UEL_ZSCALE = (UEL_HEIGHT_WMAX - UEL_HEIGHT_WMIN) / (HIM_HEIGHT_MAX - HIM_HEIGHT_MIN);

	int startX = 31;
	int startY = 30;
	int endX = 34;
	int endY = 33;

	uint32 RoseSizeX = 4 * 16 * (endX - startX + 1);
	uint32 RoseSizeY = 4 * 16 * (endY - startY + 1);
	uint32 SizeX = (RoseSizeX / 63 + 1) * 63 + 1;
	uint32 SizeY = (RoseSizeY / 63 + 1) * 63 + 1;
	uint32 TileSizeX = SizeX;
	uint32 TileSizeY = SizeY;

	TArray<uint16> Data;
	Data.AddZeroed(SizeX * SizeY);
	for (int i = 0; i < Data.Num(); ++i) {
		Data[i] = 0x8000;
	}

	TArray<uint8> WeightData[8];
	for (int32 i = 0; i < 8; ++i) {
		WeightData[i].AddZeroed(TileSizeX * TileSizeY);
	}

	float MinHeight = +1000000;
	float MaxHeight = -1000000;
	for (int iy = startY; iy <= endY; ++iy) {
		for (int ix = startX; ix <= endX; ++ix) {
			int outTileX = (ix - startX) * 16;
			int outTileY = (iy - startY) * 16;
			int outBaseX = (ix - startX) * 64;
			int outBaseY = (iy - startY) * 64;

			FString TilPath = FString::Printf(TEXT("3DDATA/MAPS/JUNON/JDT01/%d_%d.til"), ix, iy);
			Til tilData(*(RoseBasePath + TilPath));

			for (int32 sy = 0; sy < 16; ++sy) {
				for (int32 sx = 0; sx < 16; ++sx) {
					int32 BrushIdx = tilData.Data[sy * 16 + sx].Brush;
					check(BrushIdx >= 0 && BrushIdx < 8);

					for (int32 py = 0; py < 5; ++py) {
						for (int32 px = 0; px < 5; ++px) {
							int32 PixelX = (outTileX + sx) * 4 + px;
							int32 PixelY = (outTileY + sy) * 4 + py;

							WeightData[BrushIdx][PixelY * TileSizeX + PixelX] = 50;
						}
					}
				}
			}

			FString HimPath = FString::Printf(TEXT("3DDATA/MAPS/JUNON/JDT01/%d_%d.him"), ix, iy);
			Him himData(*(RoseBasePath + HimPath));

			for (int sy = 0; sy < 65; ++sy) {
				for (int sx = 0; sx < 65; ++sx) {
					int outIdx = (outBaseY + sy) * SizeX + (outBaseX + sx);
					float hmValue = himData.heights[sy * 65 + sx];
					float ueValue = FMath::Clamp(hmValue + 25600.0f, 0.0f, 51200.0f) / 51200.0f * 65535.0f;

					Data[outIdx] = ueValue;

					if (hmValue < MinHeight) {
						MinHeight = hmValue;
					}
					if (hmValue > MaxHeight) {
						MaxHeight = hmValue;
					}
				}
			}


			FString IfoPath = FString::Printf(TEXT("3DDATA/MAPS/JUNON/JDT01/%d_%d.ifo"), ix, iy);
			Ifo ifoData(*(RoseBasePath + IfoPath));

			if (IMPORT_BUILDINGS) {
				for (int32 i = 0; i < ifoData.Buildings.Num(); ++i) {
					const Ifo::FBuildingBlock& obj = ifoData.Buildings[i];
					FString ObjName = FString::Printf(TEXT("Bldg_%d_%d_%d"), ix, iy, i);
					FString AssetName = FString::Printf(TEXT("JDTC_%d"), obj.ObjectID);
					AActor* ObjActor = SpawnWorldModel(ObjName, CnstPackageName, AssetName, obj.Rotation, obj.Position, obj.Scale);
				}
			}
			if (IMPORT_OBJECTS) {
				for (int32 i = 0; i < ifoData.Objects.Num(); ++i) {
					const Ifo::FObjectBlock& obj = ifoData.Objects[i];
					FString ObjName = FString::Printf(TEXT("Deco_%d_%d_%d"), ix, iy, i);
					FString AssetName = FString::Printf(TEXT("JDTD_%d"), obj.ObjectID);
					AActor* ObjActor = SpawnWorldModel(ObjName, CnstPackageName, AssetName, obj.Rotation, obj.Position, obj.Scale);
					if (ObjActor) {
						ObjActor->SetActorScale3D(obj.Scale);
					}

				}
			}
			if (IMPORT_COLLISIONS) {
				for (int32 i = 0; i < ifoData.Collisions.Num(); ++i) {
					const Ifo::FCollisionBlock& obj = ifoData.Collisions[i];

					FVector ColSize(120.0f * obj.Scale.X, 6.8f * obj.Scale.Y, 252.2f * obj.Scale.Z);
					FVector RecenterPos =
						FRotationTranslationMatrix(FRotator(obj.Rotation), FVector::ZeroVector)
						.TransformPosition(FVector(0, 0, -ColSize.Z / 2));

					FActorSpawnParameters SpawnInfo;
					SpawnInfo.Name = *FString::Printf(TEXT("Collision_%d_%d_%d"), ix, iy, i);
					ABlockingVolume* ObjColl = GWorld->SpawnActor<ABlockingVolume>(
						obj.Position - RecenterPos, FRotator(obj.Rotation), SpawnInfo);

					if (ObjColl) {
						UCubeBuilder* Builder = NewObject<UCubeBuilder>(); // ConstructObject<UCubeBuilder>(UCubeBuilder::StaticClass());
						Builder->X = ColSize.X;
						Builder->Y = ColSize.Y;
						Builder->Z = ColSize.Z;
						CreateBrushForVolumeActor(ObjColl, Builder);


						
						ObjColl->GetBrushComponent()->BuildSimpleBrushCollision();
						if (ObjColl->GetBrushComponent()->IsPhysicsStateCreated()) {
							ObjColl->GetBrushComponent()->RecreatePhysicsState();
						}

						ObjColl->GetBrushComponent()->SetCollisionResponseToAllChannels(ECR_Block);
						ObjColl->GetBrushComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
						ObjColl->GetBrushComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("Imported map height bounds were: %f, %f"), MinHeight, MaxHeight);
	return;

	FVector Location = FVector(0, 0, 0);
	FRotator Rotation = FRotator(0, 0, 0);
	ALandscape* Landscape = GWorld->SpawnActor<ALandscape>(Location, Rotation);
	Landscape->PreEditChange(NULL);

	Landscape->SetActorScale3D(FVector(250.0f, 250.0f, 51200.0f / 51200.0f * 100.0f));
	UMaterial* LMaterial = LoadObject<UMaterial>(NULL, TEXT("/Game/ROSEImp/Terrain/Junon/JD_Material.JD_Material"), NULL, LOAD_None, NULL);
	Landscape->LandscapeMaterial = LMaterial;


	TArray<FLandscapeImportLayerInfo> LayerInfos;
	auto LayerNames = Landscape->GetLayersFromMaterial();
	for (int32 i = 0; i < LayerNames.Num(); ++i) {
		const FName& LayerName = LayerNames[i];

		FString LIPackageName = TEXT("/Layers");
		FString LayerObjectName = FString::Printf(TEXT("LayerInfo_%d"), i);

		UPackage* LIPackage = GetOrMakePackage(LIPackageName, LayerObjectName);
		ULandscapeLayerInfoObject* LIData = NewObject<ULandscapeLayerInfoObject>(LIPackage, *LayerObjectName, RF_Public | RF_Standalone | RF_Transactional);
		LIData->LayerName = LayerName;
		LIData->bNoWeightBlend = false;

		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(LIData);

		// Mark the package dirty...
		LIPackage->MarkPackageDirty();

		FLandscapeImportLayerInfo LayerInfo;
		if (LayerName.Compare(TEXT("Dirt")) == 0) {
			LayerInfo.LayerData = WeightData[0];
			//UE_LOG(RosePlugin, Log, TEXT("Found Dirt Layer!"));
		}
		else if (LayerName.Compare(TEXT("Grass1")) == 0) {
			LayerInfo.LayerData = WeightData[1];
			//UE_LOG(RosePlugin, Log, TEXT("Found Grass1 Layer!"));
		}
		else if (LayerName.Compare(TEXT("Grass2")) == 0) {
			LayerInfo.LayerData = WeightData[3];
			//UE_LOG(RosePlugin, Log, TEXT("Found Grass2 Layer!"));
		}
		else if (LayerName.Compare(TEXT("Rock")) == 0) {
			LayerInfo.LayerData = WeightData[5];
			//UE_LOG(RosePlugin, Log, TEXT("Found Rock Layer!"));
		}
		else {
			LayerInfo.LayerData = WeightData[7];
			//UE_LOG(RosePlugin, Log, TEXT("Found Unknown Layer (%s)!"), *(LayerName.ToString()));
		}
		LayerInfo.LayerName = LayerName;
		LayerInfo.LayerInfo = LIData;
		LayerInfos.Add(LayerInfo);
	}

	//ELandscapeImportAlphamapType p = ELandscapeImportAlphamapType::Additive;

	/* Landscape->Import(
		FGuid::NewGuid(), 
		0,
		0, 
		SizeX, 
		SizeY, 
		63, 
		1, 
		//63,
		Data.GetData(), 
		NULL, 
		LayerInfos,
		p
	);*/

	/*Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((SizeX * SizeY) / (2048 * 2048) + 1), (uint32)2);

	Landscape->SetActorLocation(FVector((startX - 32) * 16000 - 8000, (startY - 32) * 16000 - 8000, 0));
	Landscape->StaticLightingResolution = 4.0f;

	ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	LandscapeInfo->UpdateLayerInfoMap(Landscape);

	for (int32 i = 0; i < LayerInfos.Num(); i++)
	{
		if (LayerInfos[i].LayerInfo != NULL)
		{
			Landscape->EditorLayerSettings.Add(FLandscapeEditorLayerSettings(LayerInfos[i].LayerInfo));

			int32 LayerInfoIndex = LandscapeInfo->GetLayerInfoIndex(LayerInfos[i].LayerName);
			if (ensure(LayerInfoIndex != INDEX_NONE))
			{
				FLandscapeInfoLayerSettings& LayerSettings = LandscapeInfo->Layers[LayerInfoIndex];
				LayerSettings.LayerInfoObj = LayerInfos[i].LayerInfo;
			}
		}
	}

	Landscape->PostEditChange();

	for (auto Component : Landscape->LandscapeComponents) {
		Component->UpdateMaterialInstances();
	}*/
}

void FRoseImportModule::AddMenuExtension(FMenuBuilder& Builder)
{
	Builder.AddMenuEntry(FRoseImportCommands::Get().PluginAction);
}

void FRoseImportModule::AddToolbarExtension(FToolBarBuilder& Builder)
{
	Builder.AddToolBarButton(FRoseImportCommands::Get().PluginAction);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FRoseImportModule, RoseImport)