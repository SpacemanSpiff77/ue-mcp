#include "BlueprintHandlers.h"
#include "BlueprintTopologySerializer.h"
#include "HandlerUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UObject/Package.h"

namespace
{
	constexpr int32 DefaultMaxNodes = 512;
	constexpr int32 DefaultMaxPins = 4096;
	constexpr int32 DefaultMaxConnections = 8192;
	constexpr int32 HardMaxNodes = 4096;
	constexpr int32 HardMaxPins = 32768;
	constexpr int32 HardMaxConnections = 65536;

	struct FTopologyNode
	{
		UEdGraphNode* Node = nullptr;
		FString SortKey;
		FString Id;
	};

	struct FTopologyPin
	{
		UEdGraphPin* Pin = nullptr;
		FString SortKey;
		FString Id;
	};

	struct FTopologyConnection
	{
		FString SortKey;
		TSharedPtr<FJsonObject> Json;
	};

	FString GuidString(const FGuid& Guid)
	{
		return Guid.IsValid() ? Guid.ToString(EGuidFormats::Digits) : FString();
	}

	FString ContainerTypeString(EPinContainerType Type)
	{
		switch (Type)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set: return TEXT("set");
		case EPinContainerType::Map: return TEXT("map");
		default: return TEXT("none");
		}
	}

	FString ObjectPath(const TWeakObjectPtr<UObject>& Object)
	{
		const UObject* Value = Object.Get();
		return Value ? Value->GetPathName() : FString();
	}

	TSharedPtr<FJsonObject> TerminalTypeJson(const FEdGraphTerminalType& Type)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("category"), Type.TerminalCategory.ToString());
		Json->SetStringField(TEXT("subCategory"), Type.TerminalSubCategory.ToString());
		Json->SetStringField(TEXT("subCategoryObject"), ObjectPath(Type.TerminalSubCategoryObject));
		Json->SetBoolField(TEXT("isConst"), Type.bTerminalIsConst);
		Json->SetBoolField(TEXT("isWeakPointer"), Type.bTerminalIsWeakPointer);
		Json->SetBoolField(TEXT("isUObjectWrapper"), Type.bTerminalIsUObjectWrapper);
		return Json;
	}

	TSharedPtr<FJsonObject> SimpleMemberReferenceJson(const FSimpleMemberReference& Reference)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		const UObject* Parent = Reference.MemberParent.Get();
		Json->SetStringField(TEXT("memberName"), Reference.MemberName.ToString());
		Json->SetStringField(TEXT("memberParent"), Parent ? Parent->GetPathName() : FString());
		Json->SetStringField(TEXT("memberGuid"), GuidString(Reference.MemberGuid));
		return Json;
	}

	TSharedPtr<FJsonObject> PinTypeJson(const FEdGraphPinType& Type)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("category"), Type.PinCategory.ToString());
		Json->SetStringField(TEXT("subCategory"), Type.PinSubCategory.ToString());
		Json->SetStringField(TEXT("subCategoryObject"), ObjectPath(Type.PinSubCategoryObject));
		Json->SetObjectField(TEXT("subCategoryMemberReference"), SimpleMemberReferenceJson(Type.PinSubCategoryMemberReference));
		Json->SetStringField(TEXT("containerType"), ContainerTypeString(Type.ContainerType));
		Json->SetBoolField(TEXT("isReference"), Type.bIsReference);
		Json->SetBoolField(TEXT("isConst"), Type.bIsConst);
		Json->SetBoolField(TEXT("isWeakPointer"), Type.bIsWeakPointer);
		Json->SetBoolField(TEXT("isUObjectWrapper"), Type.bIsUObjectWrapper);
		Json->SetBoolField(TEXT("serializeAsSinglePrecisionFloat"), Type.bSerializeAsSinglePrecisionFloat);
		Json->SetObjectField(TEXT("valueTerminalType"), TerminalTypeJson(Type.PinValueType));
		return Json;
	}

	FString NodeRole(UEdGraphNode* Node)
	{
		const FString ClassName = Node->GetClass()->GetName();
		if (Cast<UK2Node_FunctionEntry>(Node)) return TEXT("entry");
		if (ClassName == TEXT("K2Node_FunctionResult")) return TEXT("result");
		if (Cast<UK2Node_VariableGet>(Node)) return TEXT("variable-get");
		if (Cast<UK2Node_VariableSet>(Node)) return TEXT("variable-set");
		if (Cast<UK2Node_CallFunction>(Node)) return TEXT("function-call");
		if (Cast<UK2Node_MacroInstance>(Node))
		{
			const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (Title.Contains(TEXT("Loop"), ESearchCase::IgnoreCase)
				|| Title.Contains(TEXT("For Each"), ESearchCase::IgnoreCase)
				|| Title.Contains(TEXT("While"), ESearchCase::IgnoreCase))
			{
				return TEXT("loop");
			}
			return TEXT("macro");
		}
		if (Cast<UK2Node_IfThenElse>(Node)) return TEXT("branch");
		if (Cast<UK2Node_DynamicCast>(Node)) return TEXT("cast");
		if (ClassName.Contains(TEXT("ExecutionSequence"))) return TEXT("sequence");
		if (ClassName.Contains(TEXT("Switch"))) return TEXT("switch");
		return TEXT("other");
	}

	void WriteMemberReference(
		const FMemberReference& Reference,
		const TCHAR* FieldName,
		TSharedPtr<FJsonObject>& NodeJson,
		FString& SemanticIdentity)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		const FString Name = Reference.GetMemberName().ToString();
		const UClass* Parent = Reference.GetMemberParentClass();
		Json->SetStringField(TEXT("name"), Name);
		Json->SetStringField(TEXT("owner"), Parent ? Parent->GetPathName() : FString());
		Json->SetBoolField(TEXT("selfContext"), Reference.IsSelfContext());
		Json->SetStringField(TEXT("memberGuid"), GuidString(Reference.GetMemberGuid()));
		NodeJson->SetObjectField(FieldName, Json);
		if (!Name.IsEmpty()) SemanticIdentity = Name;
	}

	TSharedPtr<FJsonObject> BaseResult(
		const FString& AssetPath,
		const FString& ObjectPathValue,
		const FString& FunctionName,
		bool bDirtyBefore)
	{
		TSharedPtr<FJsonObject> Result = MCPSuccess();
		Result->SetStringField(TEXT("contractVersion"), TEXT("spacehead.selected-function-topology@1.0"));
		Result->SetStringField(TEXT("status"), TEXT("unresolved"));
		Result->SetStringField(TEXT("assetPath"), AssetPath);
		Result->SetStringField(TEXT("objectPath"), ObjectPathValue);
		Result->SetStringField(TEXT("functionName"), FunctionName);
		Result->SetStringField(TEXT("scanScope"), TEXT("selected-function"));
		Result->SetBoolField(TEXT("graphInventoryComplete"), false);
		Result->SetBoolField(TEXT("complete"), false);
		Result->SetBoolField(TEXT("truncated"), false);
		Result->SetBoolField(TEXT("dataOmitted"), false);
		Result->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
		Result->SetBoolField(TEXT("compileRequested"), false);
		Result->SetBoolField(TEXT("saveRequested"), false);
		Result->SetBoolField(TEXT("reconstructRequested"), false);
		Result->SetBoolField(TEXT("mutationOperationsPerformed"), false);

		TSharedPtr<FJsonObject> Provider = MakeShared<FJsonObject>();
		Provider->SetStringField(TEXT("name"), TEXT("UE_MCP_Bridge"));
		Provider->SetStringField(TEXT("version"), TEXT("0.3.0"));
		Provider->SetStringField(TEXT("tool"), TEXT("blueprint.read_function_topology"));
		Provider->SetStringField(TEXT("toolVersion"), TEXT("1.0.0"));
		Result->SetObjectField(TEXT("provider"), Provider);
		Result->SetArrayField(TEXT("graphInventory"), {});
		Result->SetArrayField(TEXT("graphs"), {});
		return Result;
	}

	void FinishDirtyState(TSharedPtr<FJsonObject>& Result, UPackage* Package, bool bDirtyBefore)
	{
		const bool bDirtyAfter = Package && Package->IsDirty();
		Result->SetBoolField(TEXT("dirtyAfter"), bDirtyAfter);
		Result->SetBoolField(TEXT("dirtyStateChanged"), bDirtyBefore != bDirtyAfter);
		Result->SetBoolField(TEXT("mutationGuardPassed"), bDirtyBefore == bDirtyAfter);
	}
}

