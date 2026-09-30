#include "Commands/UnrealMCPGraphBuilder.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "BlueprintActionDatabase.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_GetSubsystem.h"
#include "InputAction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "AIGraphTypes.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraphSchema_BehaviorTree.h"

namespace UnrealMCPGraph
{
	TSharedPtr<FJsonObject> GraphError(const FString& Message)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Message);
	}

	/** Resolves a class from an object path, a Blueprint asset path, or a bare native class name. */
	UClass* ResolveClass(const FString& Name)
	{
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (Name.StartsWith(TEXT("/")))
		{
			if (UClass* Loaded = LoadObject<UClass>(nullptr, *Name))
			{
				return Loaded;
			}
			// A Blueprint asset path without the _C suffix.
			FString Path = Name;
			if (!Path.Contains(TEXT(".")))
			{
				Path = Path + TEXT(".") + FPackageName::GetShortName(Path);
			}
			if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path))
			{
				return BP->GeneratedClass;
			}
			return LoadObject<UClass>(nullptr, *(Path + TEXT("_C")));
		}
		if (UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}
		FString Stripped = Name;
		if (Stripped.Len() > 1 && (Stripped[0] == 'U' || Stripped[0] == 'A') && FChar::IsUpper(Stripped[1]))
		{
			Stripped.RightChopInline(1);
			return FindFirstObject<UClass>(*Stripped, EFindFirstObjectOptions::NativeFirst);
		}
		return nullptr;
	}

	UScriptStruct* ResolveStruct(const FString& Name)
	{
		if (Name.StartsWith(TEXT("/")))
		{
			return LoadObject<UScriptStruct>(nullptr, *Name);
		}
		FString Stripped = Name;
		if (Stripped.Len() > 1 && Stripped[0] == 'F' && FChar::IsUpper(Stripped[1]))
		{
			Stripped.RightChopInline(1);
		}
		return FindFirstObject<UScriptStruct>(*Stripped, EFindFirstObjectOptions::NativeFirst);
	}

	/** bool, int, int64, float, byte, name, string, text, vector, rotator, transform, color, object:<class>, class:<class>, softobject:<class>, softclass:<class>, struct:<struct>, enum:<enum>, array:<type>, set:<type>. */
	bool ParsePinType(const FString& In, FEdGraphPinType& Out, FString& OutError)
	{
		Out = FEdGraphPinType();
		FString Type = In.TrimStartAndEnd();
		if (Type.StartsWith(TEXT("array:")))
		{
			if (!ParsePinType(Type.Mid(6), Out, OutError)) return false;
			Out.ContainerType = EPinContainerType::Array;
			return true;
		}
		if (Type.StartsWith(TEXT("set:")))
		{
			if (!ParsePinType(Type.Mid(4), Out, OutError)) return false;
			Out.ContainerType = EPinContainerType::Set;
			return true;
		}
		if (Type.StartsWith(TEXT("map:")))
		{
			// map:<key>:<value>, where the key is a single word type (name, int, string, object:<class> is not supported as a key here).
			FString Rest = Type.Mid(4), KeyType, ValueType;
			if (!Rest.Split(TEXT(":"), &KeyType, &ValueType)) { OutError = TEXT("map needs map:<key>:<value>"); return false; }
			FEdGraphPinType ValuePin;
			if (!ParsePinType(KeyType, Out, OutError) || !ParsePinType(ValueType, ValuePin, OutError)) return false;
			Out.ContainerType = EPinContainerType::Map;
			Out.PinValueType = FEdGraphTerminalType::FromPinType(ValuePin);
			return true;
		}
		FString Head = Type, Arg;
		Type.Split(TEXT(":"), &Head, &Arg);
		Head = Head.ToLower();

		if (Head == TEXT("bool") || Head == TEXT("boolean")) { Out.PinCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (Head == TEXT("int") || Head == TEXT("integer")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int; return true; }
		if (Head == TEXT("int64")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int64; return true; }
		if (Head == TEXT("float") || Head == TEXT("double") || Head == TEXT("real")) { Out.PinCategory = UEdGraphSchema_K2::PC_Real; Out.PinSubCategory = UEdGraphSchema_K2::PC_Double; return true; }
		if (Head == TEXT("byte")) { Out.PinCategory = UEdGraphSchema_K2::PC_Byte; return true; }
		if (Head == TEXT("name")) { Out.PinCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (Head == TEXT("string")) { Out.PinCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (Head == TEXT("text")) { Out.PinCategory = UEdGraphSchema_K2::PC_Text; return true; }

		auto SetStruct = [&Out](UScriptStruct* S) { Out.PinCategory = UEdGraphSchema_K2::PC_Struct; Out.PinSubCategoryObject = S; return S != nullptr; };
		if (Head == TEXT("vector")) return SetStruct(TBaseStructure<FVector>::Get());
		if (Head == TEXT("rotator")) return SetStruct(TBaseStructure<FRotator>::Get());
		if (Head == TEXT("transform")) return SetStruct(TBaseStructure<FTransform>::Get());
		if (Head == TEXT("color") || Head == TEXT("linearcolor")) return SetStruct(TBaseStructure<FLinearColor>::Get());

		if (Head == TEXT("struct"))
		{
			if (SetStruct(ResolveStruct(Arg))) return true;
			OutError = FString::Printf(TEXT("Unknown struct '%s'"), *Arg);
			return false;
		}
		if (Head == TEXT("enum"))
		{
			UEnum* Enum = Arg.StartsWith(TEXT("/")) ? LoadObject<UEnum>(nullptr, *Arg) : FindFirstObject<UEnum>(*Arg, EFindFirstObjectOptions::NativeFirst);
			if (!Enum) { OutError = FString::Printf(TEXT("Unknown enum '%s'"), *Arg); return false; }
			Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
			Out.PinSubCategoryObject = Enum;
			return true;
		}
		if (Head == TEXT("object") || Head == TEXT("class") || Head == TEXT("softobject") || Head == TEXT("softclass") || Head == TEXT("interface"))
		{
			UClass* Class = ResolveClass(Arg);
			if (!Class) { OutError = FString::Printf(TEXT("Unknown class '%s'"), *Arg); return false; }
			Out.PinCategory = Head == TEXT("object") ? UEdGraphSchema_K2::PC_Object
				: Head == TEXT("class") ? UEdGraphSchema_K2::PC_Class
				: Head == TEXT("softobject") ? UEdGraphSchema_K2::PC_SoftObject
				: Head == TEXT("softclass") ? UEdGraphSchema_K2::PC_SoftClass
				: UEdGraphSchema_K2::PC_Interface;
			Out.PinSubCategoryObject = Class;
			return true;
		}
		OutError = FString::Printf(TEXT("Unknown type '%s'"), *In);
		return false;
	}

	UBlueprint* LoadBlueprint(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		FString Name;
		if (!Params->TryGetStringField(TEXT("blueprint"), Name))
		{
			OutError = TEXT("Missing 'blueprint'");
			return nullptr;
		}
		UBlueprint* BP = FUnrealMCPCommonUtils::FindBlueprint(Name);
		if (!BP)
		{
			OutError = FString::Printf(TEXT("Blueprint not found: %s"), *Name);
		}
		return BP;
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction)
	{
		auto Matches = [&](UEdGraphPin* Pin, bool bUseDisplay)
		{
			if (!Pin || Pin->bHidden || (Direction != EGPD_MAX && Pin->Direction != Direction)) return false;
			if (Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase)) return true;
			return bUseDisplay && Pin->GetDisplayName().ToString().Replace(TEXT(" "), TEXT("")).Equals(Name.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase);
		};
		for (UEdGraphPin* Pin : Node->Pins) if (Matches(Pin, false)) return Pin;
		for (UEdGraphPin* Pin : Node->Pins) if (Matches(Pin, true)) return Pin;
		// Hidden pins such as a self pin on a static call are still addressable when named exactly.
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase) && (Direction == EGPD_MAX || Pin->Direction == Direction)) return Pin;
		}
		return nullptr;
	}

	FString DescribePins(UEdGraphNode* Node)
	{
		TArray<FString> Parts;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			FString Type = Pin->PinType.PinCategory.ToString();
			if (Pin->PinType.PinSubCategoryObject.IsValid()) Type += TEXT(":") + Pin->PinType.PinSubCategoryObject->GetName();
			Parts.Add(FString::Printf(TEXT("%s%s(%s)"), Pin->Direction == EGPD_Input ? TEXT(">") : TEXT("<"), *Pin->PinName.ToString(), *Type));
		}
		return FString::Join(Parts, TEXT(" "));
	}

	/** "Class:Function" or "self:Function". */
	UFunction* ResolveFunction(UBlueprint* BP, const FString& Spec, FString& OutError)
	{
		FString ClassName, FuncName;
		if (!Spec.Split(TEXT(":"), &ClassName, &FuncName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			ClassName = TEXT("self");
			FuncName = Spec;
		}
		UClass* Class = nullptr;
		if (ClassName.Equals(TEXT("self"), ESearchCase::IgnoreCase))
		{
			// The compiled class first: a function graph rebuilt in this session leaves the skeleton's copy of it stale.
			if (BP->GeneratedClass && BP->GeneratedClass->FindFunctionByName(*FuncName))
			{
				Class = BP->GeneratedClass;
			}
			else
			{
				Class = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass : BP->GeneratedClass;
			}
		}
		else
		{
			// Another Blueprint's functions resolve on its generated class: its skeleton class is regenerated whenever a dependent compiles, which leaves a skeleton function pointer dangling.
			Class = ResolveClass(ClassName);
		}
		if (!Class)
		{
			OutError = FString::Printf(TEXT("Unknown class in '%s'"), *Spec);
			return nullptr;
		}
		UFunction* Func = Class->FindFunctionByName(*FuncName);
		if (!Func)
		{
			OutError = FString::Printf(TEXT("Function '%s' not found on %s"), *FuncName, *Class->GetName());
		}
		return Func;
	}

	void ReadPins(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TArray<TPair<FName, FEdGraphPinType>>& Out, TArray<FString>& Errors)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Field, Arr)) return;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> P = V->AsObject();
			FEdGraphPinType Type;
			FString Err;
			if (ParsePinType(P->GetStringField(TEXT("type")), Type, Err))
			{
				Out.Add({FName(*P->GetStringField(TEXT("name"))), Type});
			}
			else
			{
				Errors.Add(Err);
			}
		}
	}

	/** Sets a property by a dotted path. Steps through struct properties, array elements written as Name[3], and object properties, which step into the object they point at. */
	bool SetPropertyByPath(UObject* Object, const FString& Path, const FString& Value, FString& OutError)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		UStruct* Struct = Object->GetClass();
		void* Container = Object;
		UObject* Owner = Object;
		for (int32 i = 0; i < Parts.Num(); ++i)
		{
			FString Name = Parts[i];
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
				return false;
			}
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
			if (ElementIndex != INDEX_NONE)
			{
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
				if (!ArrayProp)
				{
					OutError = FString::Printf(TEXT("'%s' is not an array"), *Name);
					return false;
				}
				FScriptArrayHelper Helper(ArrayProp, ValuePtr);
				if (!Helper.IsValidIndex(ElementIndex))
				{
					OutError = FString::Printf(TEXT("'%s' has no element %d"), *Name, ElementIndex);
					return false;
				}
				ValuePtr = Helper.GetRawPtr(ElementIndex);
				Prop = ArrayProp->Inner;
			}
			if (i == Parts.Num() - 1)
			{
				if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop); ObjProp && Value.StartsWith(TEXT("/")))
				{
					UObject* Loaded = StaticLoadObject(ObjProp->PropertyClass, nullptr, *Value);
					if (!Loaded)
					{
						if (UClass* AsClass = ResolveClass(Value)) Loaded = AsClass;
					}
					ObjProp->SetObjectPropertyValue(ValuePtr, Loaded);
					return Loaded != nullptr;
				}
				if (!Prop->ImportText_Direct(*Value, ValuePtr, Owner, PPF_None))
				{
					OutError = FString::Printf(TEXT("Could not set %s to '%s'"), *Path, *Value);
					return false;
				}
				if (Owner != Object)
				{
					Owner->Modify();
					Owner->PostEditChange();
				}
				return true;
			}
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				Struct = StructProp->Struct;
				Container = ValuePtr;
				continue;
			}
			if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
			{
				UObject* Inner = ObjectProp->GetObjectPropertyValue(ValuePtr);
				if (!Inner)
				{
					OutError = FString::Printf(TEXT("'%s' is empty"), *Name);
					return false;
				}
				Struct = Inner->GetClass();
				Container = Inner;
				Owner = Inner;
				continue;
			}
			OutError = FString::Printf(TEXT("'%s' is neither a struct nor an object"), *Name);
			return false;
		}
		return false;
	}

	bool SetDefault(const UEdGraphSchema_K2* Schema, UEdGraphPin* Pin, const FString& Value)
	{
		const FName Cat = Pin->PinType.PinCategory;
		if (Cat == UEdGraphSchema_K2::PC_Class || Cat == UEdGraphSchema_K2::PC_SoftClass)
		{
			UClass* Class = ResolveClass(Value);
			Schema->TrySetDefaultObject(*Pin, Class);
			return Class != nullptr;
		}
		if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_SoftObject || Cat == UEdGraphSchema_K2::PC_Interface)
		{
			UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *Value);
			Schema->TrySetDefaultObject(*Pin, Obj);
			return Obj != nullptr;
		}
		Schema->TrySetDefaultValue(*Pin, Value);
		return true;
	}

	TSharedPtr<FJsonObject> Compile(UBlueprint* BP, TSharedPtr<FJsonObject> Result)
	{
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Msg : Log.Messages)
		{
			if (Msg->GetSeverity() == EMessageSeverity::Error || Msg->GetSeverity() == EMessageSeverity::Warning)
			{
				Messages.Add(MakeShared<FJsonValueString>(Msg->ToText().ToString()));
			}
		}
		Result->SetNumberField(TEXT("compile_errors"), Log.NumErrors);
		Result->SetNumberField(TEXT("compile_warnings"), Log.NumWarnings);
		Result->SetArrayField(TEXT("compile_messages"), Messages);
		UEditorAssetLibrary::SaveLoadedAsset(BP, false);
		return Result;
	}

	template <typename T>
	T* LoadOrCreateAsset(const FString& Path)
	{
		const FString ObjectPath = Path + TEXT(".") + FPackageName::GetShortName(Path);
		if (T* Existing = LoadObject<T>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*Path);
		T* Asset = NewObject<T>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}
}

