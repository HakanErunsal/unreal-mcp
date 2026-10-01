#include "Commands/UnrealMCPNiagaraCommands.h"

#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace UnrealMCPNiagaraPrivate
{
	TSharedPtr<FJsonObject> McpNiagaraError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Message);
		return Result;
	}

	template <typename T>
	T* McpLoad(const FString& Path)
	{
		if (T* Found = LoadObject<T>(nullptr, *Path))
		{
			return Found;
		}
		if (!Path.Contains(TEXT(".")))
		{
			return LoadObject<T>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
		}
		return nullptr;
	}

	bool McpNameMatches(const FText& Display, const FString& Wanted)
	{
		return Display.ToString().Equals(Wanted, ESearchCase::IgnoreCase);
	}

	TSharedRef<FNiagaraSystemViewModel> McpMakeViewModel(UNiagaraSystem& System)
	{
		// Matches the engine's headless view models (CreateSystemViewModelForUpgrade); the stack's message subscriptions assert without an asset key.
		System.WaitForCompilationComplete();
		FNiagaraSystemViewModelOptions Options;
		Options.bCanModifyEmittersFromTimeline = false;
		Options.bCanAutoCompile = false;
		Options.bCanSimulate = false;
		Options.bIsForDataProcessingOnly = true;
		Options.MessageLogGuid = System.GetAssetGuid();
		Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
		TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
		ViewModel->Initialize(System, Options);
		return ViewModel;
	}

	TSharedPtr<FNiagaraEmitterHandleViewModel> McpFindEmitter(const TSharedRef<FNiagaraSystemViewModel>& ViewModel, const FString& Name)
	{
		for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel->GetEmitterHandleViewModels())
		{
			if (Handle->GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Handle;
			}
		}
		return nullptr;
	}

	UNiagaraStackViewModel* McpFindStack(const TSharedRef<FNiagaraSystemViewModel>& ViewModel, const FString& EmitterName, FString& OutError)
	{
		if (EmitterName.IsEmpty() || EmitterName.Equals(TEXT("System"), ESearchCase::IgnoreCase))
		{
			return ViewModel->GetSystemStackViewModel();
		}
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Handle = McpFindEmitter(ViewModel, EmitterName);
		if (!Handle.IsValid())
		{
			OutError = FString::Printf(TEXT("emitter not found: %s"), *EmitterName);
			return nullptr;
		}
		return Handle->GetEmitterStackViewModel();
	}

	void McpRefresh(UNiagaraStackViewModel* Stack)
	{
		if (UNiagaraStackEntry* Root = Stack ? Stack->GetRootEntry() : nullptr)
		{
			Root->RefreshChildren();
		}
	}

	template <typename T>
	TArray<T*> McpCollect(UNiagaraStackViewModel* Stack)
	{
		TArray<T*> Out;
		if (UNiagaraStackEntry* Root = Stack ? Stack->GetRootEntry() : nullptr)
		{
			Root->GetUnfilteredChildrenOfType(Out, true);
		}
		return Out;
	}

	FString McpGroupName(const UNiagaraStackEntry* Entry)
	{
		for (const UObject* Outer = Entry ? Entry->GetOuter() : nullptr; Outer; Outer = Outer->GetOuter())
		{
			if (const UNiagaraStackScriptItemGroup* Group = Cast<UNiagaraStackScriptItemGroup>(Outer))
			{
				return Group->GetDisplayName().ToString();
			}
		}
		return FString();
	}

	UNiagaraStackModuleItem* McpFindModule(UNiagaraStackViewModel* Stack, const FString& Wanted, FString& OutError)
	{
		FString ModuleName = Wanted, GroupName;
		Wanted.Split(TEXT("@"), &ModuleName, &GroupName);
		TArray<UNiagaraStackModuleItem*> Matches;
		for (UNiagaraStackModuleItem* Module : McpCollect<UNiagaraStackModuleItem>(Stack))
		{
			if (McpNameMatches(Module->GetDisplayName(), ModuleName) && (GroupName.IsEmpty() || McpGroupName(Module).Equals(GroupName, ESearchCase::IgnoreCase)))
			{
				Matches.Add(Module);
			}
		}
		if (Matches.Num() != 1)
		{
			OutError = FString::Printf(TEXT("module '%s' matched %d entries"), *Wanted, Matches.Num());
			return nullptr;
		}
		return Matches[0];
	}

	UNiagaraStackFunctionInput* McpFindInput(UNiagaraStackModuleItem* Module, const FString& Path, FString& OutError)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"));
		TArray<UNiagaraStackFunctionInput*> Candidates;
		Module->GetParameterInputs(Candidates);
		UNiagaraStackFunctionInput* Current = nullptr;
		for (const FString& Part : Parts)
		{
			Current = nullptr;
			for (UNiagaraStackFunctionInput* Candidate : Candidates)
			{
				if (McpNameMatches(Candidate->GetDisplayName(), Part))
				{
					Current = Candidate;
					break;
				}
			}
			if (!Current)
			{
				OutError = FString::Printf(TEXT("input '%s' not found on '%s'"), *Path, *Module->GetDisplayName().ToString());
				return nullptr;
			}
			Candidates = Current->GetChildInputs();
		}
		return Current;
	}

	const TCHAR* McpModeName(UNiagaraStackFunctionInput::EValueMode Mode)
	{
		switch (Mode)
		{
		case UNiagaraStackFunctionInput::EValueMode::Local: return TEXT("Local");
		case UNiagaraStackFunctionInput::EValueMode::Linked: return TEXT("Linked");
		case UNiagaraStackFunctionInput::EValueMode::Dynamic: return TEXT("Dynamic");
		case UNiagaraStackFunctionInput::EValueMode::Data: return TEXT("Data");
		case UNiagaraStackFunctionInput::EValueMode::ObjectAsset: return TEXT("ObjectAsset");
		case UNiagaraStackFunctionInput::EValueMode::Expression: return TEXT("Expression");
		case UNiagaraStackFunctionInput::EValueMode::DefaultFunction: return TEXT("DefaultFunction");
		case UNiagaraStackFunctionInput::EValueMode::InvalidOverride: return TEXT("InvalidOverride");
		case UNiagaraStackFunctionInput::EValueMode::UnsupportedDefault: return TEXT("UnsupportedDefault");
		default: return TEXT("None");
		}
	}

	FString McpValueText(UNiagaraStackFunctionInput* Input)
	{
		switch (Input->GetValueMode())
		{
		case UNiagaraStackFunctionInput::EValueMode::Local:
		{
			const TSharedPtr<const FStructOnScope> Local = Input->GetLocalValueStruct();
			if (!Local.IsValid())
			{
				return FString();
			}
			const FNiagaraTypeDefinition& Type = Input->GetInputType();
			if (Type.IsEnum())
			{
				return Type.GetEnum()->GetDisplayNameTextByValue(*reinterpret_cast<const int32*>(Local->GetStructMemory())).ToString();
			}
			return Type.ToString(Local->GetStructMemory()).TrimStartAndEnd();
		}
		case UNiagaraStackFunctionInput::EValueMode::Linked:
			return Input->GetLinkedParameterValue().GetName().ToString();
		case UNiagaraStackFunctionInput::EValueMode::Dynamic:
			return Input->GetDynamicInputNode() ? Input->GetDynamicInputNode()->GetFunctionName() : FString();
		case UNiagaraStackFunctionInput::EValueMode::Data:
			return GetNameSafe(Input->GetDataValueObject());
		case UNiagaraStackFunctionInput::EValueMode::ObjectAsset:
			return GetPathNameSafe(Input->GetObjectAssetValue());
		default:
			return FString();
		}
	}

	TSharedPtr<FJsonObject> McpDescribeInput(UNiagaraStackFunctionInput* Input)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Input->GetDisplayName().ToString());
		Out->SetStringField(TEXT("type"), Input->GetInputType().GetName());
		Out->SetStringField(TEXT("mode"), McpModeName(Input->GetValueMode()));
		Out->SetStringField(TEXT("value"), McpValueText(Input));
		if (Input->GetHasEditCondition())
		{
			Out->SetBoolField(TEXT("edit_condition"), Input->GetEditConditionEnabled());
		}
		TArray<TSharedPtr<FJsonValue>> Children;
		for (UNiagaraStackFunctionInput* Child : Input->GetChildInputs())
		{
			Children.Add(MakeShared<FJsonValueObject>(McpDescribeInput(Child)));
		}
		if (Children.Num() > 0)
		{
			Out->SetArrayField(TEXT("inputs"), Children);
		}
		return Out;
	}

	TArray<double> McpNumbers(const TSharedPtr<FJsonValue>& Value)
	{
		TArray<double> Numbers;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array))
		{
			for (const TSharedPtr<FJsonValue>& Element : *Array)
			{
				Numbers.Add(Element->AsNumber());
			}
		}
		else
		{
			double Number = 0.0;
			if (Value->TryGetNumber(Number))
			{
				Numbers.Add(Number);
			}
		}
		return Numbers;
	}

	bool McpSetLocal(UNiagaraStackFunctionInput* Input, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		const FNiagaraTypeDefinition& Type = Input->GetInputType();
		if (Type.IsUObject() || Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::ObjectAsset)
		{
			UObject* Asset = Value->Type == EJson::Null ? nullptr : McpLoad<UObject>(Value->AsString());
			if (!Asset && Value->Type != EJson::Null)
			{
				OutError = FString::Printf(TEXT("asset not found: %s"), *Value->AsString());
				return false;
			}
			Input->SetObjectAssetValue(Asset);
			return true;
		}

		const UScriptStruct* Struct = Type.GetScriptStruct();
		if (!Struct)
		{
			OutError = FString::Printf(TEXT("input type %s has no value struct"), *Type.GetName());
			return false;
		}
		TSharedRef<FStructOnScope> Local = MakeShared<FStructOnScope>(Struct);
		uint8* Memory = Local->GetStructMemory();
		const TArray<double> N = McpNumbers(Value);

		if (Type.IsEnum())
		{
			int64 EnumValue = INDEX_NONE;
			FString EnumName;
			if (Value->TryGetString(EnumName))
			{
				const UEnum* Enum = Type.GetEnum();
				EnumValue = Enum->GetValueByNameString(EnumName);
				if (EnumValue == INDEX_NONE)
				{
					for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
					{
						if (Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(EnumName, ESearchCase::IgnoreCase))
						{
							EnumValue = Enum->GetValueByIndex(Index);
							break;
						}
					}
				}
			}
			else if (N.Num() > 0)
			{
				EnumValue = static_cast<int64>(N[0]);
			}
			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("enum value not found for %s"), *Type.GetName());
				return false;
			}
			*reinterpret_cast<int32*>(Memory) = static_cast<int32>(EnumValue);
		}
		else if (Struct == FNiagaraTypeDefinition::GetBoolStruct())
		{
			bool bValue = false;
			if (!Value->TryGetBool(bValue))
			{
				bValue = N.Num() > 0 && N[0] != 0.0;
			}
			reinterpret_cast<FNiagaraBool*>(Memory)->SetValue(bValue);
		}
		else if (Struct == FNiagaraTypeDefinition::GetIntStruct())
		{
			*reinterpret_cast<int32*>(Memory) = N.Num() > 0 ? static_cast<int32>(N[0]) : 0;
		}
		else
		{
			const int32 Floats = Struct->GetStructureSize() / sizeof(float);
			if (Floats < 1 || N.Num() < Floats || Struct->GetStructureSize() % sizeof(float) != 0)
			{
				OutError = FString::Printf(TEXT("input type %s needs %d numbers"), *Type.GetName(), Floats);
				return false;
			}
			float* Components = reinterpret_cast<float*>(Memory);
			for (int32 Index = 0; Index < Floats; ++Index)
			{
				Components[Index] = static_cast<float>(N[Index]);
			}
		}
		Input->SetLocalValue(Local);
		return true;
	}

	FString McpOpString(const TSharedPtr<FJsonObject>& Op, const TCHAR* Field)
	{
		FString Value;
		Op->TryGetStringField(Field, Value);
		return Value;
	}

	void McpCompileAndSave(UNiagaraSystem& System)
	{
		System.RequestCompile(false);
		System.WaitForCompilationComplete();
		System.MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(&System, false);
	}
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("niagara_add_emitter")) return AddEmitter(Params);
	if (CommandType == TEXT("niagara_describe")) return Describe(Params);
	if (CommandType == TEXT("niagara_edit")) return Edit(Params);
	return UnrealMCPNiagaraPrivate::McpNiagaraError(FString::Printf(TEXT("Unknown niagara command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::AddEmitter(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UnrealMCPNiagaraPrivate;

	UNiagaraSystem* System = McpLoad<UNiagaraSystem>(Params->GetStringField(TEXT("system")));
	UNiagaraEmitter* Template = McpLoad<UNiagaraEmitter>(Params->GetStringField(TEXT("template")));
	if (!System || !Template)
	{
		return McpNiagaraError(TEXT("system and template must both load"));
	}

	TSharedRef<FNiagaraSystemViewModel> ViewModel = McpMakeViewModel(*System);
	const TSharedPtr<FNiagaraEmitterHandleViewModel> Added = ViewModel->AddEmitter(*Template, Template->GetExposedVersion().VersionGuid);
	if (!Added.IsValid())
	{
		return McpNiagaraError(TEXT("AddEmitter failed"));
	}
	FString Name;
	if (Params->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
	{
		Added->SetName(FName(*Name));
	}
	McpCompileAndSave(*System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("emitter"), Added->GetName().ToString());
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::Describe(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UnrealMCPNiagaraPrivate;

	UNiagaraSystem* System = McpLoad<UNiagaraSystem>(Params->GetStringField(TEXT("system")));
	if (!System)
	{
		return McpNiagaraError(TEXT("system not found"));
	}
	FString OnlyEmitter;
	Params->TryGetStringField(TEXT("emitter"), OnlyEmitter);

	TSharedRef<FNiagaraSystemViewModel> ViewModel = McpMakeViewModel(*System);
	TArray<FString> StackNames;
	if (OnlyEmitter.IsEmpty())
	{
		StackNames.Add(TEXT("System"));
		for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel->GetEmitterHandleViewModels())
		{
			StackNames.Add(Handle->GetName().ToString());
		}
	}
	else
	{
		StackNames.Add(OnlyEmitter);
	}

	TArray<TSharedPtr<FJsonValue>> Stacks;
	for (const FString& StackName : StackNames)
	{
		FString Error;
		UNiagaraStackViewModel* Stack = McpFindStack(ViewModel, StackName, Error);
		if (!Stack)
		{
			return McpNiagaraError(Error);
		}
		McpRefresh(Stack);

		TSharedPtr<FJsonObject> StackJson = MakeShared<FJsonObject>();
		StackJson->SetStringField(TEXT("emitter"), StackName);
		TArray<TSharedPtr<FJsonValue>> Modules;
		for (UNiagaraStackModuleItem* Module : McpCollect<UNiagaraStackModuleItem>(Stack))
		{
			TSharedPtr<FJsonObject> ModuleJson = MakeShared<FJsonObject>();
			ModuleJson->SetStringField(TEXT("name"), Module->GetDisplayName().ToString());
			ModuleJson->SetStringField(TEXT("group"), McpGroupName(Module));
			ModuleJson->SetBoolField(TEXT("enabled"), Module->GetIsEnabled());
			ModuleJson->SetStringField(TEXT("script"), GetPathNameSafe(Module->GetModuleNode().FunctionScript));
			TArray<UNiagaraStackFunctionInput*> Inputs;
			Module->GetParameterInputs(Inputs);
			TArray<TSharedPtr<FJsonValue>> InputJson;
			for (UNiagaraStackFunctionInput* Input : Inputs)
			{
				InputJson.Add(MakeShared<FJsonValueObject>(McpDescribeInput(Input)));
			}
			ModuleJson->SetArrayField(TEXT("inputs"), InputJson);
			Modules.Add(MakeShared<FJsonValueObject>(ModuleJson));
		}
		StackJson->SetArrayField(TEXT("modules"), Modules);

		if (const TSharedPtr<FNiagaraEmitterHandleViewModel> Handle = McpFindEmitter(ViewModel, StackName))
		{
			TArray<TSharedPtr<FJsonValue>> Renderers;
			const FVersionedNiagaraEmitter Instance = Handle->GetEmitterHandle()->GetInstance();
			if (const FVersionedNiagaraEmitterData* Data = Instance.GetEmitterData())
			{
				for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
				{
					Renderers.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Renderer)));
				}
			}
			StackJson->SetArrayField(TEXT("renderers"), Renderers);
		}
		Stacks.Add(MakeShared<FJsonValueObject>(StackJson));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("stacks"), Stacks);
	return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::Edit(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UnrealMCPNiagaraPrivate;

	UNiagaraSystem* System = McpLoad<UNiagaraSystem>(Params->GetStringField(TEXT("system")));
	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (!System || !Params->TryGetArrayField(TEXT("ops"), Ops))
	{
		return McpNiagaraError(TEXT("system and ops are required"));
	}

	TSharedRef<FNiagaraSystemViewModel> ViewModel = McpMakeViewModel(*System);
	TArray<TSharedPtr<FJsonValue>> Report;
	for (int32 OpIndex = 0; OpIndex < Ops->Num(); ++OpIndex)
	{
		const TSharedPtr<FJsonObject> Op = (*Ops)[OpIndex]->AsObject();
		if (!Op.IsValid())
		{
			return McpNiagaraError(FString::Printf(TEXT("op %d is not an object"), OpIndex));
		}
		const FString Kind = McpOpString(Op, TEXT("op"));
		const FString EmitterName = McpOpString(Op, TEXT("emitter"));
		FString Error, Note;
		auto Fail = [&](const FString& Message)
		{
			return McpNiagaraError(FString::Printf(TEXT("op %d (%s): %s"), OpIndex, *Kind, *Message));
		};

		if (Kind == TEXT("rename_emitter") || Kind == TEXT("enable_emitter") || Kind == TEXT("remove_emitter") || Kind == TEXT("add_renderer") || Kind == TEXT("remove_renderer"))
		{
			const TSharedPtr<FNiagaraEmitterHandleViewModel> Handle = McpFindEmitter(ViewModel, EmitterName);
			if (!Handle.IsValid())
			{
				return Fail(FString::Printf(TEXT("emitter not found: %s"), *EmitterName));
			}
			if (Kind == TEXT("rename_emitter"))
			{
				Handle->SetName(FName(*McpOpString(Op, TEXT("name"))));
			}
			else if (Kind == TEXT("enable_emitter"))
			{
				Handle->SetIsEnabled(Op->GetBoolField(TEXT("enabled")), false);
			}
			else if (Kind == TEXT("remove_emitter"))
			{
				ViewModel->DeleteEmitters({ Handle->GetId() });
			}
			else
			{
				FVersionedNiagaraEmitter Instance = Handle->GetEmitterHandle()->GetInstance();
				UNiagaraEmitter* Emitter = Instance.Emitter;
				if (Kind == TEXT("add_renderer"))
				{
					UClass* RendererClass = McpLoad<UClass>(McpOpString(Op, TEXT("class")));
					if (!RendererClass || !RendererClass->IsChildOf(UNiagaraRendererProperties::StaticClass()))
					{
						return Fail(TEXT("class must be a Niagara renderer properties class"));
					}
					UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, NAME_None, RF_Transactional);
					Emitter->AddRenderer(Renderer, Instance.Version);
					Note = Renderer->GetPathName();
				}
				else
				{
					const FVersionedNiagaraEmitterData* Data = Instance.GetEmitterData();
					const int32 RendererIndex = static_cast<int32>(Op->GetNumberField(TEXT("index")));
					if (!Data || !Data->GetRenderers().IsValidIndex(RendererIndex))
					{
						return Fail(TEXT("renderer index out of range"));
					}
					Emitter->RemoveRenderer(Data->GetRenderers()[RendererIndex], Instance.Version);
				}
			}
		}
		else
		{
			UNiagaraStackViewModel* Stack = McpFindStack(ViewModel, EmitterName, Error);
			if (!Stack)
			{
				return Fail(Error);
			}
			McpRefresh(Stack);

			if (Kind == TEXT("add_module"))
			{
				const FString GroupName = McpOpString(Op, TEXT("group"));
				UNiagaraStackScriptItemGroup* Group = nullptr;
				for (UNiagaraStackScriptItemGroup* Candidate : McpCollect<UNiagaraStackScriptItemGroup>(Stack))
				{
					if (McpNameMatches(Candidate->GetDisplayName(), GroupName))
					{
						Group = Candidate;
						break;
					}
				}
				UNiagaraScript* Script = McpLoad<UNiagaraScript>(McpOpString(Op, TEXT("script")));
				UNiagaraNodeOutput* Output = Group ? Group->GetScriptOutputNode() : nullptr;
				if (!Output || !Script)
				{
					return Fail(FString::Printf(TEXT("group '%s' or script not found"), *GroupName));
				}
				const int32 Index = Op->HasField(TEXT("index")) ? static_cast<int32>(Op->GetNumberField(TEXT("index"))) : INDEX_NONE;
				UNiagaraNodeFunctionCall* Added = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *Output, Index, McpOpString(Op, TEXT("name")));
				if (!Added)
				{
					return Fail(TEXT("AddScriptModuleToStack failed"));
				}
				Note = Added->GetFunctionName();
			}
			else
			{
				UNiagaraStackModuleItem* Module = McpFindModule(Stack, McpOpString(Op, TEXT("module")), Error);
				if (!Module)
				{
					return Fail(Error);
				}
				if (Kind == TEXT("enable_module"))
				{
					Module->SetIsEnabled(Op->GetBoolField(TEXT("enabled")));
				}
				else if (Kind == TEXT("remove_module"))
				{
					Module->Delete();
				}
				else
				{
					UNiagaraStackFunctionInput* Input = McpFindInput(Module, McpOpString(Op, TEXT("input")), Error);
					if (!Input)
					{
						return Fail(Error);
					}
					if (Kind == TEXT("set"))
					{
						if (!McpSetLocal(Input, Op->TryGetField(TEXT("value")), Error))
						{
							return Fail(Error);
						}
					}
					else if (Kind == TEXT("dynamic"))
					{
						UNiagaraScript* Script = McpLoad<UNiagaraScript>(McpOpString(Op, TEXT("script")));
						if (!Script)
						{
							return Fail(TEXT("dynamic input script not found"));
						}
						Input->SetDynamicInput(Script);
					}
					else if (Kind == TEXT("link"))
					{
						Input->SetLinkedParameterValue(FNiagaraVariableBase(Input->GetInputType(), FName(*McpOpString(Op, TEXT("parameter")))));
					}
					else if (Kind == TEXT("reset"))
					{
						Input->Reset();
					}
					else if (Kind == TEXT("edit_condition"))
					{
						if (!Input->GetHasEditCondition())
						{
							return Fail(TEXT("input has no edit condition"));
						}
						Input->SetEditConditionEnabled(Op->GetBoolField(TEXT("enabled")));
					}
					else
					{
						return Fail(TEXT("unknown op"));
					}
					Note = McpValueText(Input);
				}
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("op"), Kind);
		Entry->SetStringField(TEXT("result"), Note);
		Report.Add(MakeShared<FJsonValueObject>(Entry));
	}

	McpCompileAndSave(*System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("ops"), Report);
	return Result;
}
