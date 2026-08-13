#include "BlueprintHandlers.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintEditorSettings.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSignature.h"
#include "BlueprintNodeSpawner.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "HandlerUtils.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace BlueprintActionDiscovery
{
	static constexpr int32 DefaultLimit = 2000;
	static constexpr int32 MaximumLimit = 5000;
	static constexpr int32 MaximumContexts = 8;

	struct FContextProfile
	{
		FString Id;
		FString AssetPath;
		FString GraphName;
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		bool bPackageDirtyBefore = false;
	};

	struct FActionRecord
	{
		FString SortKey;
		TSharedPtr<FJsonObject> Json;
	};

	static FString ObjectPath(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	static FString FieldPath(const FFieldVariant& Field)
	{
		if (Field.IsUObject()) return ObjectPath(Field.ToUObject());
		if (const FField* NativeField = Field.ToField()) return NativeField->GetPathName();
		return FString();
	}

	static FString ContainerName(const EPinContainerType Container)
	{
		switch (Container)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set: return TEXT("set");
		case EPinContainerType::Map: return TEXT("map");
		default: return TEXT("scalar");
		}
	}

	static TSharedPtr<FJsonObject> SerializeTerminalType(const FEdGraphTerminalType& Type)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("category"), Type.TerminalCategory.ToString());
		Result->SetStringField(TEXT("subcategory"), Type.TerminalSubCategory.ToString());
		Result->SetStringField(TEXT("subcategory_object"), ObjectPath(Type.TerminalSubCategoryObject.Get()));
		Result->SetBoolField(TEXT("const"), Type.bTerminalIsConst);
		Result->SetBoolField(TEXT("weak_reference"), Type.bTerminalIsWeakPointer);
		Result->SetBoolField(TEXT("uobject_wrapper"), Type.bTerminalIsUObjectWrapper);
		return Result;
	}

	static TSharedPtr<FJsonObject> SerializePinType(const FEdGraphPinType& Type)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("category"), Type.PinCategory.ToString());
		Result->SetStringField(TEXT("subcategory"), Type.PinSubCategory.ToString());
		Result->SetStringField(TEXT("subcategory_object"), ObjectPath(Type.PinSubCategoryObject.Get()));
		Result->SetStringField(TEXT("container"), ContainerName(Type.ContainerType));
		Result->SetBoolField(TEXT("reference"), Type.bIsReference);
		Result->SetBoolField(TEXT("const"), Type.bIsConst);
		Result->SetBoolField(TEXT("weak_reference"), Type.bIsWeakPointer);
		Result->SetBoolField(TEXT("uobject_wrapper"), Type.bIsUObjectWrapper);
		Result->SetBoolField(TEXT("single_precision_legacy"), Type.bSerializeAsSinglePrecisionFloat);
		TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
		Member->SetStringField(TEXT("name"), Type.PinSubCategoryMemberReference.MemberName.ToString());
		Member->SetStringField(TEXT("owner"), ObjectPath(Type.PinSubCategoryMemberReference.MemberParent));
		Member->SetStringField(TEXT("guid"), Type.PinSubCategoryMemberReference.MemberGuid.IsValid()
			? Type.PinSubCategoryMemberReference.MemberGuid.ToString(EGuidFormats::Digits) : FString());
		Result->SetObjectField(TEXT("subcategory_member_reference"), Member);
		Result->SetObjectField(TEXT("map_value_type"), SerializeTerminalType(Type.PinValueType));
		return Result;
	}

	static FString Direction(const FProperty* Property)
	{
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm)) return TEXT("return");
		if (Property->HasAnyPropertyFlags(CPF_OutParm))
			return Property->HasAnyPropertyFlags(CPF_ReferenceParm | CPF_ConstParm) ? TEXT("inout") : TEXT("output");
		return TEXT("input");
	}

	static TSharedPtr<FJsonObject> SerializeProperty(const FProperty* Property, const UFunction* Function, int32 Index)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("index"), Index);
		Result->SetStringField(TEXT("name"), Property->GetName());
		Result->SetStringField(TEXT("direction"), Direction(Property));
		Result->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
		Result->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Result->SetStringField(TEXT("property_flags"), FString::Printf(TEXT("0x%016llx"),
			static_cast<unsigned long long>(Property->GetPropertyFlags())));
		Result->SetBoolField(TEXT("const"), Property->HasAnyPropertyFlags(CPF_ConstParm));
		Result->SetBoolField(TEXT("reference"), Property->HasAnyPropertyFlags(CPF_ReferenceParm));
		Result->SetBoolField(TEXT("out"), Property->HasAnyPropertyFlags(CPF_OutParm));
		Result->SetBoolField(TEXT("return"), Property->HasAnyPropertyFlags(CPF_ReturnParm));
		FEdGraphPinType PinType;
		const bool bConverted = GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType);
		Result->SetBoolField(TEXT("k2_type_resolved"), bConverted);
		Result->SetObjectField(TEXT("pin_type"), SerializePinType(PinType));
		const FString DefaultKey = FString::Printf(TEXT("CPP_Default_%s"), *Property->GetName());
		const bool bHasDefault = Function && Function->HasMetaData(*DefaultKey);
		Result->SetBoolField(TEXT("has_default"), bHasDefault);
		Result->SetStringField(TEXT("default_value"), bHasDefault ? Function->GetMetaData(*DefaultKey) : FString());
		Result->SetBoolField(TEXT("required"), Direction(Property) == TEXT("input") && !bHasDefault);
		return Result;
	}

	static const TArray<FString>& BehavioralMetadataNames()
	{
		static const TArray<FString> Names = {
			TEXT("Latent"), TEXT("LatentInfo"), TEXT("WorldContext"), TEXT("DefaultToSelf"),
			TEXT("CallableWithoutWorldContext"), TEXT("HideSelfPin"), TEXT("HidePin"),
			TEXT("AutoCreateRefTerm"), TEXT("ExpandEnumAsExecs"), TEXT("ExpandBoolAsExecs"),
			TEXT("DeterminesOutputType"), TEXT("DynamicOutputType"), TEXT("DynamicOutputParam"),
			TEXT("CustomStructureParam"), TEXT("ArrayParm"), TEXT("ArrayTypeDependentParams"),
			TEXT("ArrayDependentParam"), TEXT("SetParam"), TEXT("MapParam"), TEXT("MapKeyParam"),
			TEXT("MapValueParam"), TEXT("CommutativeAssociativeBinaryOperator"), TEXT("CustomThunk"),
			TEXT("Variadic"), TEXT("BlueprintInternalUseOnly"), TEXT("DeprecatedFunction"),
			TEXT("UnsafeDuringActorConstruction"), TEXT("UnsafeForConstructionScripts"),
		};
		return Names;
	}

	static TSharedPtr<FJsonObject> SerializeMetadata(const UFunction* Function)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Behavioral = MakeShared<FJsonObject>();
		for (const FString& Name : BehavioralMetadataNames())
		{
			if (Function->HasMetaData(*Name)) Behavioral->SetStringField(Name, Function->GetMetaData(*Name));
		}
		TSharedPtr<FJsonObject> Presentation = MakeShared<FJsonObject>();
		static const TArray<FString> PresentationNames = {
			TEXT("AdvancedDisplay"), TEXT("CompactNodeTitle"), TEXT("DisplayName"), TEXT("Category")
		};
		for (const FString& Name : PresentationNames)
		{
			if (Function->HasMetaData(*Name)) Presentation->SetStringField(Name, Function->GetMetaData(*Name));
		}
		TArray<FString> Keys;
		if (const TMap<FName, FString>* Map = FMetaData::GetMapForObject(Function))
			for (const TPair<FName, FString>& Pair : *Map) Keys.Add(Pair.Key.ToString());
		Keys.Sort();
		TArray<TSharedPtr<FJsonValue>> KeyValues;
		for (const FString& Key : Keys) KeyValues.Add(MakeShared<FJsonValueString>(Key));
		Result->SetObjectField(TEXT("behavioral"), Behavioral);
		Result->SetObjectField(TEXT("presentation"), Presentation);
		Result->SetArrayField(TEXT("all_keys"), KeyValues);
		return Result;
	}

	static FString GuidForMember(const FFieldVariant& Field, UClass* OwnerClass)
	{
		if (!OwnerClass) return FString();
		FGuid Guid;
		bool bFound = false;
		if (const UFunction* Function = Cast<UFunction>(Field.ToUObject()))
			bFound = UBlueprint::GetGuidFromClassByFieldName<UFunction>(OwnerClass, Function->GetFName(), Guid);
		else if (const FProperty* Property = Field.Get<FProperty>())
			bFound = UBlueprint::GetGuidFromClassByFieldName<FProperty>(OwnerClass, Property->GetFName(), Guid);
		return bFound && Guid.IsValid() ? Guid.ToString(EGuidFormats::Digits) : FString();
	}

	static FString ModuleProvenance(const UObject* Object)
	{
		if (!Object) return FString();
		const FString PackageName = Object->GetOutermost()->GetName();
		return PackageName.StartsWith(TEXT("/Script/")) ? PackageName.RightChop(8) : PackageName;
	}

	static TSharedPtr<FJsonObject> SerializeFunction(const UFunction* Function)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UClass* Declaring = Function->GetOwnerClass();
		UClass* Authoritative = Declaring ? Declaring->GetAuthoritativeClass() : nullptr;
		Result->SetStringField(TEXT("path"), Function->GetPathName());
		Result->SetStringField(TEXT("native_member"), Function->GetName());
		Result->SetStringField(TEXT("declaring_owner"), ObjectPath(Declaring));
		Result->SetStringField(TEXT("authoritative_owner"), ObjectPath(Authoritative));
		Result->SetStringField(TEXT("blueprint_member_guid"), GuidForMember(FFieldVariant(const_cast<UFunction*>(Function)), Declaring));
		Result->SetStringField(TEXT("module_or_plugin"), ModuleProvenance(Declaring));
		Result->SetStringField(TEXT("call_mode"), Function->HasAnyFunctionFlags(FUNC_Static) ? TEXT("static") : TEXT("instance"));
		Result->SetStringField(TEXT("function_flags"), FString::Printf(TEXT("0x%016llx"),
			static_cast<unsigned long long>(Function->FunctionFlags)));
		TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
		Flags->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		Flags->SetBoolField(TEXT("blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
		Flags->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
		Flags->SetBoolField(TEXT("const"), Function->HasAnyFunctionFlags(FUNC_Const));
		Flags->SetBoolField(TEXT("net"), Function->HasAnyFunctionFlags(FUNC_Net));
		Flags->SetBoolField(TEXT("net_server"), Function->HasAnyFunctionFlags(FUNC_NetServer));
		Flags->SetBoolField(TEXT("net_client"), Function->HasAnyFunctionFlags(FUNC_NetClient));
		Flags->SetBoolField(TEXT("net_multicast"), Function->HasAnyFunctionFlags(FUNC_NetMulticast));
		Flags->SetBoolField(TEXT("net_reliable"), Function->HasAnyFunctionFlags(FUNC_NetReliable));
		Result->SetObjectField(TEXT("flags"), Flags);
		Result->SetObjectField(TEXT("metadata"), SerializeMetadata(Function));
		TArray<TSharedPtr<FJsonValue>> Parameters;
		int32 Index = 0;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			Parameters.Add(MakeShared<FJsonValueObject>(SerializeProperty(*It, Function, Index++)));
		Result->SetArrayField(TEXT("parameters"), Parameters);
		return Result;
	}

	static void AddContextAvailability(
		TSharedPtr<FJsonObject>& Result,
		const UObject* ActionOwner,
		const UBlueprintNodeSpawner* Spawner,
		const TArray<FContextProfile>& Contexts)
	{
		TArray<TSharedPtr<FJsonValue>> Availability;
		for (const FContextProfile& Profile : Contexts)
		{
			FBlueprintActionFilter Filter;
			Filter.Context.Blueprints.Add(Profile.Blueprint);
			Filter.Context.Graphs.Add(Profile.Graph);
			FBlueprintActionInfo Info(ActionOwner, Spawner);
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("context_id"), Profile.Id);
			Entry->SetBoolField(TEXT("available"), !Filter.IsFiltered(Info));
			Availability.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("context_availability"), Availability);
	}

	static bool MatchesQuery(const FString& Query, const TArray<FString>& Values)
	{
		if (Query.IsEmpty()) return true;
		for (const FString& Value : Values)
			if (Value.Contains(Query, ESearchCase::IgnoreCase)) return true;
		return false;
	}
}