using namespace UnrealMCPGraph;

TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("add_variables")) return AddVariables(Params);
	if (CommandType == TEXT("build_graph")) return BuildGraph(Params);
	if (CommandType == TEXT("build_blackboard")) return BuildBlackboard(Params);
	if (CommandType == TEXT("build_behavior_tree")) return BuildBehaviorTree(Params);
	if (CommandType == TEXT("describe_graph")) return DescribeGraph(Params);
	if (CommandType == TEXT("set_properties")) return SetProperties(Params);
	if (CommandType == TEXT("build_widget")) return BuildWidget(Params);
	if (CommandType == TEXT("recolor_comments")) return RecolorComments(Params);
	if (CommandType == TEXT("list_comments")) return ListComments(Params);
	if (CommandType == TEXT("describe_widget")) return DescribeWidget(Params);
	if (CommandType == TEXT("set_comment")) return SetComment(Params);
	return GraphError(FString::Printf(TEXT("Unknown graph command: %s"), *CommandType));
}

// Variables - {blueprint, variables:[{name, type, default, editable, expose_on_spawn, category, tooltip}]}
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::AddVariables(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UBlueprint* BP = LoadBlueprint(Params, Err);
	if (!BP) return GraphError(Err);

	TArray<TSharedPtr<FJsonValue>> Errors;
	const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
	if (Params->TryGetArrayField(TEXT("variables"), Vars))
	{
		for (const TSharedPtr<FJsonValue>& V : *Vars)
		{
			const TSharedPtr<FJsonObject> Def = V->AsObject();
			const FName Name(*Def->GetStringField(TEXT("name")));
			FEdGraphPinType Type;
			if (!ParsePinType(Def->GetStringField(TEXT("type")), Type, Err))
			{
				Errors.Add(MakeShared<FJsonValueString>(Name.ToString() + TEXT(": ") + Err));
				continue;
			}
			FString Default;
			Def->TryGetStringField(TEXT("default"), Default);
			if (FBlueprintEditorUtils::FindNewVariableIndex(BP, Name) == INDEX_NONE)
			{
				FBlueprintEditorUtils::AddMemberVariable(BP, Name, Type, Default);
			}
			else
			{
				FBlueprintEditorUtils::ChangeMemberVariableType(BP, Name, Type);
				const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Name);
				BP->NewVariables[Index].DefaultValue = Default;
			}
			bool bEditable = false;
			Def->TryGetBoolField(TEXT("editable"), bEditable);
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(BP, Name, !bEditable);
			bool bExpose = false;
			if (Def->TryGetBoolField(TEXT("expose_on_spawn"), bExpose) && bExpose)
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, Name, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
			}
			FString Category, Tooltip;
			if (Def->TryGetStringField(TEXT("category"), Category))
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, Name, nullptr, FText::FromString(Category), true);
			}
			if (Def->TryGetStringField(TEXT("tooltip"), Tooltip))
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, Name, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
			}
			const TSharedPtr<FJsonObject>* Meta = nullptr;
			if (Def->TryGetObjectField(TEXT("meta"), Meta))
			{
				for (const auto& KV : (*Meta)->Values)
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, Name, nullptr, FName(*KV.Key), KV.Value->AsString());
				}
			}
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Errors.Num() == 0);
	Result->SetArrayField(TEXT("errors"), Errors);
	if (Errors.Num() > 0) Result->SetStringField(TEXT("error"), Errors[0]->AsString());
	return Compile(BP, Result);
}

