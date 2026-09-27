#include "Commands/UnrealMCPAnimCommands.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "EditorAssetLibrary.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

namespace
{
	TSharedPtr<FJsonObject> AnimError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Message);
		return Result;
	}

	template <typename T>
	T* LoadAssetByPath(const FString& Path)
	{
		if (T* Found = LoadObject<T>(nullptr, *Path))
		{
			return Found;
		}
		return LoadObject<T>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}

	UClass* LoadClassByPath(const FString& Path)
	{
		if (UClass* Found = LoadObject<UClass>(nullptr, *Path))
		{
			return Found;
		}
		return FindFirstObject<UClass>(*Path, EFindFirstObjectOptions::None);
	}

	FString ExportProperty(const FProperty* Property, const void* Container, UObject* Owner)
	{
		FString Text;
		Property->ExportTextItem_InContainer(Text, Container, nullptr, Owner, PPF_None);
		return Text;
	}

	/** Copies every editable property the two classes share by name, round-tripping through text so structs and tags that differ only in their type name still carry across. */
	void CopySharedProperties(UObject* From, UObject* To, const TArray<TPair<FString, FString>>& Replacements, TArray<TSharedPtr<FJsonValue>>& OutLog)
	{
		for (TFieldIterator<FProperty> It(From->GetClass()); It; ++It)
		{
			const FProperty* Source = *It;
			if (Source->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
			{
				continue;
			}
			FProperty* Target = FindFProperty<FProperty>(To->GetClass(), Source->GetFName());
			if (!Target)
			{
				OutLog.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("dropped %s (no such property on %s)"), *Source->GetName(), *To->GetClass()->GetName())));
				continue;
			}
			FString Text = ExportProperty(Source, From, From);
			for (const TPair<FString, FString>& Pair : Replacements)
			{
				Text.ReplaceInline(*Pair.Key, *Pair.Value, ESearchCase::CaseSensitive);
			}
			if (!Target->ImportText_InContainer(*Text, To, To, PPF_None))
			{
				OutLog.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("could not import %s = %s"), *Source->GetName(), *Text)));
			}
		}
	}

	void SaveAsset(UObject* Asset)
	{
		Asset->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(Asset, false);
	}
}