TSharedPtr<FJsonValue> FBlueprintHandlers::ReadBlueprintFunctionTopology(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Error = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Error;
	FString FunctionName;
	if (auto Error = RequireString(Params, TEXT("functionName"), FunctionName)) return Error;

	const int32 MaxNodes = FMath::Clamp(OptionalInt(Params, TEXT("maxNodes"), DefaultMaxNodes), 1, HardMaxNodes);
	const int32 MaxPins = FMath::Clamp(OptionalInt(Params, TEXT("maxPins"), DefaultMaxPins), 1, HardMaxPins);
	const int32 MaxConnections = FMath::Clamp(OptionalInt(Params, TEXT("maxConnections"), DefaultMaxConnections), 1, HardMaxConnections);

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MCPError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UPackage* Package = Blueprint->GetOutermost();
	const bool bDirtyBefore = Package && Package->IsDirty();
	const FString BlueprintObjectPath = Blueprint->GetPathName();
	TSharedPtr<FJsonObject> Result = BaseResult(AssetPath, BlueprintObjectPath, FunctionName, bDirtyBefore);

	TArray<UEdGraph*> Matches;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::CaseSensitive))
		{
			Matches.Add(Graph);
		}
	}

	Matches.Sort([](const UEdGraph& A, const UEdGraph& B)
	{
		const FString AKey = GuidString(A.GraphGuid) + TEXT(":") + A.GetPathName();
		const FString BKey = GuidString(B.GraphGuid) + TEXT(":") + B.GetPathName();
		return AKey < BKey;
	});

	if (Matches.Num() != 1)
	{
		const bool bMissing = Matches.IsEmpty();
		Result->SetStringField(TEXT("status"), bMissing ? TEXT("missing") : TEXT("ambiguous"));
		Result->SetStringField(TEXT("errorCode"), bMissing ? TEXT("function-missing") : TEXT("function-ambiguous"));
		Result->SetNumberField(TEXT("exactMatchCount"), Matches.Num());
		TArray<TSharedPtr<FJsonValue>> Candidates;
		for (UEdGraph* Graph : Matches)
		{
			TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), Graph->GetName());
			Candidate->SetStringField(TEXT("objectPath"), Graph->GetPathName());
			Candidate->SetStringField(TEXT("graphGuid"), GuidString(Graph->GraphGuid));
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		Result->SetArrayField(TEXT("functionCandidates"), Candidates);
		FinishDirtyState(Result, Package, bDirtyBefore);
		return MCPResult(Result);
	}

	UEdGraph* TargetGraph = Matches[0];
	int32 TotalNodes = 0;
	int32 TotalPins = 0;
	int32 TotalConnections = 0;
	for (UEdGraphNode* Node : TargetGraph->Nodes)
	{
		if (!Node) continue;
		++TotalNodes;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			++TotalPins;
			if (Pin->Direction == EGPD_Output) TotalConnections += Pin->LinkedTo.Num();
		}
	}
	const bool bTruncated = TotalNodes > MaxNodes || TotalPins > MaxPins || TotalConnections > MaxConnections;

	Result->SetStringField(TEXT("status"), bTruncated ? TEXT("truncated") : TEXT("exact"));
	Result->SetNumberField(TEXT("exactMatchCount"), 1);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetBoolField(TEXT("dataOmitted"), bTruncated);
	Result->SetNumberField(TEXT("totalNodeCount"), TotalNodes);
	Result->SetNumberField(TEXT("totalPinCount"), TotalPins);
	Result->SetNumberField(TEXT("totalConnectionCount"), TotalConnections);

	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("maxNodes"), MaxNodes);
	Limits->SetNumberField(TEXT("maxPins"), MaxPins);
	Limits->SetNumberField(TEXT("maxConnections"), MaxConnections);
	Result->SetObjectField(TEXT("limits"), Limits);

	TSharedPtr<FJsonObject> InventoryItem = MakeShared<FJsonObject>();
	InventoryItem->SetStringField(TEXT("name"), TargetGraph->GetName());
	InventoryItem->SetStringField(TEXT("type"), TEXT("function"));
	InventoryItem->SetStringField(TEXT("functionName"), FunctionName);
	InventoryItem->SetStringField(TEXT("guid"), GuidString(TargetGraph->GraphGuid));
	Result->SetArrayField(TEXT("graphInventory"), { MakeShared<FJsonValueObject>(InventoryItem) });

	if (bTruncated)
	{
		TSharedPtr<FJsonObject> Omission = MakeShared<FJsonObject>();
		Omission->SetStringField(TEXT("reason"), TEXT("bounded topology limits exceeded; no graph slice was returned"));
		Omission->SetBoolField(TEXT("allOrNothing"), true);
		Omission->SetNumberField(TEXT("omittedNodeCount"), TotalNodes);
		Omission->SetNumberField(TEXT("omittedPinCount"), TotalPins);
		Omission->SetNumberField(TEXT("omittedConnectionCount"), TotalConnections);
		Result->SetObjectField(TEXT("omission"), Omission);
		FinishDirtyState(Result, Package, bDirtyBefore);
		return MCPResult(Result);
	}

	UE_MCP_BlueprintTopology::FGraphSerializationOptions SerializeOptions;
	SerializeOptions.GraphIdentity = BlueprintObjectPath + TEXT("::function::") + TargetGraph->GetPathName();
	SerializeOptions.NormalizedGraphType = TEXT("function");
	SerializeOptions.FunctionName = FunctionName;
	const UE_MCP_BlueprintTopology::FSerializedGraphTopology Serialized =
		UE_MCP_BlueprintTopology::SerializeGraph(TargetGraph, SerializeOptions);
	Result->SetArrayField(TEXT("graphs"), { MakeShared<FJsonValueObject>(Serialized.Graph) });
	Result->SetNumberField(TEXT("unresolvedEndpointCount"), Serialized.UnresolvedEndpointCount);

	FinishDirtyState(Result, Package, bDirtyBefore);
	const bool bComplete = Serialized.UnresolvedEndpointCount == 0 && !Result->GetBoolField(TEXT("dirtyStateChanged"));
	Result->SetBoolField(TEXT("complete"), bComplete);
	if (!bComplete) Result->SetStringField(TEXT("status"), TEXT("partial"));
	return MCPResult(Result);
}