// Graph - {blueprint, graph, function:{inputs, outputs, pure}, clear, nodes:[...], links:[["a.pin","b.pin"]], defaults:{"node.pin":"value"}}
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::BuildGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UBlueprint* BP = LoadBlueprint(Params, Err);
	if (!BP) return GraphError(Err);

	// Components and functions declared earlier must exist on the skeleton class before nodes reference them.
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	FString GraphName = TEXT("EventGraph");
	Params->TryGetStringField(TEXT("graph"), GraphName);
	bool bClear = false;
	Params->TryGetBoolField(TEXT("clear"), bClear);
	const TSharedPtr<FJsonObject>* FunctionDef = nullptr;
	const bool bIsFunction = Params->TryGetObjectField(TEXT("function"), FunctionDef);

	TArray<FString> Errors;
	TMap<FString, UEdGraphNode*> Nodes;
	UEdGraph* Graph = nullptr;

	if (!bIsFunction)
	{
		// "graph_path" names a nested graph directly, such as an animation transition rule, which shares its name with every other rule.
		FString GraphPath;
		if (Params->TryGetStringField(TEXT("graph_path"), GraphPath))
		{
			Graph = LoadObject<UEdGraph>(nullptr, *GraphPath);
			if (!Graph) return GraphError(FString::Printf(TEXT("Graph not found at path: %s"), *GraphPath));
		}
		if (!Graph)
		{
			Graph = GraphName == TEXT("EventGraph") ? FBlueprintEditorUtils::FindEventGraph(BP) : nullptr;
		}
		if (!Graph)
		{
			for (UEdGraph* G : BP->UbergraphPages) if (G && G->GetName() == GraphName) Graph = G;
		}
		if (!Graph)
		{
			for (UEdGraph* G : BP->FunctionGraphs) if (G && G->GetName() == GraphName) Graph = G;
		}
		if (!Graph)
		{
			// An interface function the Blueprint implements lives on its interface description, not in FunctionGraphs.
			for (FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
			{
				for (UEdGraph* G : Interface.Graphs) if (G && G->GetName() == GraphName) Graph = G;
			}
		}
		if (!Graph) return GraphError(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		if (bClear)
		{
			TArray<UEdGraphNode*> Existing = Graph->Nodes;
			for (UEdGraphNode* N : Existing)
			{
				if (!Cast<UK2Node_FunctionEntry>(N)) FBlueprintEditorUtils::RemoveNode(BP, N, true);
			}
		}
		// "remove" names nodes already in the graph, as describe_graph lists them.
		const TArray<TSharedPtr<FJsonValue>>* RemoveList = nullptr;
		if (Params->TryGetArrayField(TEXT("remove"), RemoveList))
		{
			for (const TSharedPtr<FJsonValue>& Value : *RemoveList)
			{
				const FString NodeName = Value->AsString();
				TObjectPtr<UEdGraphNode>* Match = Graph->Nodes.FindByPredicate([&NodeName](const UEdGraphNode* N) { return N && N->GetName() == NodeName; });
				if (Match) FBlueprintEditorUtils::RemoveNode(BP, Match->Get(), true);
				else Errors.Add(FString::Printf(TEXT("remove: no node %s"), *NodeName));
			}
		}
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) Nodes.Add(TEXT("entry"), E);
		}
	}
	else
	{
		// An override of a parent function takes the parent's signature; its entry and result pins are the parent's, never authored here.
		bool bOverride = false;
		(*FunctionDef)->TryGetBoolField(TEXT("override"), bOverride);
		UFunction* ParentFunction = bOverride && BP->ParentClass ? BP->ParentClass->FindFunctionByName(FName(*GraphName)) : nullptr;
		if (bOverride && !ParentFunction) return GraphError(FString::Printf(TEXT("No parent function %s to override"), *GraphName));
		for (UEdGraph* G : BP->FunctionGraphs) if (G && G->GetName() == GraphName) Graph = G;
		if (Graph && bClear && bOverride)
		{
			TArray<UEdGraphNode*> Existing = Graph->Nodes;
			for (UEdGraphNode* N : Existing)
			{
				if (!Cast<UK2Node_FunctionEntry>(N) && !Cast<UK2Node_FunctionResult>(N)) FBlueprintEditorUtils::RemoveNode(BP, N, true);
			}
		}
		else if (Graph && bClear)
		{
			// Emptied in place: removing the graph and creating one under the same name crashed the editor.
			TArray<UEdGraphNode*> Existing = Graph->Nodes;
			for (UEdGraphNode* N : Existing)
			{
				if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N))
				{
					TArray<TSharedPtr<FUserPinInfo>> Pins = E->UserDefinedPins;
					for (const TSharedPtr<FUserPinInfo>& Pin : Pins) E->RemoveUserDefinedPin(Pin);
				}
				else
				{
					FBlueprintEditorUtils::RemoveNode(BP, N, true);
				}
			}
		}
		if (!Graph)
		{
			Graph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (ParentFunction)
			{
				// The editor's own Override action passes the function's owning class (BlueprintEditor.cpp, OnAddNewFunction path), which is what makes the entry and result take the parent's signature.
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, Graph, false, ParentFunction->GetOwnerClass());
			}
			else
			{
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, Graph, true, nullptr);
			}
		}
		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) Entry = E;
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) ResultNode = R;
		}
		TArray<TPair<FName, FEdGraphPinType>> Inputs, Outputs;
		if (!bOverride)
		{
			ReadPins(*FunctionDef, TEXT("inputs"), Inputs, Errors);
			ReadPins(*FunctionDef, TEXT("outputs"), Outputs, Errors);
		}
		for (const auto& In : Inputs) Entry->CreateUserDefinedPin(In.Key, In.Value, EGPD_Output, false);
		bool bPure = false;
		(*FunctionDef)->TryGetBoolField(TEXT("pure"), bPure);
		if (bPure) Entry->AddExtraFlags(FUNC_BlueprintPure);
		FString Tooltip;
		if ((*FunctionDef)->TryGetStringField(TEXT("tooltip"), Tooltip)) Entry->MetaData.ToolTip = FText::FromString(Tooltip);
		FString Category;
		if ((*FunctionDef)->TryGetStringField(TEXT("category"), Category)) Entry->MetaData.Category = FText::FromString(Category);
		if (Outputs.Num() > 0 && !ResultNode)
		{
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
			ResultNode = Creator.CreateNode(false);
			ResultNode->NodePosX = 1600;
			ResultNode->NodePosY = Entry->NodePosY;
			Creator.Finalize();
		}
		for (const auto& Out : Outputs) ResultNode->CreateUserDefinedPin(Out.Key, Out.Value, EGPD_Input, false);
		Entry->ReconstructNode();
		if (ResultNode) ResultNode->ReconstructNode();
		Nodes.Add(TEXT("entry"), Entry);
		if (ResultNode) Nodes.Add(TEXT("result"), ResultNode);
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UClass* SelfClass = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass : BP->GeneratedClass;

	const TArray<TSharedPtr<FJsonValue>>* NodeDefs = nullptr;
	Params->TryGetArrayField(TEXT("nodes"), NodeDefs);
	int32 AutoIndex = 0;
	if (NodeDefs)
	{
		for (const TSharedPtr<FJsonValue>& V : *NodeDefs)
		{
			const TSharedPtr<FJsonObject> Def = V->AsObject();
			const FString Id = Def->GetStringField(TEXT("id"));
			const FString Type = Def->GetStringField(TEXT("type")).ToLower();
			int32 X = 300 * (AutoIndex % 8), Y = 250 * (AutoIndex / 8);
			++AutoIndex;
			const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
			if (Def->TryGetArrayField(TEXT("pos"), Pos) && Pos->Num() == 2)
			{
				X = (int32)(*Pos)[0]->AsNumber();
				Y = (int32)(*Pos)[1]->AsNumber();
			}
			FString Name;
			Def->TryGetStringField(TEXT("name"), Name);
			UEdGraphNode* Node = nullptr;

			auto Place = [X, Y](UEdGraphNode* N) { N->NodePosX = X; N->NodePosY = Y; };

			if (Type == TEXT("event"))
			{
				UFunction* Func = BP->ParentClass ? BP->ParentClass->FindFunctionByName(*Name) : nullptr;
				if (!Func) { Errors.Add(FString::Printf(TEXT("%s: no event '%s' on parent"), *Id, *Name)); continue; }
				UClass* Owner = Func->GetOwnerClass();
				if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(BP, Owner, Func->GetFName()))
				{
					Node = Existing;
					Place(Node);
				}
				else
				{
					FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
					UK2Node_Event* N = Creator.CreateNode(false);
					N->EventReference.SetExternalMember(Func->GetFName(), Owner);
					N->bOverrideFunction = true;
					Place(N);
					Creator.Finalize();
					Node = N;
				}
			}
			else if (Type == TEXT("custom_event"))
			{
				FString Signature;
				if (Def->TryGetStringField(TEXT("signature"), Signature))
				{
					// "Class:Delegate" takes the parameter list from a multicast delegate, ready for a Bind node.
					FString ClassName, DelegateName;
					Signature.Split(TEXT(":"), &ClassName, &DelegateName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					UClass* Class = ResolveClass(ClassName);
					FMulticastDelegateProperty* Delegate = Class ? FindFProperty<FMulticastDelegateProperty>(Class, *DelegateName) : nullptr;
					if (!Delegate) { Errors.Add(FString::Printf(TEXT("%s: delegate '%s' not found"), *Id, *Signature)); continue; }
					Node = UK2Node_CustomEvent::CreateFromFunction(FVector2D(X, Y), Graph, Name, Delegate->SignatureFunction, false);
				}
				else
				{
					FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
					UK2Node_CustomEvent* N = Creator.CreateNode(false);
					N->CustomFunctionName = FName(*Name);
					Place(N);
					Creator.Finalize();
					TArray<TPair<FName, FEdGraphPinType>> Inputs;
					ReadPins(Def, TEXT("inputs"), Inputs, Errors);
					for (const auto& In : Inputs) N->CreateUserDefinedPin(In.Key, In.Value, EGPD_Output, false);
					Node = N;
				}
			}
			else if (Type == TEXT("component_event"))
			{
				const FString Component = Def->GetStringField(TEXT("component"));
				const FString DelegateName = Def->GetStringField(TEXT("delegate"));
				FObjectProperty* CompProp = FindFProperty<FObjectProperty>(SelfClass, *Component);
				FMulticastDelegateProperty* Delegate = CompProp ? FindFProperty<FMulticastDelegateProperty>(CompProp->PropertyClass, *DelegateName) : nullptr;
				if (!Delegate) { Errors.Add(FString::Printf(TEXT("%s: %s.%s not found"), *Id, *Component, *DelegateName)); continue; }
				FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
				UK2Node_ComponentBoundEvent* N = Creator.CreateNode(false);
				N->InitializeComponentBoundEventParams(CompProp, Delegate);
				Place(N);
				Creator.Finalize();
				Node = N;
			}
			else if (Type == TEXT("call"))
			{
				UFunction* Func = ResolveFunction(BP, Def->GetStringField(TEXT("function")), Err);
				if (!Func) { Errors.Add(Id + TEXT(": ") + Err); continue; }
				// The spawner picks the node class the palette would (array, commutative operator, and so on), which is what makes wildcard pins take a type.
				// Wildcard container functions need the palette's spawner to pick their node class and type their pins. Every other function is a plain call node, made directly: the spawner goes through the palette's template cache, which a play session can leave stale, and it turns math operators into promotable nodes whose pins ignore typed defaults.
				const UClass* Owner = Func->GetOwnerClass();
				const bool bNeedsSpawner = Func->HasMetaData(TEXT("ArrayParm")) || Func->HasMetaData(TEXT("MapParam")) || Func->HasMetaData(TEXT("SetParam")) || Func->HasMetaData(TEXT("CustomStructureParam"))
					|| (Owner && (Owner->GetName() == TEXT("KismetArrayLibrary") || Owner->GetName() == TEXT("BlueprintMapLibrary") || Owner->GetName() == TEXT("BlueprintSetLibrary")));
				if (bNeedsSpawner)
				{
					UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(Func);
					if (!Spawner) { Errors.Add(FString::Printf(TEXT("%s: no node spawner for %s"), *Id, *Func->GetName())); continue; }
					Node = Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(X, Y));
				}
				else
				{
					FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
					UK2Node_CallFunction* N = Creator.CreateNode(false);
					N->SetFromFunction(Func);
					Place(N);
					Creator.Finalize();
					Node = N;
				}
				if (!Node) { Errors.Add(FString::Printf(TEXT("%s: could not spawn %s"), *Id, *Func->GetName())); continue; }
			}
			else if (Type == TEXT("call_parent"))
			{
				// The parent's version of the function this graph overrides, or of a named one.
				FString FuncName = GraphName;
				Def->TryGetStringField(TEXT("function"), FuncName);
				UFunction* Func = BP->ParentClass ? BP->ParentClass->FindFunctionByName(FName(*FuncName)) : nullptr;
				if (!Func) { Errors.Add(Id + TEXT(": no parent function ") + FuncName); continue; }
				FGraphNodeCreator<UK2Node_CallParentFunction> Creator(*Graph);
				UK2Node_CallParentFunction* N = Creator.CreateNode(false);
				N->SetFromFunction(Func);
				Place(N);
				Creator.Finalize();
				Node = N;
			}
			else if (Type == TEXT("input_action"))
			{
				// An Enhanced Input action event, as the palette places it; the owning actor needs input enabled for it to fire.
				UInputAction* Action = LoadObject<UInputAction>(nullptr, *Def->GetStringField(TEXT("action")));
				if (!Action) { Errors.Add(Id + TEXT(": input action not found")); continue; }
				FGraphNodeCreator<UK2Node_EnhancedInputAction> Creator(*Graph);
				UK2Node_EnhancedInputAction* N = Creator.CreateNode(false);
				N->InputAction = Action;
				Place(N);
				Creator.Finalize();
				Node = N;
			}
			else if (Type == TEXT("subsystem_from_pc"))
			{
				// The engine's class for this node is not exported, so it is found by name and set up through its exported base.
				UClass* NodeClass = FindFirstObject<UClass>(TEXT("K2Node_GetSubsystemFromPC"), EFindFirstObjectOptions::NativeFirst);
				UClass* Subsystem = ResolveClass(Def->GetStringField(TEXT("class")));
				if (!NodeClass || !Subsystem) { Errors.Add(Id + TEXT(": subsystem node or class not found")); continue; }
				UK2Node_GetSubsystem* N = NewObject<UK2Node_GetSubsystem>(Graph, NodeClass);
				N->Initialize(Subsystem);
				N->CreateNewGuid();
				Place(N);
				Graph->AddNode(N, false, false);
				N->PostPlacedNewNode();
				N->AllocateDefaultPins();
				Node = N;
			}
			else if (Type == TEXT("async"))
			{
				// A latent Blueprint async action (a UBlueprintAsyncActionBase factory), with its delegate pins as exec outputs.
				UFunction* Func = ResolveFunction(BP, Def->GetStringField(TEXT("function")), Err);
				if (!Func) { Errors.Add(Id + TEXT(": ") + Err); continue; }
				FGraphNodeCreator<UK2Node_AsyncAction> Creator(*Graph);
				UK2Node_AsyncAction* N = Creator.CreateNode(false);
				N->InitializeProxyFromFunction(Func);
				Place(N);
				Creator.Finalize();
				Node = N;
			}
			else if (Type == TEXT("palette"))
			{
				// A factory function the palette offers under a node class of its own (an ability task, a gameplay task), placed through the palette's own spawner so the node class sets itself up.
				UFunction* Func = ResolveFunction(BP, Def->GetStringField(TEXT("function")), Err);
				if (!Func) { Errors.Add(Id + TEXT(": ") + Err); continue; }
				FString NodeClassName;
				Def->TryGetStringField(TEXT("node_class"), NodeClassName);
				UBlueprintNodeSpawner* Found = nullptr;
				for (const auto& Pair : FBlueprintActionDatabase::Get().GetAllActions())
				{
					for (UBlueprintNodeSpawner* Spawner : Pair.Value)
					{
						const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner);
						if (FuncSpawner && FuncSpawner->GetFunction() == Func && Spawner->NodeClass && (NodeClassName.IsEmpty() ? !Spawner->NodeClass->IsChildOf(UK2Node_CallFunction::StaticClass()) : Spawner->NodeClass->GetName() == NodeClassName))
						{
							Found = Spawner;
							break;
						}
					}
					if (Found) break;
				}
				if (!Found) { Errors.Add(FString::Printf(TEXT("%s: no palette entry for %s"), *Id, *Func->GetName())); continue; }
				Node = Found->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(X, Y));
				if (!Node) { Errors.Add(FString::Printf(TEXT("%s: could not spawn %s"), *Id, *Func->GetName())); continue; }
			}
			else if (Type == TEXT("get_var") || Type == TEXT("set_var"))
			{
				const FName Var(*Def->GetStringField(TEXT("var")));
				FString TargetClass;
				UClass* External = Def->TryGetStringField(TEXT("class"), TargetClass) ? ResolveClass(TargetClass) : nullptr;
				if (Type == TEXT("get_var"))
				{
					FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
					UK2Node_VariableGet* N = Creator.CreateNode(false);
					if (External) N->VariableReference.SetExternalMember(Var, External); else N->VariableReference.SetSelfMember(Var);
					Place(N);
					Creator.Finalize();
					Node = N;
				}
				else
				{
					FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
					UK2Node_VariableSet* N = Creator.CreateNode(false);
					if (External) N->VariableReference.SetExternalMember(Var, External); else N->VariableReference.SetSelfMember(Var);
					Place(N);
					Creator.Finalize();
					Node = N;
				}
			}
			else if (Type == TEXT("branch"))
			{
				FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
				Node = Creator.CreateNode(false);
				Place(Node);
				Creator.Finalize();
			}
			else if (Type == TEXT("sequence"))
			{
				FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
				UK2Node_ExecutionSequence* N = Creator.CreateNode(false);
				Place(N);
				Creator.Finalize();
				int32 Count = 2;
				Def->TryGetNumberField(TEXT("count"), Count);
				for (int32 i = 2; i < Count; ++i) N->AddInputPin();
				Node = N;
			}
			else if (Type == TEXT("cast"))
			{
				UClass* Target = ResolveClass(Def->GetStringField(TEXT("class")));
				if (!Target) { Errors.Add(FString::Printf(TEXT("%s: cast class not found"), *Id)); continue; }
				FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
				UK2Node_DynamicCast* N = Creator.CreateNode(false);
				N->TargetType = Target;
				Place(N);
				Creator.Finalize();
				bool bPure = false;
				if (Def->TryGetBoolField(TEXT("pure"), bPure) && bPure) N->SetPurity(true);
				Node = N;
			}
			else if (Type == TEXT("self"))
			{
				FGraphNodeCreator<UK2Node_Self> Creator(*Graph);
				Node = Creator.CreateNode(false);
				Place(Node);
				Creator.Finalize();
			}
			else if (Type == TEXT("macro"))
			{
				UBlueprint* Macros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
				UEdGraph* MacroGraph = nullptr;
				if (Macros) for (UEdGraph* G : Macros->MacroGraphs) if (G && G->GetName() == Name) MacroGraph = G;
				if (!MacroGraph) { Errors.Add(FString::Printf(TEXT("%s: macro '%s' not found"), *Id, *Name)); continue; }
				FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
				UK2Node_MacroInstance* N = Creator.CreateNode(false);
				N->SetMacroGraph(MacroGraph);
				Place(N);
				Creator.Finalize();
				Node = N;
			}
			else if (Type == TEXT("make_struct") || Type == TEXT("break_struct"))
			{
				UScriptStruct* Struct = ResolveStruct(Def->GetStringField(TEXT("struct")));
				if (!Struct) { Errors.Add(FString::Printf(TEXT("%s: struct not found"), *Id)); continue; }
				if (Type == TEXT("make_struct"))
				{
					FGraphNodeCreator<UK2Node_MakeStruct> Creator(*Graph);
					UK2Node_MakeStruct* N = Creator.CreateNode(false);
					N->StructType = Struct;
					Place(N);
					Creator.Finalize();
					Node = N;
				}
				else
				{
					FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Graph);
					UK2Node_BreakStruct* N = Creator.CreateNode(false);
					N->StructType = Struct;
					Place(N);
					Creator.Finalize();
					Node = N;
				}
			}
			else if (Type == TEXT("make_array"))
			{
				FGraphNodeCreator<UK2Node_MakeArray> Creator(*Graph);
				UK2Node_MakeArray* N = Creator.CreateNode(false);
				Place(N);
				Creator.Finalize();
				int32 Count = 1;
				Def->TryGetNumberField(TEXT("count"), Count);
				for (int32 i = 1; i < Count; ++i) N->AddInputPin();
				Node = N;
			}
			else if (Type == TEXT("select"))
			{
				FGraphNodeCreator<UK2Node_Select> Creator(*Graph);
				Node = Creator.CreateNode(false);
				Place(Node);
				Creator.Finalize();
			}
			else if (Type == TEXT("switch_enum"))
			{
				const FString EnumName = Def->GetStringField(TEXT("enum"));
				UEnum* Enum = EnumName.StartsWith(TEXT("/")) ? LoadObject<UEnum>(nullptr, *EnumName) : FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::NativeFirst);
				if (!Enum) { Errors.Add(FString::Printf(TEXT("%s: enum not found"), *Id)); continue; }
				FGraphNodeCreator<UK2Node_SwitchEnum> Creator(*Graph);
				UK2Node_SwitchEnum* N = Creator.CreateNode(false);
				N->SetEnum(Enum);
				Place(N);
				Creator.Finalize();
				Node = N;
			}
			else if (Type == TEXT("bind") || Type == TEXT("unbind") || Type == TEXT("create_delegate"))
			{
				if (Type == TEXT("unbind"))
				{
					FString ClassName, DelegateName;
					Def->GetStringField(TEXT("delegate")).Split(TEXT(":"), &ClassName, &DelegateName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					UClass* Class = ClassName.Equals(TEXT("self"), ESearchCase::IgnoreCase) ? SelfClass : ResolveClass(ClassName);
					FMulticastDelegateProperty* Delegate = Class ? FindFProperty<FMulticastDelegateProperty>(Class, *DelegateName) : nullptr;
					if (!Delegate) { Errors.Add(FString::Printf(TEXT("%s: delegate not found"), *Id)); continue; }
					FGraphNodeCreator<UK2Node_RemoveDelegate> Creator(*Graph);
					UK2Node_RemoveDelegate* N = Creator.CreateNode(false);
					N->SetFromProperty(Delegate, Class == SelfClass, Class);
					Place(N);
					Creator.Finalize();
					Node = N;
				}
				else if (Type == TEXT("bind"))
				{
					// "Class:Delegate"; the target pin takes the object that owns the delegate.
					FString ClassName, DelegateName;
					Def->GetStringField(TEXT("delegate")).Split(TEXT(":"), &ClassName, &DelegateName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					UClass* Class = ClassName.Equals(TEXT("self"), ESearchCase::IgnoreCase) ? SelfClass : ResolveClass(ClassName);
					FMulticastDelegateProperty* Delegate = Class ? FindFProperty<FMulticastDelegateProperty>(Class, *DelegateName) : nullptr;
					if (!Delegate) { Errors.Add(FString::Printf(TEXT("%s: delegate not found"), *Id)); continue; }
					FGraphNodeCreator<UK2Node_AddDelegate> Creator(*Graph);
					UK2Node_AddDelegate* N = Creator.CreateNode(false);
					N->SetFromProperty(Delegate, Class == SelfClass, Class);
					Place(N);
					Creator.Finalize();
					Node = N;
				}
				else
				{
					FGraphNodeCreator<UK2Node_CreateDelegate> Creator(*Graph);
					UK2Node_CreateDelegate* N = Creator.CreateNode(false);
					Place(N);
					Creator.Finalize();
					N->SetFunction(FName(*Name));
					Node = N;
				}
			}
			else if (Type == TEXT("return"))
			{
				FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
				UK2Node_FunctionResult* N = Creator.CreateNode(false);
				Place(N);
				Creator.Finalize();
				N->ReconstructNode();
				Node = N;
			}
			else if (Type == TEXT("comment"))
			{
				UEdGraphNode_Comment* N = NewObject<UEdGraphNode_Comment>(Graph);
				N->CreateNewGuid();
				N->NodeComment = Def->GetStringField(TEXT("text"));
				N->CommentColor = FLinearColor::Black;
				FString ColorText;
				if (Def->TryGetStringField(TEXT("color"), ColorText)) N->CommentColor.InitFromString(ColorText);
				const TArray<TSharedPtr<FJsonValue>>* Size = nullptr;
				N->NodeWidth = 600;
				N->NodeHeight = 300;
				if (Def->TryGetArrayField(TEXT("size"), Size) && Size->Num() == 2)
				{
					N->NodeWidth = (int32)(*Size)[0]->AsNumber();
					N->NodeHeight = (int32)(*Size)[1]->AsNumber();
				}
				Place(N);
				Graph->AddNode(N, false, false);
				Node = N;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("%s: unknown node type '%s'"), *Id, *Type));
				continue;
			}

			if (Node)
			{
				Nodes.Add(Id, Node);
			}
		}
	}

	auto ResolveRef = [&](const FString& Ref, EEdGraphPinDirection Dir, FString& OutErr) -> UEdGraphPin*
	{
		FString NodeId, PinName;
		if (!Ref.Split(TEXT("."), &NodeId, &PinName))
		{
			OutErr = FString::Printf(TEXT("Bad pin reference '%s'"), *Ref);
			return nullptr;
		}
		// "@K2Node_CallFunction_7" names a node already in the graph, as describe_graph lists it.
		if (NodeId.StartsWith(TEXT("@")) && !Nodes.Contains(NodeId))
		{
			for (UEdGraphNode* Existing : Graph->Nodes)
			{
				if (Existing && Existing->GetName() == NodeId.Mid(1)) Nodes.Add(NodeId, Existing);
			}
		}
		UEdGraphNode** Found = Nodes.Find(NodeId);
		if (!Found)
		{
			OutErr = FString::Printf(TEXT("Unknown node '%s'"), *NodeId);
			return nullptr;
		}
		UEdGraphPin* Pin = FindPin(*Found, PinName, Dir);
		if (!Pin)
		{
			OutErr = FString::Printf(TEXT("Pin '%s' not on %s. Pins: %s"), *PinName, *NodeId, *DescribePins(*Found));
		}
		return Pin;
	};

	const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
	Params->TryGetObjectField(TEXT("defaults"), DefaultsObj);
	TArray<TPair<FString, FString>> PendingDefaults;
	if (DefaultsObj)
	{
		for (const auto& KV : (*DefaultsObj)->Values) PendingDefaults.Add({KV.Key, KV.Value->AsString()});
	}
	// Class and object defaults can reshape a node (a class pin retypes its outputs), so they apply before links.
	auto ApplyDefaults = [&](bool bFinal)
	{
		for (int32 i = PendingDefaults.Num() - 1; i >= 0; --i)
		{
			FString PinErr;
			UEdGraphPin* Pin = ResolveRef(PendingDefaults[i].Key, EGPD_Input, PinErr);
			if (!Pin)
			{
				if (bFinal) Errors.Add(PinErr);
				continue;
			}
			if (!SetDefault(Schema, Pin, PendingDefaults[i].Value)) Errors.Add(FString::Printf(TEXT("%s: could not resolve '%s'"), *PendingDefaults[i].Key, *PendingDefaults[i].Value));
			Pin->GetOwningNode()->PinDefaultValueChanged(Pin);
			PendingDefaults.RemoveAt(i);
		}
	};
	ApplyDefaults(false);

	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	if (Params->TryGetArrayField(TEXT("links"), Links))
	{
		for (const TSharedPtr<FJsonValue>& L : *Links)
		{
			const TArray<TSharedPtr<FJsonValue>>& Pair = L->AsArray();
			if (Pair.Num() != 2) continue;
			FString ErrA, ErrB;
			UEdGraphPin* A = ResolveRef(Pair[0]->AsString(), EGPD_Output, ErrA);
			UEdGraphPin* B = ResolveRef(Pair[1]->AsString(), EGPD_Input, ErrB);
			if (!A || !B)
			{
				Errors.Add(!A ? ErrA : ErrB);
				continue;
			}
			const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
			if (!Schema->TryCreateConnection(A, B))
			{
				Errors.Add(FString::Printf(TEXT("Link %s -> %s refused: %s"), *Pair[0]->AsString(), *Pair[1]->AsString(), *Response.Message.ToString()));
			}
		}
	}
	ApplyDefaults(true);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> PinInfo = MakeShared<FJsonObject>();
	bool bVerbose = false;
	Params->TryGetBoolField(TEXT("verbose"), bVerbose);
	if (bVerbose)
	{
		for (const auto& KV : Nodes) PinInfo->SetStringField(KV.Key, DescribePins(KV.Value));
		Result->SetObjectField(TEXT("pins"), PinInfo);
	}
	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& E : Errors) ErrorValues.Add(MakeShared<FJsonValueString>(E));
	Result->SetArrayField(TEXT("errors"), ErrorValues);
	Result->SetNumberField(TEXT("nodes"), Nodes.Num());
	Result->SetBoolField(TEXT("success"), true);
	return Compile(BP, Result);
}

