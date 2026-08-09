#include "BlueprintHandlers.h"
#include "BlueprintTopologySerializer.h"
#include "HandlerUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* ContractVersion = TEXT("blueprint.apply_build_spec@1.0");
	const TCHAR* EndpointName = TEXT("blueprint.apply_build_spec");

	struct FCachedBuildSpecExecution
	{
		FString InputHash;
		TSharedPtr<FJsonObject> Response;
	};

	TMap<FString, FCachedBuildSpecExecution> CorrelatedExecutions;

	TSharedPtr<FJsonObject> ObjectField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Name, Value) ? *Value : nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Name, Value) ? Value : nullptr;
	}

	FString JsonString(const TSharedPtr<FJsonObject>& Object)
	{
		FString Value;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Value);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Value;
	}

	FString Sha256(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		FSHA256Signature Signature{};
		if (!FPlatformMisc::GetSHA256Signature(Utf8.Get(), Utf8.Length(), Signature))
		{
			return FString();
		}
		return Signature.ToString().ToLower();
	}

	TSharedPtr<FJsonObject> BaseResponse(
		const FString& RequestId,
		const FString& InputHash)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("contractVersion"), ContractVersion);
		Result->SetStringField(TEXT("endpoint"), EndpointName);
		Result->SetStringField(TEXT("requestId"), RequestId);
		Result->SetStringField(TEXT("inputHash"), InputHash);
		Result->SetStringField(TEXT("transactionState"), TEXT("failed-without-mutation"));
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("finalStage"), TEXT("NOT_SAVED"));
		Result->SetArrayField(TEXT("stages"), {});
		Result->SetBoolField(TEXT("targetIdentityReconfirmed"), false);
		Result->SetBoolField(TEXT("baselineTopologyReconfirmed"), false);
		Result->SetBoolField(TEXT("rollbackStateCaptured"), false);
		Result->SetBoolField(TEXT("mutationAttempted"), false);
		Result->SetBoolField(TEXT("mutationPerformed"), false);
		Result->SetObjectField(TEXT("compile"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("postBuildScan"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("verification"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("save"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("rollback"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("dirtyPackage"), MakeShared<FJsonObject>());
		Result->SetArrayField(TEXT("operationResults"), {});
		Result->SetArrayField(TEXT("errorCodes"), {});
		return Result;
	}

	void Step(TSharedPtr<FJsonObject>& Result, const TCHAR* Stage)
	{
		TArray<TSharedPtr<FJsonValue>> Stages = Result->GetArrayField(TEXT("stages"));
		Stages.Add(MakeShared<FJsonValueString>(Stage));
		Result->SetArrayField(TEXT("stages"), Stages);
		Result->SetStringField(TEXT("finalStage"), Stage);
	}

	void SetAttemptResult(
		TSharedPtr<FJsonObject>& Result,
		const TCHAR* Field,
		bool bAttempted,
		bool bSucceeded)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetBoolField(TEXT("attempted"), bAttempted);
		Value->SetBoolField(TEXT("succeeded"), bSucceeded);
		Result->SetObjectField(Field, Value);
	}

	void SetRollback(
		TSharedPtr<FJsonObject>& Result,
		bool bAttempted,
		const TCHAR* RollbackResult,
		TOptional<bool> bRestored,
		const FString& EvidenceHash)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetBoolField(TEXT("attempted"), bAttempted);
		Value->SetStringField(TEXT("result"), RollbackResult);
		if (bRestored.IsSet()) Value->SetBoolField(TEXT("originalStateRestored"), bRestored.GetValue());
		else Value->SetField(TEXT("originalStateRestored"), MakeShared<FJsonValueNull>());
		if (EvidenceHash.IsEmpty()) Value->SetField(TEXT("evidenceHash"), MakeShared<FJsonValueNull>());
		else Value->SetStringField(TEXT("evidenceHash"), EvidenceHash);
		Result->SetObjectField(TEXT("rollback"), Value);
	}

	void SetDirty(TSharedPtr<FJsonObject>& Result, bool bBefore, bool bAfter)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetBoolField(TEXT("before"), bBefore);
		Value->SetBoolField(TEXT("after"), bAfter);
		Result->SetObjectField(TEXT("dirtyPackage"), Value);
	}

	void SetErrors(TSharedPtr<FJsonObject>& Result, const TArray<FString>& Errors)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Error : Errors) Values.Add(MakeShared<FJsonValueString>(Error));
		Result->SetArrayField(TEXT("errorCodes"), Values);
	}

	void SetOperation(
		TSharedPtr<FJsonObject>& Result,
		const FString& OperationId,
		const TCHAR* Status,
		const FString& ErrorCode = FString())
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("operationId"), OperationId);
		Value->SetStringField(TEXT("type"), TEXT("set_pin_default"));
		Value->SetStringField(TEXT("status"), Status);
		if (!ErrorCode.IsEmpty()) Value->SetStringField(TEXT("errorCode"), ErrorCode);
		Result->SetArrayField(TEXT("operationResults"), { MakeShared<FJsonValueObject>(Value) });
	}

	bool ValidHash(const FString& Value)
	{
		if (Value.Len() != 64) return false;
		for (TCHAR Character : Value)
			if (!FChar::IsHexDigit(Character)) return false;
		return true;
	}

	UEdGraph* ResolveTargetGraph(UBlueprint* Blueprint, const FString& Kind, const FString& Name)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		TArray<UEdGraph*> Matches;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph || !Graph->GetName().Equals(Name, ESearchCase::CaseSensitive)) continue;
			const bool bFunction = Blueprint->FunctionGraphs.Contains(Graph);
			if ((Kind == TEXT("function") && bFunction) || (Kind == TEXT("graph") && !bFunction))
				Matches.Add(Graph);
		}
		return Matches.Num() == 1 ? Matches[0] : nullptr;
	}

	TSharedPtr<FJsonObject> SerializeBlueprint(
		UBlueprint* Blueprint,
		bool& bComplete,
		TSharedPtr<FJsonObject>& TargetGraphJson,
		UEdGraph* TargetGraph)
	{
		bComplete = true;
		TargetGraphJson.Reset();
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		Graphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GetPathName() < B.GetPathName(); });
		TArray<TSharedPtr<FJsonValue>> Values;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			UE_MCP_BlueprintTopology::FGraphSerializationOptions Options;
			Options.GraphIdentity = Graph->GetPathName();
			Options.NormalizedGraphType = Blueprint->FunctionGraphs.Contains(Graph)
				? TEXT("function") : TEXT("graph");
			Options.FunctionName = Blueprint->FunctionGraphs.Contains(Graph) ? Graph->GetName() : FString();
			Options.bRejectDuplicateNodeScopedPinIdentity = true;
			const UE_MCP_BlueprintTopology::FSerializedGraphTopology Serialized =
				UE_MCP_BlueprintTopology::SerializeGraph(Graph, Options);
			bComplete &= Serialized.IsComplete();
			Values.Add(MakeShared<FJsonValueObject>(Serialized.Graph));
			if (Graph == TargetGraph) TargetGraphJson = Serialized.Graph;
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("graphs"), Values);
		return Result;
	}

	TSharedPtr<FJsonObject> FindNodeJson(
		const TSharedPtr<FJsonObject>& Graph,
		const FString& Guid)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = ArrayField(Graph, TEXT("nodes"));
		if (!Nodes) return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject> Node = Value->AsObject();
			FString NodeGuid;
			if (Node.IsValid() && Node->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
				&& NodeGuid.Equals(Guid, ESearchCase::IgnoreCase))
				return Node;
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindPinJson(
		const TSharedPtr<FJsonObject>& Node,
		const FString& Name,
		const FString& Direction)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pins = ArrayField(Node, TEXT("pins"));
		if (!Pins) return nullptr;
		TSharedPtr<FJsonObject> Match;
		for (const TSharedPtr<FJsonValue>& Value : *Pins)
		{
			const TSharedPtr<FJsonObject> Pin = Value->AsObject();
			FString PinName;
			FString PinDirection;
			if (Pin.IsValid()
				&& Pin->TryGetStringField(TEXT("name"), PinName)
				&& Pin->TryGetStringField(TEXT("direction"), PinDirection)
				&& PinName == Name && PinDirection == Direction)
			{
				if (Match.IsValid()) return nullptr;
				Match = Pin;
			}
		}
		return Match;
	}

	bool BaselineMatches(
		const TSharedPtr<FJsonObject>& Approved,
		const TSharedPtr<FJsonObject>& Current)
	{
		FString ApprovedGraphGuid;
		FString CurrentGraphGuid;
		if (!Approved->TryGetStringField(TEXT("graphGuid"), ApprovedGraphGuid)
			|| !Current->TryGetStringField(TEXT("graphGuid"), CurrentGraphGuid)
			|| !ApprovedGraphGuid.Equals(CurrentGraphGuid, ESearchCase::IgnoreCase))
			return false;
		const TArray<TSharedPtr<FJsonValue>>* ApprovedNodes = ArrayField(Approved, TEXT("nodes"));
		const TArray<TSharedPtr<FJsonValue>>* CurrentNodes = ArrayField(Current, TEXT("nodes"));
		const TArray<TSharedPtr<FJsonValue>>* ApprovedConnections = ArrayField(Approved, TEXT("connections"));
		const TArray<TSharedPtr<FJsonValue>>* CurrentConnections = ArrayField(Current, TEXT("connections"));
		if (!ApprovedNodes || !CurrentNodes || !ApprovedConnections || !CurrentConnections
			|| ApprovedNodes->Num() != CurrentNodes->Num()
			|| ApprovedConnections->Num() != CurrentConnections->Num())
			return false;
		for (const TSharedPtr<FJsonValue>& NodeValue : *ApprovedNodes)
		{
			const TSharedPtr<FJsonObject> ApprovedNode = NodeValue->AsObject();
			FString Guid;
			if (!ApprovedNode.IsValid() || !ApprovedNode->TryGetStringField(TEXT("unrealGuid"), Guid))
				return false;
			const TSharedPtr<FJsonObject> CurrentNode = FindNodeJson(Current, Guid);
			if (!CurrentNode.IsValid()) return false;
			FString ApprovedClass;
			FString CurrentClass;
			FString ApprovedKind;
			FString CurrentKind;
			if (!ApprovedNode->TryGetStringField(TEXT("nodeClass"), ApprovedClass)
				|| !CurrentNode->TryGetStringField(TEXT("nodeClass"), CurrentClass)
				|| !ApprovedNode->TryGetStringField(TEXT("kind"), ApprovedKind)
				|| !CurrentNode->TryGetStringField(TEXT("kind"), CurrentKind)
				|| ApprovedClass != CurrentClass || ApprovedKind != CurrentKind)
				return false;
			int32 ApprovedPinCount = 0;
			for (const TCHAR* Collection : { TEXT("inputPins"), TEXT("outputPins") })
			{
				const TArray<TSharedPtr<FJsonValue>>* Pins = ArrayField(ApprovedNode, Collection);
				if (!Pins) return false;
				ApprovedPinCount += Pins->Num();
				for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
				{
					const TSharedPtr<FJsonObject> ApprovedPin = PinValue->AsObject();
					FString Name;
					FString Direction;
					FString Type;
					FString Container;
					FString Default;
					if (!ApprovedPin.IsValid()
						|| !ApprovedPin->TryGetStringField(TEXT("name"), Name)
						|| !ApprovedPin->TryGetStringField(TEXT("direction"), Direction)
						|| !ApprovedPin->TryGetStringField(TEXT("type"), Type)
						|| !ApprovedPin->TryGetStringField(TEXT("containerType"), Container))
						return false;
					ApprovedPin->TryGetStringField(TEXT("defaultValue"), Default);
					const TSharedPtr<FJsonObject> CurrentPin = FindPinJson(CurrentNode, Name, Direction);
					if (!CurrentPin.IsValid()
						|| CurrentPin->GetStringField(TEXT("type")) != Type
						|| CurrentPin->GetStringField(TEXT("containerType")) != Container
						|| CurrentPin->GetStringField(TEXT("defaultValue")) != Default)
						return false;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* CurrentPins = ArrayField(CurrentNode, TEXT("pins"));
			if (!CurrentPins || CurrentPins->Num() != ApprovedPinCount) return false;
		}
		TArray<FString> ApprovedConnectionKeys;
		TArray<FString> CurrentConnectionKeys;
		for (const TSharedPtr<FJsonValue>& Value : *ApprovedConnections)
		{
			const TSharedPtr<FJsonObject> Connection = Value->AsObject();
			if (!Connection.IsValid()) return false;
			ApprovedConnectionKeys.Add(
				Connection->GetStringField(TEXT("sourceNodeId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("sourcePinId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("targetNodeId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("targetPinId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("classification")));
		}
		for (const TSharedPtr<FJsonValue>& Value : *CurrentConnections)
		{
			const TSharedPtr<FJsonObject> Connection = Value->AsObject();
			if (!Connection.IsValid()) return false;
			CurrentConnectionKeys.Add(
				Connection->GetStringField(TEXT("sourceNodeId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("sourcePinId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("targetNodeId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("targetPinId")) + TEXT(":")
				+ Connection->GetStringField(TEXT("classification")));
		}
		ApprovedConnectionKeys.Sort();
		CurrentConnectionKeys.Sort();
		if (ApprovedConnectionKeys != CurrentConnectionKeys) return false;
		return true;
	}

	FString PrimitiveDefault(const TSharedPtr<FJsonValue>& Value, bool& bValid)
	{
		bValid = true;
		switch (Value.IsValid() ? Value->Type : EJson::None)
		{
		case EJson::Null: return FString();
		case EJson::String: return Value->AsString();
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number: return FString::SanitizeFloat(Value->AsNumber());
		default: bValid = false; return FString();
		}
	}

	bool Compile(UBlueprint* Blueprint)
	{
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave, &Log);
		return Log.NumErrors == 0 && Blueprint->Status != EBlueprintStatus::BS_Error;
	}
}

TSharedPtr<FJsonValue> FBlueprintHandlers::ApplyBuildSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestKind = TEXT("apply");
	Params->TryGetStringField(TEXT("requestKind"), RequestKind);
	if (RequestKind == TEXT("capabilities"))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("contractVersion"), ContractVersion);
		Result->SetStringField(TEXT("endpoint"), EndpointName);
		Result->SetArrayField(TEXT("supportedOperations"), {
			MakeShared<FJsonValueString>(TEXT("set_pin_default"))
		});
		return MCPResult(Result);
	}

	FString Contract;
	FString Endpoint;
	FString RequestId;
	FString InputHash;
	if (!Params->TryGetStringField(TEXT("contractVersion"), Contract)
		|| Contract != ContractVersion
		|| !Params->TryGetStringField(TEXT("endpoint"), Endpoint)
		|| Endpoint != EndpointName
		|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
		|| RequestId.IsEmpty()
		|| !Params->TryGetStringField(TEXT("inputHash"), InputHash)
		|| !ValidHash(InputHash))
		return MCPError(TEXT("Invalid atomic Build Spec correlation contract"));

	if (const FCachedBuildSpecExecution* Cached = CorrelatedExecutions.Find(RequestId))
	{
		if (Cached->InputHash != InputHash) return MCPError(TEXT("REQUEST_ID_CONFLICT"));
		return MCPResult(Cached->Response);
	}
	if (RequestKind == TEXT("status"))
	{
		TSharedPtr<FJsonObject> Unknown = BaseResponse(RequestId, InputHash);
		Unknown->SetStringField(TEXT("transactionState"), TEXT("unknown"));
		SetAttemptResult(Unknown, TEXT("compile"), false, false);
		SetAttemptResult(Unknown, TEXT("postBuildScan"), false, false);
		SetAttemptResult(Unknown, TEXT("verification"), false, false);
		SetAttemptResult(Unknown, TEXT("save"), false, false);
		SetRollback(Unknown, false, TEXT("not-attempted"), TOptional<bool>(), FString());
		SetDirty(Unknown, false, false);
		SetErrors(Unknown, { TEXT("EXECUTION_STATE_UNKNOWN") });
		return MCPResult(Unknown);
	}

	TSharedPtr<FJsonObject> Result = BaseResponse(RequestId, InputHash);
	CorrelatedExecutions.Add(RequestId, { InputHash, Result });
	SetAttemptResult(Result, TEXT("compile"), false, false);
	SetAttemptResult(Result, TEXT("postBuildScan"), false, false);
	SetAttemptResult(Result, TEXT("verification"), false, false);
	SetAttemptResult(Result, TEXT("save"), false, false);
	SetRollback(Result, false, TEXT("not-attempted"), TOptional<bool>(), FString());

	const TSharedPtr<FJsonObject> Spec = ObjectField(Params, TEXT("buildSpec"));
	const TSharedPtr<FJsonObject> Target = ObjectField(Spec, TEXT("target"));
	const TSharedPtr<FJsonObject> Selector = ObjectField(Target, TEXT("selector"));
	const TSharedPtr<FJsonObject> Approved = ObjectField(Params, TEXT("approvedBaselineTopology"));
	const TArray<TSharedPtr<FJsonValue>>* Operations = ArrayField(Spec, TEXT("operations"));
	FString AssetPath;
	FString SelectorKind;
	FString SelectorName;
	if (!Spec.IsValid() || !Target.IsValid() || !Selector.IsValid() || !Approved.IsValid()
		|| !Target->TryGetStringField(TEXT("assetPath"), AssetPath)
		|| !Selector->TryGetStringField(TEXT("kind"), SelectorKind)
		|| !Selector->TryGetStringField(TEXT("name"), SelectorName)
		|| !Operations || Operations->Num() != 1
		|| !AssetPath.StartsWith(TEXT("/Game/")))
	{
		SetErrors(Result, { TEXT("PREFLIGHT_FAILED") });
		Step(Result, TEXT("FAILED")); Step(Result, TEXT("NOT_SAVED"));
		SetDirty(Result, false, false);
		return MCPResult(Result);
	}
	const TSharedPtr<FJsonObject> Operation = (*Operations)[0]->AsObject();
	const TSharedPtr<FJsonObject> PinReference = ObjectField(Operation, TEXT("pin"));
	const TSharedPtr<FJsonObject> NodeReference = ObjectField(PinReference, TEXT("node"));
	FString OperationId;
	FString OperationType;
	FString NodeKind;
	FString NodeGuid;
	FString PinName;
	FString PinDirection;
	if (!Operation.IsValid() || !PinReference.IsValid() || !NodeReference.IsValid()
		|| !Operation->TryGetStringField(TEXT("operationId"), OperationId)
		|| !Operation->TryGetStringField(TEXT("type"), OperationType)
		|| OperationType != TEXT("set_pin_default")
		|| !NodeReference->TryGetStringField(TEXT("kind"), NodeKind)
		|| NodeKind != TEXT("existing")
		|| !NodeReference->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
		|| !PinReference->TryGetStringField(TEXT("pinName"), PinName)
		|| !PinReference->TryGetStringField(TEXT("direction"), PinDirection)
		|| PinDirection != TEXT("input"))
	{
		SetOperation(Result, OperationId, TEXT("failed"), TEXT("PREFLIGHT_FAILED"));
		SetErrors(Result, { TEXT("PREFLIGHT_FAILED") });
		Step(Result, TEXT("FAILED")); Step(Result, TEXT("NOT_SAVED"));
		SetDirty(Result, false, false);
		return MCPResult(Result);
	}
	bool bValueValid = false;
	const FString NewDefault = PrimitiveDefault(Operation->TryGetField(TEXT("value")), bValueValid);
	if (!bValueValid)
	{
		SetOperation(Result, OperationId, TEXT("failed"), TEXT("PREFLIGHT_FAILED"));
		SetErrors(Result, { TEXT("PREFLIGHT_FAILED") });
		Step(Result, TEXT("FAILED")); Step(Result, TEXT("NOT_SAVED"));
		SetDirty(Result, false, false);
		return MCPResult(Result);
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	UEdGraph* Graph = Blueprint ? ResolveTargetGraph(Blueprint, SelectorKind, SelectorName) : nullptr;
	UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
	const bool bDirtyBefore = Package && Package->IsDirty();
	SetDirty(Result, bDirtyBefore, bDirtyBefore);
	if (!Blueprint || !Graph || !Package || bDirtyBefore)
	{
		SetOperation(Result, OperationId, TEXT("failed"), TEXT("PREFLIGHT_FAILED"));
		SetErrors(Result, { TEXT("PREFLIGHT_FAILED") });
		Step(Result, TEXT("FAILED")); Step(Result, TEXT("NOT_SAVED"));
		return MCPResult(Result);
	}

	UEdGraphNode* TargetNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
		if (Node && Node->NodeGuid.ToString(EGuidFormats::Digits).Equals(NodeGuid, ESearchCase::IgnoreCase))
			TargetNode = TargetNode ? nullptr : Node;
	if (!TargetNode)
	{
		SetOperation(Result, OperationId, TEXT("failed"), TEXT("PREFLIGHT_FAILED"));
		SetErrors(Result, { TEXT("PREFLIGHT_FAILED") });
		Step(Result, TEXT("FAILED")); Step(Result, TEXT("NOT_SAVED"));
		return MCPResult(Result);
	}
	UEdGraphPin* TargetPin = nullptr;
	for (UEdGraphPin* Pin : TargetNode->Pins)
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString() == PinName)
			TargetPin = TargetPin ? nullptr : Pin;
	if (!TargetPin || TargetPin->bDefaultValueIsReadOnly || !TargetPin->LinkedTo.IsEmpty()
		|| TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		SetOperation(Result, OperationId, TEXT("failed"), TEXT("PREFLIGHT_FAILED"));
		SetErrors(Result, { TEXT("PREFLIGHT_FAILED") });
		Step(Result, TEXT("FAILED")); Step(Result, TEXT("NOT_SAVED"));
		return MCPResult(Result);
	}

	bool bOriginalComplete = false;
	TSharedPtr<FJsonObject> OriginalTarget;
	const TSharedPtr<FJsonObject> OriginalTopology =
		SerializeBlueprint(Blueprint, bOriginalComplete, OriginalTarget, Graph);
	if (!bOriginalComplete || !OriginalTarget.IsValid() || !BaselineMatches(Approved, OriginalTarget))
	{
		SetOperation(Result, OperationId, TEXT("failed"), TEXT("PREFLIGHT_FAILED"));
		SetErrors(Result, { TEXT("PREFLIGHT_FAILED") });
		Step(Result, TEXT("FAILED")); Step(Result, TEXT("NOT_SAVED"));
		return MCPResult(Result);
	}
	Result->SetBoolField(TEXT("targetIdentityReconfirmed"), true);
	Result->SetBoolField(TEXT("baselineTopologyReconfirmed"), true);
	Result->SetBoolField(TEXT("rollbackStateCaptured"), true);
	Step(Result, TEXT("PREFLIGHT_CHECKED"));
	const FString OriginalDefault = TargetPin->DefaultValue;
	const FString OriginalTopologyHash = Sha256(JsonString(OriginalTopology));
	const FString RollbackEvidenceHash = Sha256(
		OriginalTopologyHash + TEXT(":") + OriginalDefault + TEXT(":clean"));

	auto FailAfterMutation = [&](const FString& ErrorCode) -> TSharedPtr<FJsonValue>
	{
		Step(Result, TEXT("FAILED"));
		Step(Result, TEXT("ROLLBACK_STARTED"));
		TargetNode->Modify();
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema) Schema->TrySetDefaultValue(*TargetPin, OriginalDefault);
		const bool bRestoredValue = Schema && TargetPin->DefaultValue == OriginalDefault;
		const bool bRecompiled = bRestoredValue && Compile(Blueprint);
		bool bRollbackComplete = false;
		TSharedPtr<FJsonObject> RollbackTarget;
		const TSharedPtr<FJsonObject> RollbackTopology =
			SerializeBlueprint(Blueprint, bRollbackComplete, RollbackTarget, Graph);
		const bool bVerified = bRecompiled && bRollbackComplete
			&& Sha256(JsonString(RollbackTopology)) == OriginalTopologyHash;
		Package->SetDirtyFlag(false);
		SetDirty(Result, false, Package->IsDirty());
		SetAttemptResult(Result, TEXT("postBuildScan"), true, bRollbackComplete);
		SetAttemptResult(Result, TEXT("verification"), true, false);
		SetAttemptResult(Result, TEXT("save"), false, false);
		if (bVerified)
		{
			Result->SetStringField(TEXT("transactionState"), TEXT("rolled-back-successfully"));
			SetRollback(Result, true, TEXT("succeeded"), true, RollbackEvidenceHash);
			Step(Result, TEXT("ROLLBACK_COMPLETED"));
			SetErrors(Result, { ErrorCode });
		}
		else
		{
			Result->SetStringField(TEXT("transactionState"), TEXT("rollback-failed"));
			SetRollback(Result, true, TEXT("failed"), false, FString());
			Step(Result, TEXT("ROLLBACK_FAILED"));
			SetErrors(Result, { ErrorCode, TEXT("ROLLBACK_FAILED") });
		}
		SetOperation(Result, OperationId, TEXT("failed"), ErrorCode);
		Step(Result, TEXT("NOT_SAVED"));
		return MCPResult(Result);
	};

	Result->SetBoolField(TEXT("mutationAttempted"), true);
	Step(Result, TEXT("MUTATION_STARTED"));
	Blueprint->Modify();
	TargetNode->Modify();
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema) return FailAfterMutation(TEXT("MUTATION_FAILED"));
	Schema->TrySetDefaultValue(*TargetPin, NewDefault);
	if (TargetPin->DefaultValue != NewDefault)
		return FailAfterMutation(TEXT("MUTATION_FAILED"));
	Result->SetBoolField(TEXT("mutationPerformed"), true);

	const TSharedPtr<FJsonObject> TestHooks = ObjectField(Params, TEXT("testHooks"));
	bool bForceFailure = false;
	if (TestHooks.IsValid()) TestHooks->TryGetBoolField(TEXT("forceFailureAfterMutation"), bForceFailure);
	if (bForceFailure && AssetPath.StartsWith(TEXT("/Game/Tests/Builder/")))
		return FailAfterMutation(TEXT("VERIFICATION_FAILED"));

	const bool bCompiled = Compile(Blueprint);
	SetAttemptResult(Result, TEXT("compile"), true, bCompiled);
	if (!bCompiled) return FailAfterMutation(TEXT("COMPILE_FAILED"));
	Step(Result, TEXT("COMPILED"));

	bool bPostComplete = false;
	TSharedPtr<FJsonObject> PostTarget;
	const TSharedPtr<FJsonObject> PostTopology =
		SerializeBlueprint(Blueprint, bPostComplete, PostTarget, Graph);
	TSharedPtr<FJsonObject> PostScan = MakeShared<FJsonObject>();
	PostScan->SetBoolField(TEXT("attempted"), true);
	PostScan->SetBoolField(TEXT("succeeded"), bPostComplete);
	PostScan->SetStringField(TEXT("topologyLogicHash"), Sha256(JsonString(PostTarget)));
	Result->SetObjectField(TEXT("postBuildScan"), PostScan);
	if (!bPostComplete || !PostTarget.IsValid()) return FailAfterMutation(TEXT("VERIFICATION_FAILED"));
	Step(Result, TEXT("POST_BUILD_SCANNED"));

	const TSharedPtr<FJsonObject> PostNode = FindNodeJson(PostTarget, NodeGuid);
	const TSharedPtr<FJsonObject> PostPin = FindPinJson(PostNode, PinName, TEXT("input"));
	const bool bExpectedDefault = PostPin.IsValid()
		&& PostPin->GetStringField(TEXT("defaultValue")) == NewDefault;
	if (PostPin.IsValid()) PostPin->SetStringField(TEXT("defaultValue"), OriginalDefault);
	const bool bOnlyRequestedFieldChanged =
		Sha256(JsonString(PostTopology)) == OriginalTopologyHash;
	const bool bVerified = bExpectedDefault && bOnlyRequestedFieldChanged;
	SetAttemptResult(Result, TEXT("verification"), true, bVerified);
	if (!bVerified) return FailAfterMutation(TEXT("VERIFICATION_FAILED"));
	Step(Result, TEXT("VERIFIED"));

	const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
	SetAttemptResult(Result, TEXT("save"), true, bSaved);
	if (!bSaved) return FailAfterMutation(TEXT("SAVE_FAILED"));
	Step(Result, TEXT("SAVED"));
	Result->SetStringField(TEXT("transactionState"), TEXT("completed-successfully"));
	Result->SetBoolField(TEXT("success"), true);
	SetRollback(Result, false, TEXT("not-attempted"), TOptional<bool>(), FString());
	SetDirty(Result, false, Package->IsDirty());
	SetOperation(Result, OperationId, TEXT("succeeded"));
	SetErrors(Result, {});
	Step(Result, TEXT("COMPLETED"));
	return MCPResult(Result);
}