TSharedPtr<FJsonObject> FUnrealMCPAnimCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("replace_skeleton")) return ReplaceSkeleton(Params);
	if (CommandType == TEXT("replace_notify_classes")) return ReplaceNotifyClasses(Params);
	if (CommandType == TEXT("describe_notifies")) return DescribeNotifies(Params);
	return AnimError(FString::Printf(TEXT("Unknown anim command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPAnimCommands::ReplaceSkeleton(const TSharedPtr<FJsonObject>& Params)
{
	USkeleton* Skeleton = LoadAssetByPath<USkeleton>(Params->GetStringField(TEXT("skeleton")));
	if (!Skeleton)
	{
		return AnimError(TEXT("Skeleton not found"));
	}
	const bool bConvertSpaces = Params->HasField(TEXT("convert_spaces")) && Params->GetBoolField(TEXT("convert_spaces"));

	TArray<TSharedPtr<FJsonValue>> Done;
	TArray<TSharedPtr<FJsonValue>> Errors;
	for (const TSharedPtr<FJsonValue>& Value : Params->GetArrayField(TEXT("assets")))
	{
		const FString Path = Value->AsString();
		UAnimationAsset* Asset = LoadAssetByPath<UAnimationAsset>(Path);
		if (!Asset)
		{
			Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("not an animation asset: %s"), *Path)));
			continue;
		}
		Asset->Modify();
		if (!Asset->ReplaceSkeleton(Skeleton, bConvertSpaces))
		{
			Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("ReplaceSkeleton refused: %s"), *Path)));
			continue;
		}
		Asset->PostEditChange();
		SaveAsset(Asset);
		Done.Add(MakeShared<FJsonValueString>(Path));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Errors.Num() == 0);
	Result->SetArrayField(TEXT("replaced"), Done);
	Result->SetArrayField(TEXT("errors"), Errors);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimCommands::ReplaceNotifyClasses(const TSharedPtr<FJsonObject>& Params)
{
	UAnimSequenceBase* Asset = LoadAssetByPath<UAnimSequenceBase>(Params->GetStringField(TEXT("asset")));
	if (!Asset)
	{
		return AnimError(TEXT("Animation not found"));
	}

	TMap<UClass*, UClass*> ClassMap;
	const TSharedPtr<FJsonObject>* MapObject = nullptr;
	if (Params->TryGetObjectField(TEXT("class_map"), MapObject))
	{
		for (const auto& Pair : (*MapObject)->Values)
		{
			UClass* From = LoadClassByPath(Pair.Key);
			UClass* To = LoadClassByPath(Pair.Value->AsString());
			if (!From || !To)
			{
				return AnimError(FString::Printf(TEXT("Class not found: %s -> %s"), *Pair.Key, *Pair.Value->AsString()));
			}
			ClassMap.Add(From, To);
		}
	}

	TArray<TPair<FString, FString>> Replacements;
	const TArray<TSharedPtr<FJsonValue>>* ReplaceArray = nullptr;
	if (Params->TryGetArrayField(TEXT("text_replace"), ReplaceArray))
	{
		for (const TSharedPtr<FJsonValue>& Entry : *ReplaceArray)
		{
			const TArray<TSharedPtr<FJsonValue>>& Pair = Entry->AsArray();
			if (Pair.Num() == 2)
			{
				Replacements.Emplace(Pair[0]->AsString(), Pair[1]->AsString());
			}
		}
	}

	Asset->Modify();
	TArray<TSharedPtr<FJsonValue>> Log;
	int32 Swapped = 0;
	for (FAnimNotifyEvent& Event : Asset->Notifies)
	{
		UObject* Old = Event.NotifyStateClass ? static_cast<UObject*>(Event.NotifyStateClass) : static_cast<UObject*>(Event.Notify);
		UClass* const* NewClass = Old ? ClassMap.Find(Old->GetClass()) : nullptr;
		if (!NewClass)
		{
			continue;
		}
		UObject* Replacement = NewObject<UObject>(Asset, *NewClass, NAME_None, RF_Transactional);
		CopySharedProperties(Old, Replacement, Replacements, Log);
		if (UAnimNotifyState* State = Cast<UAnimNotifyState>(Replacement))
		{
			Event.NotifyStateClass = State;
			Event.Notify = nullptr;
			Event.NotifyName = FName(*State->GetNotifyName());
		}
		else if (UAnimNotify* Notify = Cast<UAnimNotify>(Replacement))
		{
			Event.Notify = Notify;
			Event.NotifyStateClass = nullptr;
			Event.NotifyName = FName(*Notify->GetNotifyName());
		}
		else
		{
			Log.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s is neither a notify nor a notify state"), *(*NewClass)->GetName())));
			continue;
		}
		Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		++Swapped;
	}
	Asset->RefreshCacheData();
	Asset->PostEditChange();
	SaveAsset(Asset);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("swapped"), Swapped);
	Result->SetArrayField(TEXT("log"), Log);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAnimCommands::DescribeNotifies(const TSharedPtr<FJsonObject>& Params)
{
	UAnimSequenceBase* Asset = LoadAssetByPath<UAnimSequenceBase>(Params->GetStringField(TEXT("asset")));
	if (!Asset)
	{
		return AnimError(TEXT("Animation not found"));
	}
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FAnimNotifyEvent& Event : Asset->Notifies)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		UObject* Object = Event.NotifyStateClass ? static_cast<UObject*>(Event.NotifyStateClass) : static_cast<UObject*>(Event.Notify);
		Entry->SetStringField(TEXT("name"), Event.NotifyName.ToString());
		Entry->SetStringField(TEXT("class"), Object ? Object->GetClass()->GetPathName() : TEXT(""));
		Entry->SetNumberField(TEXT("track"), Event.TrackIndex);
		Entry->SetNumberField(TEXT("time"), Event.GetTime());
		Entry->SetNumberField(TEXT("duration"), Event.GetDuration());
		if (Object)
		{
			TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || It->GetOwnerClass() == UObject::StaticClass())
				{
					continue;
				}
				Props->SetStringField(It->GetName(), ExportProperty(*It, Object, Object));
			}
			Entry->SetObjectField(TEXT("properties"), Props);
		}
		Out.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("notifies"), Out);
	return Result;
}