// Blackboard - {path, keys:[{name, type, base_class, instance_synced}]}
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::BuildBlackboard(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	UBlackboardData* BB = LoadOrCreateAsset<UBlackboardData>(Path);
	BB->Modify();
	BB->Keys.Reset();
	TArray<TSharedPtr<FJsonValue>> Errors;
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (Params->TryGetArrayField(TEXT("keys"), Keys))
	{
		for (const TSharedPtr<FJsonValue>& V : *Keys)
		{
			const TSharedPtr<FJsonObject> Def = V->AsObject();
			const FString Type = Def->GetStringField(TEXT("type")).ToLower();
			FBlackboardEntry Entry;
			Entry.EntryName = FName(*Def->GetStringField(TEXT("name")));
			FString BaseClass;
			Def->TryGetStringField(TEXT("base_class"), BaseClass);
			if (Type == TEXT("object"))
			{
				UBlackboardKeyType_Object* K = NewObject<UBlackboardKeyType_Object>(BB);
				K->BaseClass = BaseClass.IsEmpty() ? UObject::StaticClass() : ResolveClass(BaseClass);
				Entry.KeyType = K;
			}
			else if (Type == TEXT("class"))
			{
				UBlackboardKeyType_Class* K = NewObject<UBlackboardKeyType_Class>(BB);
				K->BaseClass = BaseClass.IsEmpty() ? UObject::StaticClass() : ResolveClass(BaseClass);
				Entry.KeyType = K;
			}
			else if (Type == TEXT("bool")) Entry.KeyType = NewObject<UBlackboardKeyType_Bool>(BB);
			else if (Type == TEXT("float")) Entry.KeyType = NewObject<UBlackboardKeyType_Float>(BB);
			else if (Type == TEXT("int")) Entry.KeyType = NewObject<UBlackboardKeyType_Int>(BB);
			else if (Type == TEXT("vector")) Entry.KeyType = NewObject<UBlackboardKeyType_Vector>(BB);
			else if (Type == TEXT("rotator")) Entry.KeyType = NewObject<UBlackboardKeyType_Rotator>(BB);
			else if (Type == TEXT("name")) Entry.KeyType = NewObject<UBlackboardKeyType_Name>(BB);
			else if (Type == TEXT("string")) Entry.KeyType = NewObject<UBlackboardKeyType_String>(BB);
			else
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Unknown key type '%s'"), *Type)));
				continue;
			}
			bool bSynced = false;
			if (Def->TryGetBoolField(TEXT("instance_synced"), bSynced)) Entry.bInstanceSynced = bSynced;
			BB->Keys.Add(Entry);
		}
	}
	BB->PostEditChange();
	BB->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(BB, false);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Errors.Num() == 0);
	Result->SetArrayField(TEXT("errors"), Errors);
	Result->SetNumberField(TEXT("keys"), BB->Keys.Num());
	if (Errors.Num() > 0) Result->SetStringField(TEXT("error"), Errors[0]->AsString());
	return Result;
}

