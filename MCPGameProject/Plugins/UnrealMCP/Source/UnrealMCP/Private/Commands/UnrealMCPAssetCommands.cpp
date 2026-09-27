#include "Commands/UnrealMCPAssetCommands.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EditorAssetLibrary.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
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

	/** Walks a dotted path through struct properties; returns the final property and the container holding it. */
	FProperty* McpResolvePropertyPath(UObject* Object, const FString& Path, void*& OutContainer, FString& OutError)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		UStruct* Struct = Object->GetClass();
		void* Container = Object;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			FProperty* Prop = FindFProperty<FProperty>(Struct, *Parts[Index]);
			if (!Prop)
			{
				OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Parts[Index], *Struct->GetName());
				return nullptr;
			}
			if (Index == Parts.Num() - 1)
			{
				OutContainer = Container;
				return Prop;
			}
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (!StructProp)
			{
				OutError = FString::Printf(TEXT("'%s' is not a struct"), *Parts[Index]);
				return nullptr;
			}
			Container = StructProp->ContainerPtrToValuePtr<void>(Container);
			Struct = StructProp->Struct;
		}
		return nullptr;
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
	return McpAssetError(FString::Printf(TEXT("Unknown asset command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPAssetCommands::ReplaceObjectReferences(const TSharedPtr<FJsonObject>& Params)
{
	TMap<UObject*, UObject*> Map;
	const TSharedPtr<FJsonObject>* MapObject = nullptr;
	if (!Params->TryGetObjectField(TEXT("map"), MapObject))
	{
		return McpAssetError(TEXT("map is required"));
	}
	for (const auto& Pair : (*MapObject)->Values)
	{
		UObject* From = McpLoadAnyObject(Pair.Key);
		UObject* To = McpLoadAnyObject(Pair.Value->AsString());
		if (!From || !To)
		{
			return McpAssetError(FString::Printf(TEXT("Object not found: %s -> %s"), *Pair.Key, *Pair.Value->AsString()));
		}
		Map.Add(From, To);
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
		TArray<UObject*> Objects;
		GetObjectsWithPackage(Asset->GetOutermost(), Objects, true);
		int64 Count = 0;
		for (UObject* Object : Objects)
		{
			FArchiveReplaceObjectRef<UObject> Replacer(Object, Map, EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
			Count += Replacer.GetCount();
		}
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
	FString Error;
	FProperty* Prop = McpResolvePropertyPath(Object, Params->GetStringField(TEXT("property")), Container, Error);
	if (!Prop)
	{
		return McpAssetError(Error);
	}

	Object->Modify();
	UObject* Created = NewObject<UObject>(Object, Class, NAME_None, RF_Transactional);
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
	Object->PostEditChangeProperty(Changed);
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
