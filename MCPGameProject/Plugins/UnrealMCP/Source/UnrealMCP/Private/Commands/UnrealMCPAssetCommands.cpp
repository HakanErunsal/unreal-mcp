#include "Commands/UnrealMCPAssetCommands.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace UnrealMCPAssetPrivate
{
	TSharedPtr<FJsonObject> McpAssetError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Message);
		return Result;
	}

	UObject* McpLoadAnyObject(const FString& Path)
	{
		if (UObject* Found = StaticLoadObject(UObject::StaticClass(), nullptr, *Path))
		{
			return Found;
		}
		if (!Path.Contains(TEXT(".")))
		{
			return StaticLoadObject(UObject::StaticClass(), nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
		}
		return nullptr;
	}

	UClass* McpLoadAnyClass(const FString& Path)
	{
		if (UClass* Found = LoadObject<UClass>(nullptr, *Path))
		{
			return Found;
		}
		return FindFirstObject<UClass>(*Path, EFindFirstObjectOptions::None);
	}

	void McpSaveOwningAsset(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		if (UObject* Asset = Package->FindAssetInPackage())
		{
			UEditorAssetLibrary::SaveLoadedAsset(Asset, false);
		}
	}

	UEdGraph* McpResolveGraph(const TSharedPtr<FJsonObject>& Params)
	{
		FString Path;
		if (Params->TryGetStringField(TEXT("graph"), Path))
		{
			return Cast<UEdGraph>(McpLoadAnyObject(Path));
		}
		if (Params->TryGetStringField(TEXT("asset"), Path))
		{
			if (UObject* Asset = McpLoadAnyObject(Path))
			{
				TArray<UObject*> Objects;
				GetObjectsWithPackage(Asset->GetOutermost(), Objects, true);
				for (UObject* Object : Objects)
				{
					if (UEdGraph* Graph = Cast<UEdGraph>(Object))
					{
						return Graph;
					}
				}
			}
		}
		return nullptr;
	}

	UEdGraphNode* McpFindNode(UEdGraph* Graph, const FString& Name)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && (Node->GetName() == Name || Node->GetPathName() == Name))
			{
				return Node;
			}
		}
		return nullptr;
	}

	UEdGraphPin* McpFindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection FallbackDirection)
	{
		if (!Name.IsEmpty())
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinName.ToString() == Name)
				{
					return Pin;
				}
			}
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == FallbackDirection)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Walks a dotted path through struct properties, array elements written as Name[3], and object properties, which step into the object they point at. Returns the final property, the container holding it and the object that owns that container. */
	FProperty* McpResolvePropertyPath(UObject* Object, const FString& Path, void*& OutContainer, UObject*& OutOwner, FString& OutError)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		UStruct* Struct = Object->GetClass();
		void* Container = Object;
		UObject* Owner = Object;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			FString Name = Parts[Index];
			int32 ElementIndex = INDEX_NONE;
			int32 Bracket = INDEX_NONE;
			if (Name.FindChar(TEXT('['), Bracket) && Name.EndsWith(TEXT("]")))
			{
				ElementIndex = FCString::Atoi(*Name.Mid(Bracket + 1, Name.Len() - Bracket - 2));
				Name.LeftInline(Bracket);
			}
			FProperty* Prop = FindFProperty<FProperty>(Struct, *Name);
			if (!Prop)
			{
				OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Name, *Struct->GetName());
				return nullptr;
			}
			if (ElementIndex != INDEX_NONE)
			{
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
				if (!ArrayProp)
				{
					OutError = FString::Printf(TEXT("'%s' is not an array"), *Name);
					return nullptr;
				}
				FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
				if (!Helper.IsValidIndex(ElementIndex))
				{
					OutError = FString::Printf(TEXT("'%s' has no element %d"), *Name, ElementIndex);
					return nullptr;
				}
				// An array's inner property sits at offset zero, so the element itself serves as its container.
				Container = Helper.GetRawPtr(ElementIndex);
				Prop = ArrayProp->Inner;
			}
			if (Index == Parts.Num() - 1)
			{
				OutContainer = Container;
				OutOwner = Owner;
				return Prop;
			}
			void* Value = Prop->ContainerPtrToValuePtr<void>(Container);
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				Container = Value;
				Struct = StructProp->Struct;
				continue;
			}
			if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
			{
				UObject* Inner = ObjectProp->GetObjectPropertyValue(Value);
				if (!Inner)
				{
					OutError = FString::Printf(TEXT("'%s' is empty"), *Name);
					return nullptr;
				}
				Container = Inner;
				Owner = Inner;
				Struct = Inner->GetClass();
				continue;
			}
			OutError = FString::Printf(TEXT("'%s' is neither a struct nor an object"), *Name);
			return nullptr;
		}
		return nullptr;
	}

	bool McpParseObjectMap(const TSharedPtr<FJsonObject>& Params, TMap<UObject*, UObject*>& OutMap, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* MapObject = nullptr;
		if (!Params->TryGetObjectField(TEXT("map"), MapObject))
		{
			return true;
		}
		for (const auto& Pair : (*MapObject)->Values)
		{
			UObject* From = McpLoadAnyObject(Pair.Key);
			UObject* To = McpLoadAnyObject(Pair.Value->AsString());
			if (!From || !To)
			{
				OutError = FString::Printf(TEXT("Object not found: %s -> %s"), *Pair.Key, *Pair.Value->AsString());
				return false;
			}
			OutMap.Add(From, To);
			UBlueprint* FromBlueprint = Cast<UBlueprint>(From);
			UBlueprint* ToBlueprint = Cast<UBlueprint>(To);
			if (FromBlueprint && ToBlueprint)
			{
				if (FromBlueprint->GeneratedClass && ToBlueprint->GeneratedClass)
				{
					OutMap.Add(FromBlueprint->GeneratedClass, ToBlueprint->GeneratedClass);
					OutMap.Add(FromBlueprint->GeneratedClass->GetDefaultObject(), ToBlueprint->GeneratedClass->GetDefaultObject());
				}
				if (FromBlueprint->SkeletonGeneratedClass && ToBlueprint->SkeletonGeneratedClass)
				{
					OutMap.Add(FromBlueprint->SkeletonGeneratedClass, ToBlueprint->SkeletonGeneratedClass);
				}
			}
		}
		return true;
	}

	TArray<TPair<FString, FString>> McpParseTextReplacements(const TSharedPtr<FJsonObject>& Params)
	{
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
		return Replacements;
	}

	/** Applies every replacement to the string in place and reports whether anything changed. */
	bool McpReplaceText(FString& Text, const TArray<TPair<FString, FString>>& Replacements)
	{
		bool bChanged = false;
		for (const TPair<FString, FString>& Pair : Replacements)
		{
			bChanged |= Text.ReplaceInline(*Pair.Key, *Pair.Value, ESearchCase::CaseSensitive) > 0;
		}
		return bChanged;
	}

	int64 McpReplaceReferencesInPackage(UObject* Asset, const TMap<UObject*, UObject*>& Map)
	{
		if (Map.Num() == 0)
		{
			return 0;
		}
		TArray<UObject*> Objects;
		GetObjectsWithPackage(Asset->GetOutermost(), Objects, true);
		int64 Count = 0;
		for (UObject* Object : Objects)
		{
			FArchiveReplaceObjectRef<UObject> Replacer(Object, Map, EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
			Count += Replacer.GetCount();
		}
		return Count;
	}

	TSharedPtr<FJsonObject> McpDescribeNode(UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Node->GetName());
		Entry->SetStringField(TEXT("path"), Node->GetPathName());
		Entry->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Entry->SetNumberField(TEXT("x"), Node->NodePosX);
		Entry->SetNumberField(TEXT("y"), Node->NodePosY);
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
			TArray<TSharedPtr<FJsonValue>> Links;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode())
				{
					Links.Add(MakeShared<FJsonValueString>(Linked->GetOwningNode()->GetName() + TEXT(".") + Linked->PinName.ToString()));
				}
			}
			PinJson->SetArrayField(TEXT("linked"), Links);
			Pins.Add(MakeShared<FJsonValueObject>(PinJson));
		}
		Entry->SetArrayField(TEXT("pins"), Pins);
		return Entry;
	}
}