// Behavior Tree - {path, blackboard, root:{class, props:{}, decorators:[{class, props}], services:[...], children:[...]}}
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::BuildBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	UBehaviorTree* BT = LoadOrCreateAsset<UBehaviorTree>(Path);
	BT->Modify();

	FString BBPath;
	if (Params->TryGetStringField(TEXT("blackboard"), BBPath))
	{
		BT->BlackboardAsset = LoadObject<UBlackboardData>(nullptr, *(BBPath + TEXT(".") + FPackageName::GetShortName(BBPath)));
	}

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
	if (!Graph)
	{
		Graph = CastChecked<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(BT, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass()));
		BT->BTGraph = Graph;
		Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		Graph->OnCreated();
	}
	else
	{
		TArray<UEdGraphNode*> Existing = Graph->Nodes;
		for (UEdGraphNode* N : Existing)
		{
			if (!Cast<UBehaviorTreeGraphNode_Root>(N)) Graph->RemoveNode(N);
		}
	}

	UBehaviorTreeGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* N : Graph->Nodes) if (UBehaviorTreeGraphNode_Root* R = Cast<UBehaviorTreeGraphNode_Root>(N)) Root = R;
	if (!Root) return GraphError(TEXT("Behavior Tree graph has no root node"));
	Root->Pins[0]->BreakAllPinLinks();

	TArray<FString> Errors;
	auto ApplyProps = [&Errors](UObject* Instance, const TSharedPtr<FJsonObject>& Def)
	{
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Instance || !Def->TryGetObjectField(TEXT("props"), Props)) return;
		for (const auto& KV : (*Props)->Values)
		{
			FString PropErr;
			if (!SetPropertyByPath(Instance, KV.Key, KV.Value->AsString(), PropErr)) Errors.Add(PropErr);
		}
	};

	auto AddSubNodes = [&](UBehaviorTreeGraphNode* Parent, const TSharedPtr<FJsonObject>& Def, const FString& Field, bool bDecorator)
	{
		const TArray<TSharedPtr<FJsonValue>>* Subs = nullptr;
		if (!Def->TryGetArrayField(Field, Subs)) return;
		for (const TSharedPtr<FJsonValue>& V : *Subs)
		{
			const TSharedPtr<FJsonObject> SubDef = V->AsObject();
			UClass* Class = ResolveClass(SubDef->GetStringField(TEXT("class")));
			if (!Class) { Errors.Add(FString::Printf(TEXT("Unknown class '%s'"), *SubDef->GetStringField(TEXT("class")))); continue; }
			UBehaviorTreeGraphNode* Sub = bDecorator
				? (UBehaviorTreeGraphNode*)NewObject<UBehaviorTreeGraphNode_Decorator>(Graph)
				: (UBehaviorTreeGraphNode*)NewObject<UBehaviorTreeGraphNode_Service>(Graph);
			Sub->ClassData = FGraphNodeClassData(Class, FString());
			Parent->AddSubNode(Sub, Graph);
			ApplyProps(Sub->NodeInstance, SubDef);
		}
	};

	int32 Leaf = 0;
	TFunction<UBehaviorTreeGraphNode*(const TSharedPtr<FJsonObject>&, int32)> Build = [&](const TSharedPtr<FJsonObject>& Def, int32 Depth) -> UBehaviorTreeGraphNode*
	{
		UClass* Class = ResolveClass(Def->GetStringField(TEXT("class")));
		if (!Class) { Errors.Add(FString::Printf(TEXT("Unknown class '%s'"), *Def->GetStringField(TEXT("class")))); return nullptr; }
		UBehaviorTreeGraphNode* Node = nullptr;
		if (Class->IsChildOf(UBTCompositeNode::StaticClass()))
		{
			FGraphNodeCreator<UBehaviorTreeGraphNode_Composite> Creator(*Graph);
			Node = Creator.CreateNode(false);
			Node->ClassData = FGraphNodeClassData(Class, FString());
			Creator.Finalize();
		}
		else if (Class->IsChildOf(UBTTaskNode::StaticClass()))
		{
			FGraphNodeCreator<UBehaviorTreeGraphNode_Task> Creator(*Graph);
			Node = Creator.CreateNode(false);
			Node->ClassData = FGraphNodeClassData(Class, FString());
			Creator.Finalize();
		}
		else
		{
			Errors.Add(FString::Printf(TEXT("'%s' is not a composite or task"), *Class->GetName()));
			return nullptr;
		}
		ApplyProps(Node->NodeInstance, Def);
		AddSubNodes(Node, Def, TEXT("decorators"), true);
		AddSubNodes(Node, Def, TEXT("services"), false);

		// Children run left to right by X, so lay leaves out in order and centre parents over them.
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		TArray<UBehaviorTreeGraphNode*> Built;
		if (Def->TryGetArrayField(TEXT("children"), Children))
		{
			for (const TSharedPtr<FJsonValue>& C : *Children)
			{
				if (UBehaviorTreeGraphNode* Child = Build(C->AsObject(), Depth + 1))
				{
					Node->GetOutputPin()->MakeLinkTo(Child->GetInputPin());
					Built.Add(Child);
				}
			}
		}
		Node->NodePosY = 200 + Depth * 220;
		if (Built.Num() > 0)
		{
			Node->NodePosX = (Built[0]->NodePosX + Built.Last()->NodePosX) / 2;
		}
		else
		{
			Node->NodePosX = Leaf++ * 320;
		}
		return Node;
	};

	const TSharedPtr<FJsonObject>* RootDef = nullptr;
	if (Params->TryGetObjectField(TEXT("root"), RootDef))
	{
		if (UBehaviorTreeGraphNode* Top = Build(*RootDef, 0))
		{
			Root->Pins[0]->MakeLinkTo(Top->GetInputPin());
			Root->NodePosX = Top->NodePosX;
			Root->NodePosY = 0;
		}
	}

	Graph->UpdateAsset();
	Graph->NotifyGraphChanged();
	BT->MarkPackageDirty();
	UEditorAssetLibrary::SaveLoadedAsset(BT, false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& E : Errors) ErrorValues.Add(MakeShared<FJsonValueString>(E));
	Result->SetArrayField(TEXT("errors"), ErrorValues);
	Result->SetBoolField(TEXT("success"), Errors.Num() == 0);
	if (Errors.Num() > 0) Result->SetStringField(TEXT("error"), Errors[0]);
	Result->SetBoolField(TEXT("has_root"), BT->RootNode != nullptr);
	return Result;
}