TSharedPtr<FJsonValue> FBlueprintHandlers::DiscoverBlueprintActions(const TSharedPtr<FJsonObject>& Params)
{
	using namespace BlueprintActionDiscovery;
	double RequestedOffset = 0.0;
	Params->TryGetNumberField(TEXT("offset"), RequestedOffset);
	const int32 Offset = FMath::Max(0, static_cast<int32>(RequestedOffset));
	int32 Limit = DefaultLimit;
	double RequestedLimit = 0.0;
	if (Params->TryGetNumberField(TEXT("limit"), RequestedLimit)) Limit = FMath::Clamp(static_cast<int32>(RequestedLimit), 1, MaximumLimit);
	const FString Query = OptionalString(Params, TEXT("query"), TEXT(""));
	bool bIncludeTemplateEvidence = false;
	Params->TryGetBoolField(TEXT("includeTemplateEvidence"), bIncludeTemplateEvidence);

	TArray<FContextProfile> Contexts;
	const TArray<TSharedPtr<FJsonValue>>* ContextValues = nullptr;
	if (Params->TryGetArrayField(TEXT("contexts"), ContextValues) && ContextValues)
	{
		if (ContextValues->Num() > MaximumContexts) return MCPError(TEXT("At most 8 discovery contexts are supported"));
		for (const TSharedPtr<FJsonValue>& Value : *ContextValues)
		{
			const TSharedPtr<FJsonObject>* ContextObject = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ContextObject) || !ContextObject || !ContextObject->IsValid())
				return MCPError(TEXT("Each discovery context must be an object"));
			FContextProfile Profile;
			if (!(*ContextObject)->TryGetStringField(TEXT("id"), Profile.Id)
				|| !(*ContextObject)->TryGetStringField(TEXT("assetPath"), Profile.AssetPath)
				|| !(*ContextObject)->TryGetStringField(TEXT("graphName"), Profile.GraphName)
				|| Profile.Id.IsEmpty())
				return MCPError(TEXT("Each discovery context requires id, assetPath, and graphName"));
			Profile.Blueprint = LoadBlueprint(Profile.AssetPath);
			if (!Profile.Blueprint) return MCPError(FString::Printf(TEXT("Context Blueprint not found: %s"), *Profile.AssetPath));
			Profile.Graph = FindGraph(Profile.Blueprint, Profile.GraphName);
			if (!Profile.Graph) return MCPError(FString::Printf(TEXT("Context graph not found: %s in %s"), *Profile.GraphName, *Profile.AssetPath));
			Profile.bPackageDirtyBefore = Profile.Blueprint->GetOutermost()->IsDirty();
			Contexts.Add(Profile);
		}
	}

	FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
	const FBlueprintActionDatabase::FActionRegistry& Registry = Database.GetAllActions();
	TArray<FActionRecord> Records;
	int32 RawActionCount = 0;
	for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : Registry)
	{
		const UObject* ActionOwner = Pair.Key.ResolveObjectPtr();
		if (!ActionOwner) continue;
		for (const UBlueprintNodeSpawner* Spawner : Pair.Value)
		{
			if (!Spawner || !Spawner->NodeClass) continue;
			++RawActionCount;
			FBlueprintActionInfo Info(ActionOwner, Spawner);
			const FFieldVariant AssociatedField = Info.GetAssociatedMemberField();
			const UFunction* Function = Info.GetAssociatedFunction();
			const FProperty* Property = Info.GetAssociatedProperty();
			const FString ActionOwnerPath = ActionOwner->GetPathName();
			const FString SpawnerClassPath = Spawner->GetClass()->GetPathName();
			const FString NodeClassPath = Spawner->NodeClass->GetPathName();
			const FString AssociatedPath = FieldPath(AssociatedField);
			if (!MatchesQuery(Query, { ActionOwnerPath, SpawnerClassPath, NodeClassPath, AssociatedPath,
				Function ? Function->GetName() : FString(), Property ? Property->GetName() : FString() })) continue;

			const FBlueprintNodeSignature Signature = Spawner->GetSpawnerSignature();
			TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
			Action->SetStringField(TEXT("action_owner"), ActionOwnerPath);
			Action->SetStringField(TEXT("action_owner_class"), ActionOwner->GetClass()->GetPathName());
			Action->SetStringField(TEXT("spawner_class"), SpawnerClassPath);
			Action->SetStringField(TEXT("k2_node_class"), NodeClassPath);
			Action->SetStringField(TEXT("associated_field_kind"), Function ? TEXT("function") : Property ? TEXT("property")
				: AssociatedField.IsValid() ? TEXT("field") : TEXT("none"));
			Action->SetStringField(TEXT("associated_field_path"), AssociatedPath);
			Action->SetStringField(TEXT("spawner_signature"), Signature.ToString());
			Action->SetStringField(TEXT("spawner_signature_guid"), Signature.AsGuid().ToString(EGuidFormats::Digits));
			FBlueprintActionContext UiContext;
			if (Contexts.Num() > 0)
			{
				UiContext.Blueprints.Add(Contexts[0].Blueprint);
				UiContext.Graphs.Add(Contexts[0].Graph);
			}
			const FBlueprintActionUiSpec Ui = Contexts.Num() > 0
				? Spawner->GetUiSpec(UiContext, Info.GetBindings()) : Spawner->PrimeDefaultUiSpec();
			Action->SetStringField(TEXT("display_name"), Ui.MenuName.ToString());
			Action->SetStringField(TEXT("category"), Ui.Category.ToString());
			if (Function) Action->SetObjectField(TEXT("function"), SerializeFunction(Function));
			if (Property)
			{
				TSharedPtr<FJsonObject> PropertyJson = SerializeProperty(Property, nullptr, 0);
				UStruct* DeclaringStruct = Property->GetOwnerStruct();
				UClass* DeclaringClass = Cast<UClass>(DeclaringStruct);
				PropertyJson->SetStringField(TEXT("path"), Property->GetPathName());
				PropertyJson->SetStringField(TEXT("declaring_owner"), ObjectPath(DeclaringStruct));
				PropertyJson->SetStringField(TEXT("authoritative_owner"), ObjectPath(DeclaringClass ? DeclaringClass->GetAuthoritativeClass() : DeclaringStruct));
				PropertyJson->SetStringField(TEXT("blueprint_member_guid"), GuidForMember(FFieldVariant(const_cast<FProperty*>(Property)), DeclaringClass));
				PropertyJson->SetStringField(TEXT("module_or_plugin"), ModuleProvenance(DeclaringStruct));
				Action->SetObjectField(TEXT("property"), PropertyJson);
			}
			AddContextAvailability(Action, ActionOwner, Spawner, Contexts);
			if (bIncludeTemplateEvidence)
			{
				UEdGraph* TargetGraph = Contexts.Num() > 0 ? Contexts[0].Graph : nullptr;
				UEdGraphNode* Template = Spawner->GetTemplateNode(TargetGraph);
				TSharedPtr<FJsonObject> TemplateEvidence = MakeShared<FJsonObject>();
				TemplateEvidence->SetBoolField(TEXT("available"), Template != nullptr);
				TemplateEvidence->SetStringField(TEXT("node_class"), Template ? Template->GetClass()->GetPathName() : FString());
				TemplateEvidence->SetStringField(TEXT("outer_class"), Template && Template->GetOuter() ? Template->GetOuter()->GetClass()->GetPathName() : FString());
				TemplateEvidence->SetBoolField(TEXT("outer_transient"), Template && Template->GetOutermost() == GetTransientPackage());
				TemplateEvidence->SetNumberField(TEXT("pin_count"), Template ? Template->Pins.Num() : 0);
				Action->SetObjectField(TEXT("template_evidence"), TemplateEvidence);
			}
			const TArray<FString> SortParts = { ActionOwnerPath, SpawnerClassPath, NodeClassPath, AssociatedPath,
				Signature.ToString() };
			const FString SortKey = FString::Join(SortParts, TEXT("\x1f"));
			Records.Add({ SortKey, Action });
		}
	}
	Records.Sort([](const FActionRecord& A, const FActionRecord& B) { return A.SortKey < B.SortKey; });

	TArray<TSharedPtr<FJsonValue>> Actions;
	const int32 End = FMath::Min(Records.Num(), Offset + Limit);
	for (int32 Index = FMath::Min(Offset, Records.Num()); Index < End; ++Index)
		Actions.Add(MakeShared<FJsonValueObject>(Records[Index].Json));
	TArray<TSharedPtr<FJsonValue>> ContextEvidence;
	bool bPersistentMutationObserved = false;
	for (const FContextProfile& Profile : Contexts)
	{
		const bool bAfter = Profile.Blueprint->GetOutermost()->IsDirty();
		bPersistentMutationObserved |= bAfter != Profile.bPackageDirtyBefore;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("id"), Profile.Id);
		Entry->SetStringField(TEXT("asset_path"), Profile.AssetPath);
		Entry->SetStringField(TEXT("graph_name"), Profile.GraphName);
		Entry->SetStringField(TEXT("graph_class"), Profile.Graph->GetClass()->GetPathName());
		Entry->SetStringField(TEXT("schema_class"), Profile.Graph->GetSchema() ? Profile.Graph->GetSchema()->GetClass()->GetPathName() : FString());
		Entry->SetBoolField(TEXT("package_dirty_before"), Profile.bPackageDirtyBefore);
		Entry->SetBoolField(TEXT("package_dirty_after"), bAfter);
		ContextEvidence.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Environment = MakeShared<FJsonObject>();
	Environment->SetStringField(TEXT("ue_version"), FEngineVersion::Current().ToString());
	Environment->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Environment->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Environment->SetBoolField(TEXT("type_promotion_enabled"), GetDefault<UBlueprintEditorSettings>()->bEnableTypePromotion);
	TArray<FString> EnabledPluginNames;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		EnabledPluginNames.Add(FString::Printf(TEXT("%s@%s"), *Plugin->GetName(), *Plugin->GetDescriptor().VersionName));
	EnabledPluginNames.Sort();
	TArray<TSharedPtr<FJsonValue>> EnabledPlugins;
	for (const FString& Plugin : EnabledPluginNames) EnabledPlugins.Add(MakeShared<FJsonValueString>(Plugin));
	Environment->SetArrayField(TEXT("enabled_plugins"), EnabledPlugins);

	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetStringField(TEXT("contract_version"), TEXT("spacehead.blueprint-action-discovery@1.0"));
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("mutation_performed"), false);
	Result->SetBoolField(TEXT("persistent_mutation_observed"), bPersistentMutationObserved);
	Result->SetNumberField(TEXT("raw_action_count"), RawActionCount);
	Result->SetNumberField(TEXT("matched_action_count"), Records.Num());
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("returned_action_count"), Actions.Num());
	Result->SetBoolField(TEXT("has_more"), End < Records.Num());
	Result->SetNumberField(TEXT("next_offset"), End);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("contexts"), ContextEvidence);
	Result->SetObjectField(TEXT("environment"), Environment);
	return MCPResult(Result);
}
