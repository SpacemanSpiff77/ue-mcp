#include "BlueprintTopologySerializer.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

namespace
{
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

	TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}
}

UE_MCP_BlueprintTopology::FSerializedGraphTopology UE_MCP_BlueprintTopology::SerializeGraph(
	UEdGraph* Graph,
	const FGraphSerializationOptions& Options)
{
	FSerializedGraphTopology Serialized;
	if (!Graph)
	{
		return Serialized;
	}

	TArray<FTopologyNode> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const FString NativeGuid = GuidString(Node->NodeGuid);
		const FString SortKey = (NativeGuid.IsEmpty() ? TEXT("1:") : TEXT("0:") + NativeGuid)
			+ TEXT(":") + Node->GetClass()->GetName()
			+ TEXT(":") + Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()
			+ FString::Printf(TEXT(":%d:%d"), Node->NodePosX, Node->NodePosY);
		Nodes.Add({ Node, SortKey, FString() });
	}
	Nodes.Sort([](const FTopologyNode& A, const FTopologyNode& B) { return A.SortKey < B.SortKey; });

	TMap<const UEdGraphNode*, FString> NodeIds;
	TMap<const UEdGraphPin*, FString> PinIds;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const FString NativeGuid = GuidString(Nodes[NodeIndex].Node->NodeGuid);
		Nodes[NodeIndex].Id = NativeGuid.IsEmpty()
			? FString::Printf(TEXT("local-node-%04d"), NodeIndex)
			: NativeGuid;
		NodeIds.Add(Nodes[NodeIndex].Node, Nodes[NodeIndex].Id);

		TArray<FTopologyPin> Pins;
		for (UEdGraphPin* Pin : Nodes[NodeIndex].Node->Pins)
		{
			if (!Pin) continue;
			const FString NativePinGuid = GuidString(Pin->PinId);
			const FString Direction = Pin->Direction == EGPD_Input ? TEXT("0") : TEXT("1");
			const FString SortKey = Direction + TEXT(":") + Pin->PinName.ToString()
				+ TEXT(":") + Pin->PinType.PinCategory.ToString()
				+ TEXT(":") + ContainerTypeString(Pin->PinType.ContainerType)
				+ TEXT(":") + NativePinGuid;
			Pins.Add({ Pin, SortKey, FString() });
		}
		Pins.Sort([](const FTopologyPin& A, const FTopologyPin& B) { return A.SortKey < B.SortKey; });

		TSet<FString> NodeScopedPinIds;
		for (int32 PinIndex = 0; PinIndex < Pins.Num(); ++PinIndex)
		{
			const FString NativePinGuid = GuidString(Pins[PinIndex].Pin->PinId);
			Pins[PinIndex].Id = NativePinGuid.IsEmpty()
				? FString::Printf(TEXT("%s:local-pin-%04d"), *Nodes[NodeIndex].Id, PinIndex)
				: NativePinGuid;
			if (NodeScopedPinIds.Contains(Pins[PinIndex].Id))
			{
				++Serialized.DuplicatePinIdentityCount;
			}
			NodeScopedPinIds.Add(Pins[PinIndex].Id);
			PinIds.Add(Pins[PinIndex].Pin, Pins[PinIndex].Id);
		}
	}

	TArray<TSharedPtr<FJsonValue>> NodeArray;
	TArray<TSharedPtr<FJsonValue>> EntryNodeIds;
	TArray<TSharedPtr<FJsonValue>> ResultNodeIds;
	for (const FTopologyNode& NodeRecord : Nodes)
	{
		UEdGraphNode* Node = NodeRecord.Node;
		const FString NativeGuid = GuidString(Node->NodeGuid);
		const FString Role = NodeRole(Node);
		FString SemanticIdentity = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("id"), NodeRecord.Id);
		NodeJson->SetStringField(TEXT("nodeGuid"), NativeGuid);
		NodeJson->SetBoolField(TEXT("nodeGuidAvailable"), !NativeGuid.IsEmpty());
		NodeJson->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
		NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		NodeJson->SetStringField(TEXT("kind"), Role);
		NodeJson->SetStringField(TEXT("semanticRole"), Role);
		TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), Node->NodePosX);
		Position->SetNumberField(TEXT("y"), Node->NodePosY);
		NodeJson->SetObjectField(TEXT("position"), Position);

		if (UK2Node_VariableGet* VariableGet = Cast<UK2Node_VariableGet>(Node))
		{
			TSharedPtr<FJsonObject> VariableJson = MakeShared<FJsonObject>();
			const FMemberReference& Reference = VariableGet->VariableReference;
			const UClass* Parent = Reference.GetMemberParentClass();
			const FString Name = Reference.GetMemberName().ToString();
			VariableJson->SetStringField(TEXT("operation"), TEXT("get"));
			VariableJson->SetStringField(TEXT("name"), Name);
			VariableJson->SetStringField(TEXT("owner"), Parent ? Parent->GetPathName() : FString());
			VariableJson->SetBoolField(TEXT("selfContext"), Reference.IsSelfContext());
			VariableJson->SetStringField(TEXT("memberGuid"), GuidString(Reference.GetMemberGuid()));
			NodeJson->SetObjectField(TEXT("variable"), VariableJson);
			if (!Name.IsEmpty()) SemanticIdentity = Name;
		}
		else if (UK2Node_VariableSet* VariableSet = Cast<UK2Node_VariableSet>(Node))
		{
			TSharedPtr<FJsonObject> VariableJson = MakeShared<FJsonObject>();
			const FMemberReference& Reference = VariableSet->VariableReference;
			const UClass* Parent = Reference.GetMemberParentClass();
			const FString Name = Reference.GetMemberName().ToString();
			VariableJson->SetStringField(TEXT("operation"), TEXT("set"));
			VariableJson->SetStringField(TEXT("name"), Name);
			VariableJson->SetStringField(TEXT("owner"), Parent ? Parent->GetPathName() : FString());
			VariableJson->SetBoolField(TEXT("selfContext"), Reference.IsSelfContext());
			VariableJson->SetStringField(TEXT("memberGuid"), GuidString(Reference.GetMemberGuid()));
			NodeJson->SetObjectField(TEXT("variable"), VariableJson);
			if (!Name.IsEmpty()) SemanticIdentity = Name;
		}
		else if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			WriteMemberReference(Call->FunctionReference, TEXT("calledFunction"), NodeJson, SemanticIdentity);
		}
		else if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			TSharedPtr<FJsonObject> MacroJson = MakeShared<FJsonObject>();
			UEdGraph* MacroGraph = Macro->GetMacroGraph();
			const FString Name = MacroGraph ? MacroGraph->GetName() : FString();
			MacroJson->SetStringField(TEXT("name"), Name);
			MacroJson->SetStringField(TEXT("owner"), MacroGraph ? MacroGraph->GetPathName() : FString());
			NodeJson->SetObjectField(TEXT("macro"), MacroJson);
			if (!Name.IsEmpty()) SemanticIdentity = Name;
		}
		NodeJson->SetStringField(TEXT("semanticIdentity"), SemanticIdentity);

		TArray<FTopologyPin> Pins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			const FString NativePinGuid = GuidString(Pin->PinId);
			const FString DirectionKey = Pin->Direction == EGPD_Input ? TEXT("0") : TEXT("1");
			const FString SortKey = DirectionKey + TEXT(":") + Pin->PinName.ToString()
				+ TEXT(":") + Pin->PinType.PinCategory.ToString()
				+ TEXT(":") + ContainerTypeString(Pin->PinType.ContainerType)
				+ TEXT(":") + NativePinGuid;
			Pins.Add({ Pin, SortKey, PinIds.FindRef(Pin) });
		}
		Pins.Sort([](const FTopologyPin& A, const FTopologyPin& B) { return A.SortKey < B.SortKey; });

		TArray<TSharedPtr<FJsonValue>> PinArray;
		for (const FTopologyPin& PinRecord : Pins)
		{
			UEdGraphPin* Pin = PinRecord.Pin;
			const FString NativePinGuid = GuidString(Pin->PinId);
			TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("id"), PinRecord.Id);
			PinJson->SetStringField(TEXT("pinGuid"), NativePinGuid);
			PinJson->SetBoolField(TEXT("pinGuidAvailable"), !NativePinGuid.IsEmpty());
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinJson->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
			PinJson->SetStringField(TEXT("subtype"), Pin->PinType.PinSubCategory.ToString());
			PinJson->SetStringField(TEXT("containerType"), ContainerTypeString(Pin->PinType.ContainerType));
			PinJson->SetStringField(TEXT("classification"),
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? TEXT("execution") : TEXT("data"));
			PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
			PinJson->SetStringField(TEXT("defaultObject"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
			PinJson->SetStringField(TEXT("defaultTextValue"), Pin->DefaultTextValue.ToString());
			PinJson->SetObjectField(TEXT("typeInfo"), PinTypeJson(Pin->PinType));
			PinJson->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);
			PinArray.Add(MakeShared<FJsonValueObject>(PinJson));
		}
		NodeJson->SetArrayField(TEXT("pins"), PinArray);
		NodeArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		if (Role == TEXT("entry")) EntryNodeIds.Add(MakeShared<FJsonValueString>(NodeRecord.Id));
		if (Role == TEXT("result")) ResultNodeIds.Add(MakeShared<FJsonValueString>(NodeRecord.Id));
	}

	TArray<FTopologyConnection> ConnectionRecords;
	TArray<FTopologyConnection> UnresolvedRecords;
	for (const FTopologyNode& NodeRecord : Nodes)
	{
		for (UEdGraphPin* Pin : NodeRecord.Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				UEdGraphNode* TargetNode = Linked ? Linked->GetOwningNode() : nullptr;
				const FString SourcePinId = PinIds.FindRef(Pin);
				const FString TargetNodeId = NodeIds.FindRef(TargetNode);
				const FString TargetPinId = PinIds.FindRef(Linked);
				const bool bResolved = Linked
					&& TargetNode
					&& Linked->Direction == EGPD_Input
					&& !SourcePinId.IsEmpty()
					&& !TargetNodeId.IsEmpty()
					&& !TargetPinId.IsEmpty();
				if (!bResolved)
				{
					TSharedPtr<FJsonObject> Unresolved = MakeShared<FJsonObject>();
					Unresolved->SetStringField(TEXT("reason"), !Linked ? TEXT("null linked pin")
						: Linked->Direction != EGPD_Input ? TEXT("linked endpoint is not an input pin")
						: Options.bIncludeFullBlueprintMetadata
							? TEXT("linked endpoint is outside the captured graph")
							: TEXT("linked endpoint is outside the captured function graph"));
					TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
					Source->SetStringField(TEXT("nodeId"), NodeRecord.Id);
					Source->SetStringField(TEXT("pinId"), SourcePinId);
					Source->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
					Unresolved->SetObjectField(TEXT("source"), Source);
					TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("nodeId"), TargetNodeId);
					Target->SetStringField(TEXT("pinId"), TargetPinId);
					Target->SetStringField(TEXT("pinName"), Linked ? Linked->PinName.ToString() : FString());
					Unresolved->SetObjectField(TEXT("target"), Target);
					const FString SortKey = NodeRecord.Id + TEXT(":") + SourcePinId
						+ TEXT(":") + TargetNodeId + TEXT(":") + TargetPinId
						+ TEXT(":") + Unresolved->GetStringField(TEXT("reason"));
					UnresolvedRecords.Add({ SortKey, Unresolved });
					continue;
				}

				TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
				Connection->SetStringField(TEXT("sourceNodeId"), NodeRecord.Id);
				Connection->SetStringField(TEXT("sourcePinId"), SourcePinId);
				Connection->SetStringField(TEXT("sourcePinName"), Pin->PinName.ToString());
				Connection->SetStringField(TEXT("targetNodeId"), TargetNodeId);
				Connection->SetStringField(TEXT("targetPinId"), TargetPinId);
				Connection->SetStringField(TEXT("targetPinName"), Linked->PinName.ToString());
				Connection->SetStringField(TEXT("classification"),
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? TEXT("execution") : TEXT("data"));
				const FString SortKey = NodeRecord.Id + TEXT(":") + SourcePinId + TEXT(":") + TargetNodeId + TEXT(":") + TargetPinId;
				ConnectionRecords.Add({ SortKey, Connection });
			}
		}
	}
	ConnectionRecords.Sort([](const FTopologyConnection& A, const FTopologyConnection& B) { return A.SortKey < B.SortKey; });
	UnresolvedRecords.Sort([](const FTopologyConnection& A, const FTopologyConnection& B) { return A.SortKey < B.SortKey; });

	TArray<TSharedPtr<FJsonValue>> Connections;
	for (const FTopologyConnection& Record : ConnectionRecords)
	{
		Connections.Add(MakeShared<FJsonValueObject>(Record.Json));
	}
	TArray<TSharedPtr<FJsonValue>> UnresolvedEndpoints;
	for (const FTopologyConnection& Record : UnresolvedRecords)
	{
		UnresolvedEndpoints.Add(MakeShared<FJsonValueObject>(Record.Json));
	}

	TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
	const FString NativeGraphGuid = GuidString(Graph->GraphGuid);
	GraphJson->SetStringField(TEXT("graphName"), Graph->GetName());
	GraphJson->SetStringField(TEXT("graphType"), Options.NormalizedGraphType);
	if (!Options.FunctionName.IsEmpty())
	{
		GraphJson->SetStringField(TEXT("functionName"), Options.FunctionName);
	}
	GraphJson->SetStringField(TEXT("graphGuid"), NativeGraphGuid);
	GraphJson->SetBoolField(TEXT("graphGuidAvailable"), !NativeGraphGuid.IsEmpty());
	GraphJson->SetStringField(TEXT("graphIdentity"), Options.GraphIdentity);
	if (Options.bIncludeFullBlueprintMetadata)
	{
		GraphJson->SetStringField(TEXT("objectPath"), Graph->GetPathName());
		GraphJson->SetStringField(TEXT("graphClass"), Graph->GetClass()->GetPathName());
		GraphJson->SetStringField(TEXT("schemaClass"), Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetPathName() : FString());
		GraphJson->SetStringField(TEXT("normalizedGraphType"), Options.NormalizedGraphType);
		GraphJson->SetArrayField(TEXT("collectionMemberships"), StringArray(Options.CollectionMemberships));
		GraphJson->SetStringField(TEXT("ownershipKind"), Options.OwnershipKind);
		GraphJson->SetStringField(TEXT("parentGraphIdentity"), Options.ParentGraphIdentity);
		GraphJson->SetNumberField(TEXT("nestingDepth"), Options.NestingDepth);
		GraphJson->SetStringField(TEXT("graphProvenance"), Options.GraphProvenance);
	}
	GraphJson->SetArrayField(TEXT("nodes"), NodeArray);
	GraphJson->SetArrayField(TEXT("connections"), Connections);
	GraphJson->SetArrayField(TEXT("entryNodeIds"), EntryNodeIds);
	GraphJson->SetArrayField(TEXT("resultNodeIds"), ResultNodeIds);
	GraphJson->SetArrayField(TEXT("unresolvedEndpoints"), UnresolvedEndpoints);
	GraphJson->SetBoolField(TEXT("truncated"), false);
	GraphJson->SetBoolField(TEXT("dataOmitted"), false);
	const bool bDuplicatePinFailure = Options.bRejectDuplicateNodeScopedPinIdentity
		&& Serialized.DuplicatePinIdentityCount > 0;
	GraphJson->SetBoolField(TEXT("complete"), UnresolvedEndpoints.IsEmpty() && !bDuplicatePinFailure);
	GraphJson->SetNumberField(TEXT("nodeCount"), NodeArray.Num());
	GraphJson->SetNumberField(TEXT("pinCount"), PinIds.Num());
	GraphJson->SetNumberField(TEXT("connectionCount"), Connections.Num());
	GraphJson->SetNumberField(TEXT("unresolvedEndpointCount"), UnresolvedEndpoints.Num());
	if (Options.bIncludeFullBlueprintMetadata)
	{
		GraphJson->SetStringField(TEXT("pinIdentityScope"), TEXT("node"));
		GraphJson->SetNumberField(TEXT("duplicatePinIdentityCount"), Serialized.DuplicatePinIdentityCount);
	}

	Serialized.Graph = GraphJson;
	Serialized.NodeCount = NodeArray.Num();
	Serialized.PinCount = PinIds.Num();
	Serialized.ConnectionCount = Connections.Num();
	Serialized.UnresolvedEndpointCount = UnresolvedEndpoints.Num();
	return Serialized;
}
