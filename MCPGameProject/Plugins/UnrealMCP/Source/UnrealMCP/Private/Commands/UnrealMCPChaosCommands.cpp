#include "Commands/UnrealMCPChaosCommands.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "Materials/MaterialInterface.h"
#include "PlanarCut.h"
#include "UObject/Package.h"

namespace UnrealMCPChaosPrivate
{
	TSharedPtr<FJsonObject> McpChaosError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Message);
		return Result;
	}

	bool McpReadVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Array) || Array->Num() < 3)
		{
			return false;
		}
		Out = FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
		return true;
	}

	bool McpReadPlaneAxis(const TSharedPtr<FJsonObject>& Object, const FString& Field, FVector& OutAxis, double& OutOffset)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object->TryGetArrayField(Field, Array) || Array->Num() < 4)
		{
			return false;
		}
		OutAxis = FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
		OutOffset = (*Array)[3]->AsNumber();
		return true;
	}

	TArray<FTransform3f> McpGlobalTransforms(const FGeometryCollection& Collection)
	{
		TArray<int32> All;
		All.Reserve(Collection.Transform.Num());
		for (int32 Index = 0; Index < Collection.Transform.Num(); ++Index)
		{
			All.Add(Index);
		}
		TArray<FTransform3f> Global;
		GeometryCollectionAlgo::GlobalMatrices(Collection.Transform, Collection.Parent, All, Global);
		return Global;
	}

	/** Rigid leaves, optionally only those whose bounds centre lies inside Within (collection space). */
	TArray<int32> McpSelectLeaves(const FGeometryCollection& Collection, const FBox* Within)
	{
		const TArray<FTransform3f> Global = McpGlobalTransforms(Collection);
		TArray<int32> Leaves;
		for (int32 TransformIndex = 0; TransformIndex < Collection.Transform.Num(); ++TransformIndex)
		{
			const int32 GeometryIndex = Collection.TransformToGeometryIndex[TransformIndex];
			if (GeometryIndex == INDEX_NONE || !Collection.IsRigid(TransformIndex))
			{
				continue;
			}
			if (Within)
			{
				const FVector Centre(Global[TransformIndex].TransformPosition(FVector3f(Collection.BoundingBox[GeometryIndex].GetCenter())));
				if (!Within->IsInsideOrOn(Centre))
				{
					continue;
				}
			}
			Leaves.Add(TransformIndex);
		}
		return Leaves;
	}

	/** Projects the UVs of every vertex used only by internal faces through U = A.p + C, V = B.p + D, p in collection space. */
	int32 McpProjectInternalUVs(FGeometryCollection& Collection, const FVector& A, double C, const FVector& B, double D)
	{
		const TArray<FTransform3f> Global = McpGlobalTransforms(Collection);
		const FVector N = FVector::CrossProduct(A, B).GetSafeNormal();
		const FVector AHat = A.GetSafeNormal();
		const FVector BHat = B.GetSafeNormal();
		const double ALength = A.Size();
		const double BLength = B.Size();

		const int32 NumVertices = Collection.Vertex.Num();
		TBitArray<> UsedByExternal(false, NumVertices);
		TArray<FVector> FaceNormalForVertex;
		FaceNormalForVertex.SetNumZeroed(NumVertices);
		TBitArray<> UsedByInternal(false, NumVertices);

		for (int32 Face = 0; Face < Collection.Indices.Num(); ++Face)
		{
			const FIntVector Tri = Collection.Indices[Face];
			if (!Collection.Internal[Face])
			{
				UsedByExternal[Tri.X] = true;
				UsedByExternal[Tri.Y] = true;
				UsedByExternal[Tri.Z] = true;
				continue;
			}
			const int32 Bone = Collection.BoneMap[Tri.X];
			const FVector P0(Global[Bone].TransformPosition(Collection.Vertex[Tri.X]));
			const FVector P1(Global[Bone].TransformPosition(Collection.Vertex[Tri.Y]));
			const FVector P2(Global[Bone].TransformPosition(Collection.Vertex[Tri.Z]));
			const FVector FaceNormal = FVector::CrossProduct(P1 - P0, P2 - P0);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = Tri[Corner];
				FaceNormalForVertex[Vertex] += FaceNormal;
				UsedByInternal[Vertex] = true;
			}
		}

		int32 Projected = 0;
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			if (!UsedByInternal[Vertex] || UsedByExternal[Vertex])
			{
				continue;
			}
			const FVector P(Global[Collection.BoneMap[Vertex]].TransformPosition(Collection.Vertex[Vertex]));
			const FVector Normal = FaceNormalForVertex[Vertex].GetSafeNormal();
			const double AlongA = FMath::Abs(Normal | AHat);
			const double AlongB = FMath::Abs(Normal | BHat);
			const double AlongN = FMath::Abs(Normal | N);

			double U = (A | P) + C;
			double V = (B | P) + D;
			if (AlongA >= AlongB && AlongA >= AlongN)
			{
				U += ALength * (N | P);
			}
			else if (AlongB >= AlongA && AlongB >= AlongN)
			{
				V += BLength * (N | P);
			}
			for (int32 Layer = 0; Layer < Collection.NumUVLayers(); ++Layer)
			{
				Collection.ModifyUV(Vertex, Layer) = FVector2f(static_cast<float>(U), static_cast<float>(V));
			}
			++Projected;
		}
		return Projected;
	}
}