using namespace UnrealMCPAssetPrivate;

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("replace_object_references")) return ReplaceObjectReferences(Params);
	if (CommandType == TEXT("create_subobject")) return CreateSubobject(Params);
	if (CommandType == TEXT("export_properties")) return ExportProperties(Params);
	if (CommandType == TEXT("edgraph_describe")) return EdGraphDescribe(Params);
	if (CommandType == TEXT("edgraph_add_node")) return EdGraphAddNode(Params);
	if (CommandType == TEXT("edgraph_connect")) return EdGraphConnect(Params);
	if (CommandType == TEXT("edgraph_remove_node")) return EdGraphRemoveNode(Params);
	if (CommandType == TEXT("blueprint_retarget")) return BlueprintRetarget(Params);
	if (CommandType == TEXT("fixup_redirectors")) return FixupRedirectors(Params);
	if (CommandType == TEXT("list_objects")) return ListObjects(Params);
	if (CommandType == TEXT("rename_object")) return RenameObject(Params);
	return McpAssetError(FString::Printf(TEXT("Unknown asset command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::ReplaceObjectReferences(const TSharedPtr<FJsonObject>& Params)
{
	TMap<UObject*, UObject*> Map;
	FString Error;
	if (!McpParseObjectMap(Params, Map, Error))
	{
		return McpAssetError(Error);
	}
	if (Map.Num() == 0)
	{
		return McpAssetError(TEXT("map is required"));
	}

	TArray<TSharedPtr<FJsonValue>> Report;
	for (const TSharedPtr<FJsonValue>& Value : Params->GetArrayField(TEXT("assets")))
	{
		UObject* Asset = McpLoadAnyObject(Value->AsString());
		if (!Asset)
		{
			Report.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("missing %s"), *Value->AsString())));
			continue;
		}
		const int64 Count = McpReplaceReferencesInPackage(Asset, Map);
		if (Count > 0)
		{
			Asset->Modify();
			Asset->PostEditChange();
			McpSaveOwningAsset(Asset);
		}
		Report.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %lld"), *Value->AsString(), Count)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("replaced"), Report);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::CreateSubobject(const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = McpLoadAnyObject(Params->GetStringField(TEXT("object")));
	UClass* Class = McpLoadAnyClass(Params->GetStringField(TEXT("class")));
	if (!Object || !Class)
	{
		return McpAssetError(TEXT("object or class not found"));
	}
	void* Container = nullptr;
	UObject* Owner = nullptr;
	FString Error;
	FProperty* Prop = McpResolvePropertyPath(Object, Params->GetStringField(TEXT("property")), Container, Owner, Error);
	if (!Prop)
	{
		return McpAssetError(Error);
	}

	Object->Modify();
	Owner->Modify();
	UObject* Created = NewObject<UObject>(Owner, Class, NAME_None, RF_Transactional);
	if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
	{
		ObjectProp->SetObjectPropertyValue_InContainer(Container, Created);
	}
	else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop); ArrayProp && ArrayProp->Inner->IsA<FObjectPropertyBase>())
	{
		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
		const int32 Index = Helper.AddValue();
		CastField<FObjectPropertyBase>(ArrayProp->Inner)->SetObjectPropertyValue(Helper.GetRawPtr(Index), Created);
	}
	else
	{
		Created->MarkAsGarbage();
		return McpAssetError(TEXT("property is neither an object nor an array of objects"));
	}
	FPropertyChangedEvent Changed(Prop, EPropertyChangeType::ValueSet);
	Owner->PostEditChangeProperty(Changed);
	if (Owner != Object)
	{
		Object->PostEditChange();
	}
	McpSaveOwningAsset(Object);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), Created->GetPathName());
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::ExportProperties(const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = McpLoadAnyObject(Params->GetStringField(TEXT("object")));
	if (!Object)
	{
		return McpAssetError(TEXT("object not found"));
	}
	TSet<FString> Wanted;
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	if (Params->TryGetArrayField(TEXT("properties"), Names))
	{
		for (const TSharedPtr<FJsonValue>& Name : *Names)
		{
			Wanted.Add(Name->AsString());
		}
	}
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		if (Wanted.Num() > 0 && !Wanted.Contains(It->GetName()))
		{
			continue;
		}
		FString Text;
		It->ExportTextItem_InContainer(Text, Object, nullptr, Object, PPF_None);
		Props->SetStringField(It->GetName(), Text);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
	Result->SetObjectField(TEXT("properties"), Props);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::EdGraphDescribe(const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = McpResolveGraph(Params);
	if (!Graph)
	{
		return McpAssetError(TEXT("graph not found"));
	}
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(McpDescribeNode(Node)));
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::EdGraphAddNode(const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = McpResolveGraph(Params);
	UClass* Class = McpLoadAnyClass(Params->GetStringField(TEXT("class")));
	if (!Graph || !Class || !Class->IsChildOf(UEdGraphNode::StaticClass()))
	{
		return McpAssetError(TEXT("graph or node class not found"));
	}
	Graph->Modify();
	UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, Class, NAME_None, RF_Transactional);
	Node->CreateNewGuid();
	Node->NodePosX = static_cast<int32>(Params->GetNumberField(TEXT("x")));
	Node->NodePosY = static_cast<int32>(Params->GetNumberField(TEXT("y")));
	Graph->AddNode(Node, /*bUserAction*/ true, /*bSelectNewNode*/ false);
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	Graph->NotifyGraphChanged();
	McpSaveOwningAsset(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("node"), McpDescribeNode(Node));
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::EdGraphConnect(const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = McpResolveGraph(Params);
	if (!Graph)
	{
		return McpAssetError(TEXT("graph not found"));
	}
	UEdGraphNode* From = McpFindNode(Graph, Params->GetStringField(TEXT("from")));
	UEdGraphNode* To = McpFindNode(Graph, Params->GetStringField(TEXT("to")));
	if (!From || !To)
	{
		return McpAssetError(TEXT("node not found"));
	}
	FString FromPinName;
	FString ToPinName;
	Params->TryGetStringField(TEXT("from_pin"), FromPinName);
	Params->TryGetStringField(TEXT("to_pin"), ToPinName);
	UEdGraphPin* FromPin = McpFindPin(From, FromPinName, EGPD_Output);
	UEdGraphPin* ToPin = McpFindPin(To, ToPinName, EGPD_Input);
	if (!FromPin || !ToPin)
	{
		return McpAssetError(TEXT("pin not found"));
	}

	const TSet<UEdGraphNode*> Before(Graph->Nodes);
	Graph->Modify();
	const bool bConnected = Graph->GetSchema()->TryCreateConnection(FromPin, ToPin);
	Graph->NotifyGraphChanged();
	McpSaveOwningAsset(Graph);

	TArray<TSharedPtr<FJsonValue>> Added;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && !Before.Contains(Node))
		{
			Added.Add(MakeShared<FJsonValueObject>(McpDescribeNode(Node)));
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bConnected);
	Result->SetArrayField(TEXT("added"), Added);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::EdGraphRemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = McpResolveGraph(Params);
	UEdGraphNode* Node = Graph ? McpFindNode(Graph, Params->GetStringField(TEXT("node"))) : nullptr;
	if (!Node)
	{
		return McpAssetError(TEXT("node not found"));
	}
	Graph->Modify();
	Node->Modify();
	Graph->GetSchema()->BreakNodeLinks(*Node);
	Node->DestroyNode();
	Graph->NotifyGraphChanged();
	McpSaveOwningAsset(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::BlueprintRetarget(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(McpLoadAnyObject(Params->GetStringField(TEXT("blueprint"))));
	if (!Blueprint)
	{
		return McpAssetError(TEXT("Blueprint not found"));
	}
	TMap<UObject*, UObject*> Map;
	FString Error;
	if (!McpParseObjectMap(Params, Map, Error))
	{
		return McpAssetError(Error);
	}
	const TArray<TPair<FString, FString>> Replacements = McpParseTextReplacements(Params);

	Blueprint->Modify();
	const int64 References = McpReplaceReferencesInPackage(Blueprint, Map);

	int32 TextEdits = 0;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			Node->Modify();
			TextEdits += McpReplaceText(Node->NodeComment, Replacements) ? 1 : 0;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
				{
					TextEdits += McpReplaceText(Pin->DefaultValue, Replacements) ? 1 : 0;
				}
			}
		}
	}
	for (FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		TextEdits += McpReplaceText(Variable.DefaultValue, Replacements) ? 1 : 0;
	}

	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// A compile rebuilds the default object from the previous one, so class defaults are rewritten after it.
	if (Replacements.Num() > 0 && Blueprint->GeneratedClass)
	{
		UObject* Defaults = Blueprint->GeneratedClass->GetDefaultObject();
		Defaults->Modify();
		for (TFieldIterator<FProperty> It(Blueprint->GeneratedClass); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
			{
				continue;
			}
			FString Text;
			It->ExportTextItem_InContainer(Text, Defaults, nullptr, Defaults, PPF_None);
			if (McpReplaceText(Text, Replacements) && It->ImportText_InContainer(*Text, Defaults, Defaults, PPF_None))
			{
				++TextEdits;
			}
		}
		Defaults->PostEditChange();
	}

	McpSaveOwningAsset(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Blueprint->Status != BS_Error);
	Result->SetNumberField(TEXT("references"), static_cast<double>(References));
	Result->SetNumberField(TEXT("text_edits"), TextEdits);
	Result->SetStringField(TEXT("status"), Blueprint->Status == BS_Error ? TEXT("error") : (Blueprint->Status == BS_UpToDateWithWarnings ? TEXT("warnings") : TEXT("ok")));
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::FixupRedirectors(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Path));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
	TArray<FAssetData> Found;
	Registry.GetAssets(Filter, Found);

	TArray<UObjectRedirector*> Redirectors;
	for (const FAssetData& Data : Found)
	{
		if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Data.GetAsset()))
		{
			Redirectors.Add(Redirector);
		}
	}
	if (Redirectors.Num() > 0)
	{
		// Leaving the redirectors avoids the engine's delete prompt, which is modal; each one is deleted below once nothing references it.
		FAssetToolsModule::GetModule().Get().FixupReferencers(Redirectors, /*bCheckoutDialogPrompt*/ false, ERedirectFixupMode::LeaveFixedUpRedirectors);
		TArray<UObject*> Unreferenced;
		for (UObjectRedirector* Redirector : Redirectors)
		{
			TArray<FName> Referencers;
			Registry.GetReferencers(Redirector->GetOutermost()->GetFName(), Referencers);
			Referencers.Remove(Redirector->GetOutermost()->GetFName());
			if (Referencers.Num() == 0)
			{
				Unreferenced.AddUnique(Redirector);
			}
		}
		for (UObject* Redirector : Unreferenced)
		{
			UEditorAssetLibrary::DeleteLoadedAsset(Redirector);
		}
	}

	TArray<FAssetData> Left;
	Registry.GetAssets(Filter, Left);
	TArray<TSharedPtr<FJsonValue>> Remaining;
	for (const FAssetData& Data : Left)
	{
		Remaining.Add(MakeShared<FJsonValueString>(Data.PackageName.ToString()));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Remaining.Num() == 0);
	Result->SetNumberField(TEXT("fixed"), Redirectors.Num());
	Result->SetArrayField(TEXT("remaining"), Remaining);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::ListObjects(const TSharedPtr<FJsonObject>& Params)
{
	UObject* Asset = McpLoadAnyObject(Params->GetStringField(TEXT("asset")));
	if (!Asset)
	{
		return McpAssetError(TEXT("asset not found"));
	}
	FString Contains;
	Params->TryGetStringField(TEXT("contains"), Contains);
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Asset->GetOutermost(), Objects, true);
	TArray<TSharedPtr<FJsonValue>> Out;
	for (UObject* Object : Objects)
	{
		if (!Contains.IsEmpty() && !Object->GetName().Contains(Contains))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Object->GetPathName());
		Entry->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
		Out.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("objects"), Out);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::RenameObject(const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = McpLoadAnyObject(Params->GetStringField(TEXT("object")));
	const FString NewName = Params->GetStringField(TEXT("new_name"));
	if (!Object || NewName.IsEmpty())
	{
		return McpAssetError(TEXT("object not found or new_name empty"));
	}
	if (Object->IsAsset())
	{
		return McpAssetError(TEXT("object is an asset; rename assets through the asset tools"));
	}
	if (!Object->Rename(*NewName, nullptr, REN_Test))
	{
		return McpAssetError(TEXT("an object with that name already exists in the same outer"));
	}
	Object->Modify();
	Object->Rename(*NewName, nullptr, REN_DontCreateRedirectors);
	McpSaveOwningAsset(Object);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), Object->GetPathName());
	return Result;
}