// Describe - {blueprint, graph (optional)} returns a compact text listing of nodes, links and pin defaults.
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::DescribeGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UBlueprint* BP = LoadBlueprint(Params, Err);
	if (!BP) return GraphError(Err);
	FString Only;
	Params->TryGetStringField(TEXT("graph"), Only);

	TArray<UEdGraph*> Graphs;
	BP->GetAllGraphs(Graphs);
	FString Text;
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph || (!Only.IsEmpty() && Graph->GetName() != Only)) continue;
		Text += FString::Printf(TEXT("## %s (%s)\n"), *Graph->GetName(), *Graph->GetClass()->GetName());
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			Text += FString::Printf(TEXT("%s [%s] \"%s\" @%d,%d\n"), *Node->GetName(), *Node->GetClass()->GetName(),
				*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), Node->NodePosX, Node->NodePosY);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				const FString Default = Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : (!Pin->DefaultTextValue.IsEmpty() ? Pin->DefaultTextValue.ToString() : Pin->DefaultValue);
				if (Pin->LinkedTo.Num() == 0 && (Default.IsEmpty() || Pin->Direction == EGPD_Output)) continue;
				TArray<FString> Links;
				for (UEdGraphPin* L : Pin->LinkedTo)
				{
					if (L && L->GetOwningNode()) Links.Add(L->GetOwningNode()->GetName() + TEXT(".") + L->PinName.ToString());
				}
				Text += FString::Printf(TEXT("  %s %s%s%s\n"), Pin->Direction == EGPD_Input ? TEXT(">") : TEXT("<"), *Pin->PinName.ToString(),
					Default.IsEmpty() || Pin->LinkedTo.Num() > 0 ? TEXT("") : *(TEXT(" = ") + Default),
					Links.Num() ? *(TEXT(" -> ") + FString::Join(Links, TEXT(", "))) : TEXT(""));
			}
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("text"), Text);
	return Result;
}