TSharedPtr<FJsonObject> FUnrealMCPChaosCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("fracture_static_mesh")) return FractureStaticMesh(Params);
	return UnrealMCPChaosPrivate::McpChaosError(FString::Printf(TEXT("Unknown chaos command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPChaosCommands::FractureStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UnrealMCPChaosPrivate;

	FString MeshPath, OutPath;
	if (!Params->TryGetStringField(TEXT("mesh"), MeshPath) || !Params->TryGetStringField(TEXT("out"), OutPath))
	{
		return McpChaosError(TEXT("mesh and out are required"));
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
	if (!Mesh && !MeshPath.Contains(TEXT(".")))
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, *(MeshPath + TEXT(".") + FPackageName::GetShortName(MeshPath)));
	}
	if (!Mesh)
	{
		return McpChaosError(FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(OutPath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	UGeometryCollection* Asset = FindObject<UGeometryCollection>(Package, *AssetName);
	const bool bCreated = Asset == nullptr;
	if (bCreated)
	{
		Asset = NewObject<UGeometryCollection>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	}
	Asset->Modify();
	Asset->Reset();
	Asset->GeometrySource.Reset();

	TArray<UMaterialInterface*> Materials;
	TArray<TObjectPtr<UMaterialInterface>> SourceMaterials;
	for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
	{
		Materials.Add(Material.MaterialInterface);
		SourceMaterials.Add(Material.MaterialInterface);
	}
	Asset->GeometrySource.Emplace(FSoftObjectPath(Mesh), FTransform::Identity, SourceMaterials, false, false);
	if (!FGeometryCollectionEngineConversion::AppendStaticMesh(Mesh, Materials, FTransform::Identity, Asset, false, false, false, false))
	{
		return McpChaosError(TEXT("AppendStaticMesh failed"));
	}
	Asset->InitializeMaterials(false);

	FGeometryCollection& Collection = *Asset->GetGeometryCollection();
	if (FGeometryCollectionClusteringUtility::ContainsMultipleRootBones(&Collection))
	{
		FGeometryCollectionClusteringUtility::ClusterAllBonesUnderNewRoot(&Collection);
	}

	const int32 Seed = Params->HasField(TEXT("seed")) ? static_cast<int32>(Params->GetNumberField(TEXT("seed"))) : 0;
	const double Grout = Params->HasField(TEXT("grout")) ? Params->GetNumberField(TEXT("grout")) : 0.0;
	const double CollisionSpacing = Params->HasField(TEXT("collision_spacing")) ? Params->GetNumberField(TEXT("collision_spacing")) : 50.0;

	TOptional<FNoiseSettings> Noise;
	const TSharedPtr<FJsonObject>* NoiseObject = nullptr;
	if (Params->TryGetObjectField(TEXT("noise"), NoiseObject))
	{
		FNoiseSettings Settings;
		(*NoiseObject)->TryGetNumberField(TEXT("amplitude"), Settings.Amplitude);
		(*NoiseObject)->TryGetNumberField(TEXT("frequency"), Settings.Frequency);
		(*NoiseObject)->TryGetNumberField(TEXT("octaves"), Settings.Octaves);
		(*NoiseObject)->TryGetNumberField(TEXT("point_spacing"), Settings.PointSpacing);
		Noise = Settings;
	}

	TArray<TSharedPtr<FJsonValue>> StageReport;
	const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
	if (Params->TryGetArrayField(TEXT("stages"), Stages))
	{
		for (int32 StageIndex = 0; StageIndex < Stages->Num(); ++StageIndex)
		{
			const TSharedPtr<FJsonObject> Stage = (*Stages)[StageIndex]->AsObject();
			if (!Stage.IsValid())
			{
				return McpChaosError(FString::Printf(TEXT("stage %d is not an object"), StageIndex));
			}

			TArray<FPlane> Planes;
			const TArray<TSharedPtr<FJsonValue>>* PlaneValues = nullptr;
			if (Stage->TryGetArrayField(TEXT("planes"), PlaneValues))
			{
				for (const TSharedPtr<FJsonValue>& PlaneValue : *PlaneValues)
				{
					const TSharedPtr<FJsonObject> PlaneObject = PlaneValue->AsObject();
					FVector Point, Normal;
					if (!PlaneObject.IsValid() || !McpReadVector(PlaneObject->TryGetField(TEXT("point")), Point) || !McpReadVector(PlaneObject->TryGetField(TEXT("normal")), Normal))
					{
						return McpChaosError(FString::Printf(TEXT("stage %d has a plane without point and normal"), StageIndex));
					}
					Planes.Add(FPlane(Point, Normal.GetSafeNormal()));
				}
			}

			FBox Within(ForceInit);
			bool bHasWithin = false;
			const TArray<TSharedPtr<FJsonValue>>* WithinValues = nullptr;
			if (Stage->TryGetArrayField(TEXT("within"), WithinValues) && WithinValues->Num() == 2)
			{
				FVector Min, Max;
				if (McpReadVector((*WithinValues)[0], Min) && McpReadVector((*WithinValues)[1], Max))
				{
					Within = FBox(Min, Max);
					bHasWithin = true;
				}
			}

			const TArray<int32> Targets = McpSelectLeaves(Collection, bHasWithin ? &Within : nullptr);
			bool bStageNoise = true;
			Stage->TryGetBoolField(TEXT("noise"), bStageNoise);

			int32 Result = INDEX_NONE;
			if (Planes.Num() > 0 && Targets.Num() > 0)
			{
				FInternalSurfaceMaterials InternalSurfaceMaterials;
				if (bStageNoise)
				{
					InternalSurfaceMaterials.NoiseSettings = Noise;
				}
				Result = CutMultipleWithMultiplePlanes(Planes, InternalSurfaceMaterials, Collection, Targets, Grout, CollisionSpacing, Seed + StageIndex, FTransform::Identity, true, nullptr, true);
			}

			TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetNumberField(TEXT("planes"), Planes.Num());
			Report->SetNumberField(TEXT("targets"), Targets.Num());
			Report->SetNumberField(TEXT("first_new_geometry"), Result);
			StageReport.Add(MakeShared<FJsonValueObject>(Report));
		}
	}

	bool bFlatten = false;
	Params->TryGetBoolField(TEXT("flatten"), bFlatten);
	if (bFlatten)
	{
		TArray<int32> Roots;
		FGeometryCollectionClusteringUtility::GetRootBones(&Collection, Roots);
		for (int32 Root : Roots)
		{
			TArray<int32> Leaves;
			FGeometryCollectionClusteringUtility::GetLeafBones(&Collection, Root, true, Leaves);
			FGeometryCollectionClusteringUtility::ClusterBonesUnderExistingNode(&Collection, Root, Leaves);
		}
		FGeometryCollectionClusteringUtility::RemoveDanglingClusters(&Collection);
	}

	int32 ProjectedVertices = 0;
	const TSharedPtr<FJsonObject>* UVObject = nullptr;
	if (Params->TryGetObjectField(TEXT("internal_uv"), UVObject))
	{
		FVector A, B;
		double C = 0.0, D = 0.0;
		if (!McpReadPlaneAxis(*UVObject, TEXT("u"), A, C) || !McpReadPlaneAxis(*UVObject, TEXT("v"), B, D))
		{
			return McpChaosError(TEXT("internal_uv needs u and v as [x, y, z, offset]"));
		}
		ProjectedVertices = McpProjectInternalUVs(Collection, A, C, B, D);
	}

	FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(&Collection, -1);
	::GeometryCollection::GenerateTemporaryGuids(&Collection, 0, true);
	Asset->UpdateGeometryDependentProperties();
	Asset->InvalidateCollection();
	Asset->RebuildRenderData();

	if (bCreated)
	{
		FAssetRegistryModule::AssetCreated(Asset);
	}
	Asset->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(Asset, false);

	int32 Rigid = 0;
	for (int32 TransformIndex = 0; TransformIndex < Collection.Transform.Num(); ++TransformIndex)
	{
		Rigid += Collection.IsRigid(TransformIndex) && Collection.IsGeometry(TransformIndex) ? 1 : 0;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset"), Asset->GetPathName());
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetNumberField(TEXT("transforms"), Collection.Transform.Num());
	Result->SetNumberField(TEXT("pieces"), Rigid);
	Result->SetNumberField(TEXT("triangles"), Collection.Indices.Num());
	Result->SetNumberField(TEXT("internal_uv_vertices"), ProjectedVertices);
	Result->SetArrayField(TEXT("stages"), StageReport);
	return Result;
}