// Properties - {object: path, values: {"Prop.Sub": "text"}} writes reflected values from text, including edit-defaults-only ones, then saves.
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::SetProperties(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("object"));
	UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	if (!Object)
	{
		FString AssetPath = Path + TEXT(".") + FPackageName::GetShortName(Path);
		Object = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	}
	if (!Object) return GraphError(FString::Printf(TEXT("Object not found: %s"), *Path));
	Object->Modify();
	TArray<TSharedPtr<FJsonValue>> Errors;
	const TSharedPtr<FJsonObject>* Values = nullptr;
	if (Params->TryGetObjectField(TEXT("values"), Values))
	{
		for (const auto& KV : (*Values)->Values)
		{
			FString Err;
			if (!SetPropertyByPath(Object, KV.Key, KV.Value->AsString(), Err)) Errors.Add(MakeShared<FJsonValueString>(Err));
		}
	}
	Object->PostEditChange();
	Object->MarkPackageDirty();
	if (UObject* Asset = Object->GetOutermost()->FindAssetInPackage())
	{
		UEditorAssetLibrary::SaveLoadedAsset(Asset, false);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Errors.Num() == 0);
	Result->SetArrayField(TEXT("errors"), Errors);
	if (Errors.Num() > 0) Result->SetStringField(TEXT("error"), Errors[0]->AsString());
	return Result;
}

// Widget tree - {blueprint, root:{type, name, variable, props:{}, slot:{}, children:[...]}}. Replaces the whole tree.
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::BuildWidget(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("blueprint"));
	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	if (!WBP || !WBP->WidgetTree) return GraphError(FString::Printf(TEXT("Widget Blueprint not found: %s"), *Path));

	WBP->Modify();
	WBP->WidgetTree->Modify();
	TArray<UWidget*> Old;
	WBP->WidgetTree->GetAllWidgets(Old);
	for (UWidget* Widget : Old)
	{
		WBP->WidgetTree->RemoveWidget(Widget);
		Widget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
	}
	WBP->WidgetTree->RootWidget = nullptr;
	WBP->WidgetVariableNameToGuidMap.Empty();

	TArray<TSharedPtr<FJsonValue>> Errors;
	TFunction<UWidget*(const TSharedPtr<FJsonObject>&, UPanelWidget*)> Build;
	Build = [&](const TSharedPtr<FJsonObject>& Spec, UPanelWidget* Parent) -> UWidget*
	{
		const FString TypeName = Spec->GetStringField(TEXT("type"));
		UClass* Class = ResolveClass(TypeName);
		if (!Class || !Class->IsChildOf(UWidget::StaticClass()))
		{
			Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Unknown widget type: %s"), *TypeName)));
			return nullptr;
		}
		FString Name;
		Spec->TryGetStringField(TEXT("name"), Name);
		UWidget* Widget = WBP->WidgetTree->ConstructWidget<UWidget>(Class, Name.IsEmpty() ? NAME_None : FName(*Name));
		bool bVariable = false;
		Spec->TryGetBoolField(TEXT("variable"), bVariable);
		Widget->bIsVariable = bVariable;

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (Spec->TryGetObjectField(TEXT("props"), Props))
		{
			for (const auto& KV : (*Props)->Values)
			{
				FString Err;
				if (!SetPropertyByPath(Widget, KV.Key, KV.Value->AsString(), Err)) Errors.Add(MakeShared<FJsonValueString>(Name + TEXT(": ") + Err));
			}
		}

		if (Parent)
		{
			UPanelSlot* Slot = Parent->AddChild(Widget);
			const TSharedPtr<FJsonObject>* SlotProps = nullptr;
			if (Slot && Spec->TryGetObjectField(TEXT("slot"), SlotProps))
			{
				for (const auto& KV : (*SlotProps)->Values)
				{
					FString Err;
					if (!SetPropertyByPath(Slot, KV.Key, KV.Value->AsString(), Err)) Errors.Add(MakeShared<FJsonValueString>(Name + TEXT(" slot: ") + Err));
				}
			}
		}
		WBP->OnVariableAdded(Widget->GetFName());

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Spec->TryGetArrayField(TEXT("children"), Children))
		{
			UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
			if (!Panel)
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s is not a panel and cannot hold children"), *Name)));
			}
			else
			{
				for (const TSharedPtr<FJsonValue>& Child : *Children)
				{
					Build(Child->AsObject(), Panel);
				}
			}
		}
		return Widget;
	};

	const TSharedPtr<FJsonObject>* Root = nullptr;
	if (Params->TryGetObjectField(TEXT("root"), Root))
	{
		WBP->WidgetTree->RootWidget = Build(*Root, nullptr);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	FKismetEditorUtilities::CompileBlueprint(WBP);
	UEditorAssetLibrary::SaveLoadedAsset(WBP, false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Errors.Num() == 0);
	Result->SetArrayField(TEXT("errors"), Errors);
	if (Errors.Num() > 0) Result->SetStringField(TEXT("error"), Errors[0]->AsString());
	return Result;
}

// Comment colours - {folder, color:"(R=0,G=0,B=0,A=1)"}: every comment box in every Blueprint under the folder.
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::RecolorComments(const TSharedPtr<FJsonObject>& Params)
{
	const FString Folder = Params->GetStringField(TEXT("folder"));
	FLinearColor Color = FLinearColor::Black;
	FString ColorText;
	if (Params->TryGetStringField(TEXT("color"), ColorText)) Color.InitFromString(ColorText);

	int32 Changed = 0;
	TArray<TSharedPtr<FJsonValue>> Touched;
	for (const FString& AssetPath : UEditorAssetLibrary::ListAssets(Folder, true, false))
	{
		UBlueprint* BP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
		if (!BP) continue;
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		int32 Here = 0;
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
				{
					if (!Comment->CommentColor.Equals(Color))
					{
						Comment->Modify();
						Comment->CommentColor = Color;
						++Here;
					}
				}
			}
		}
		if (Here > 0)
		{
			Changed += Here;
			Touched.Add(MakeShared<FJsonValueString>(BP->GetName()));
			BP->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(BP, false);
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("changed"), Changed);
	Result->SetArrayField(TEXT("blueprints"), Touched);
	return Result;
}

// Comment texts - {folder}: every comment box in every Blueprint under the folder, with the node name set_comment takes.
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::ListComments(const TSharedPtr<FJsonObject>& Params)
{
	const FString Folder = Params->GetStringField(TEXT("folder"));
	TArray<TSharedPtr<FJsonValue>> Found;
	for (const FString& AssetPath : UEditorAssetLibrary::ListAssets(Folder, true, false))
	{
		UBlueprint* BP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
		if (!BP) continue;
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("blueprint"), BP->GetPathName());
					Entry->SetStringField(TEXT("graph"), Graph->GetName());
					Entry->SetStringField(TEXT("node"), Comment->GetName());
					Entry->SetStringField(TEXT("text"), Comment->NodeComment);
					Found.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("comments"), Found);
	return Result;
}

// Comment text - {blueprint, graph, node, text}: rewrite one comment box and save.
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::SetComment(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Params->GetStringField(TEXT("blueprint")));
	if (!BP) return GraphError(TEXT("Blueprint not found"));
	const FString GraphName = Params->GetStringField(TEXT("graph"));
	const FString NodeName = Params->GetStringField(TEXT("node"));
	TArray<UEdGraph*> Graphs;
	BP->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph->GetName() != GraphName) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
			if (Comment && Comment->GetName() == NodeName)
			{
				Comment->Modify();
				Comment->NodeComment = Params->GetStringField(TEXT("text"));
				BP->MarkPackageDirty();
				UEditorAssetLibrary::SaveLoadedAsset(BP, false);
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("success"), true);
				return Result;
			}
		}
	}
	return GraphError(FString::Printf(TEXT("Comment %s not found in %s"), *NodeName, *GraphName));
}

// Widget tree - {blueprint}: every widget with its class, slot, and the non-default properties that shape its size, as text.
TSharedPtr<FJsonObject> FUnrealMCPGraphBuilder::DescribeWidget(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("blueprint"));
	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	if (!WBP || !WBP->WidgetTree) return GraphError(TEXT("Widget Blueprint not found"));

	FString Text;
	TFunction<void(UWidget*, int32)> Walk = [&](UWidget* Widget, int32 Depth)
	{
		if (!Widget) return;
		Text += FString::ChrN(Depth * 2, TEXT(' ')) + Widget->GetClass()->GetName() + TEXT(" ") + Widget->GetName();
		auto Dump = [&Text](UObject* Object)
		{
			const UObject* Defaults = Object->GetClass()->GetDefaultObject();
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Edit) || It->Identical_InContainer(Object, Defaults)) continue;
				FString Value;
				It->ExportTextItem_InContainer(Value, Object, nullptr, nullptr, PPF_None);
				Text += TEXT(" ") + It->GetName() + TEXT("=") + Value.Left(160);
			}
		};
		Dump(Widget);
		if (Widget->Slot)
		{
			Text += TEXT(" | slot:");
			Dump(Widget->Slot);
		}
		Text += TEXT("\n");
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index) Walk(Panel->GetChildAt(Index), Depth + 1);
		}
	};
	Walk(WBP->WidgetTree->RootWidget, 0);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("text"), Text);
	return Result;
}
