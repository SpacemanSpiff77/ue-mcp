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
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Internationalization/Regex.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* ContractVersion = TEXT("spacehead.blueprint-atomic-bridge@1.0");
	const TCHAR* EndpointIdentity = TEXT("blueprint.apply-atomic-build-plan@1.0");
	const TCHAR* RawMethodIdentity = TEXT("apply_atomic_build_plan");
	const TCHAR* SemanticCapability = TEXT("graph.set-pin-default");
	const TCHAR* CapabilityVersion = TEXT("1.0");
	const TCHAR* OperationVersion = TEXT("graph.set-pin-default@1.0");
	const TCHAR* HandlerVersion = TEXT("spacehead.graph.set-pin-default-handler@1.0");
	const TCHAR* TopologyVersion = TEXT("spacehead.blueprint-semantic-topology@1.0");
	const TCHAR* UeMcpVersion = TEXT("1.1.36");
	const TCHAR* BridgeVersion = TEXT("0.3.0");
	const TCHAR* PluginBuildIdentity = TEXT("spacehead-pass2-atomic-build@1");
	const TCHAR* InputSchemaIdentity = TEXT("spacehead.blueprint-atomic-bridge.request@1.0");
	const TCHAR* OutputSchemaIdentity = TEXT("spacehead.blueprint-atomic-bridge.receipt@1.0");

	struct FCachedAtomicExecution
	{
		FString CorrelationId;
		FString PlanHash;
		TSharedPtr<FJsonObject> Receipt;
	};

	TMap<FString, FCachedAtomicExecution> CorrelatedExecutions;

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Name, Value) ? *Value : nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Name, Value) ? Value : nullptr;
	}

	FString CompactJson(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid()) return FString();
		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer) ? Output : FString();
	}

	FString ScalarJson(const TSharedPtr<FJsonValue>& Value)
	{
		TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetField(TEXT("v"), Value);
		const FString Serialized = CompactJson(Wrapper);
		const int32 Colon = Serialized.Find(TEXT(":"));
		return Colon == INDEX_NONE || !Serialized.EndsWith(TEXT("}"))
			? FString() : Serialized.Mid(Colon + 1, Serialized.Len() - Colon - 2);
	}

	FString CanonicalJsonValue(const TSharedPtr<FJsonValue>& Value);

	FString CanonicalJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid()) return TEXT("null");
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Fields;
		for (const auto& Pair : Object->Values)
		{
			Fields.Emplace(FString(*Pair.Key), Pair.Value);
		}
		Fields.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A,
			const TPair<FString, TSharedPtr<FJsonValue>>& B) { return A.Key < B.Key; });
		FString Result = TEXT("{");
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			if (Index > 0) Result += TEXT(",");
			Result += ScalarJson(MakeShared<FJsonValueString>(Fields[Index].Key));
			Result += TEXT(":");
			Result += CanonicalJsonValue(Fields[Index].Value);
		}
		return Result + TEXT("}");
	}

	FString CanonicalJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull()) return TEXT("null");
		if (Value->Type == EJson::Object) return CanonicalJsonObject(Value->AsObject());
		if (Value->Type == EJson::Array)
		{
			FString Result = TEXT("[");
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (Index > 0) Result += TEXT(",");
				Result += CanonicalJsonValue(Values[Index]);
			}
			return Result + TEXT("]");
		}
		return ScalarJson(Value);
	}

	FString Sha256(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		static constexpr uint32 RoundConstants[64] = {
			0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
			0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
			0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
			0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
			0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
			0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
			0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
			0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
		};
		auto RotateRight = [](uint32 Word, uint32 Bits) { return (Word >> Bits) | (Word << (32 - Bits)); };
		TArray<uint8> Message;
		Message.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		const uint64 BitLength = static_cast<uint64>(Utf8.Length()) * 8;
		Message.Add(0x80);
		while ((Message.Num() % 64) != 56) Message.Add(0);
		for (int32 Shift = 56; Shift >= 0; Shift -= 8) Message.Add(static_cast<uint8>((BitLength >> Shift) & 0xff));

		uint32 Hash[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
		for (int32 Offset = 0; Offset < Message.Num(); Offset += 64)
		{
			uint32 Words[64]{};
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const int32 Byte = Offset + Index * 4;
				Words[Index] = (static_cast<uint32>(Message[Byte]) << 24) | (static_cast<uint32>(Message[Byte + 1]) << 16)
					| (static_cast<uint32>(Message[Byte + 2]) << 8) | static_cast<uint32>(Message[Byte + 3]);
			}
			for (int32 Index = 16; Index < 64; ++Index)
			{
				const uint32 S0 = RotateRight(Words[Index - 15], 7) ^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
				const uint32 S1 = RotateRight(Words[Index - 2], 17) ^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
				Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
			}
			uint32 A=Hash[0], B=Hash[1], C=Hash[2], D=Hash[3], E=Hash[4], F=Hash[5], G=Hash[6], H=Hash[7];
			for (int32 Index = 0; Index < 64; ++Index)
			{
				const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32 Choice = (E & F) ^ ((~E) & G);
				const uint32 Temp1 = H + S1 + Choice + RoundConstants[Index] + Words[Index];
				const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32 Temp2 = S0 + Majority;
				H=G; G=F; F=E; E=D+Temp1; D=C; C=B; B=A; A=Temp1+Temp2;
			}
			Hash[0]+=A; Hash[1]+=B; Hash[2]+=C; Hash[3]+=D; Hash[4]+=E; Hash[5]+=F; Hash[6]+=G; Hash[7]+=H;
		}
		return FString::Printf(TEXT("%08x%08x%08x%08x%08x%08x%08x%08x"),
			Hash[0],Hash[1],Hash[2],Hash[3],Hash[4],Hash[5],Hash[6],Hash[7]);
	}

	bool ValidHash(const FString& Value)
	{
		if (Value.Len() != 64) return false;
		for (TCHAR Character : Value) if (!FChar::IsHexDigit(Character)) return false;
		return true;
	}

	TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Object)
	{
		TSharedPtr<FJsonObject> Clone;
		const FString Text = CanonicalJsonObject(Object);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Clone) ? Clone : nullptr;
	}

	TSharedPtr<FJsonObject> WithoutField(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Object.IsValid()) return Result;
		for (const auto& Pair : Object->Values)
		{
			const FString Key(*Pair.Key);
			if (Key != Field) Result->SetField(Key, Pair.Value);
		}
		return Result;
	}

	FString EngineIdentity()
	{
		return FString::Printf(TEXT("%d.%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION);
	}

	TSharedPtr<FJsonObject> Failure(
		const TCHAR* Phase,
		const FString& Code,
		const TCHAR* State,
		const FString& Message)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("failure_version"), TEXT("spacehead.builder-failure@1.0"));
		Value->SetStringField(TEXT("phase"), Phase);
		Value->SetStringField(TEXT("code"), Code);
		Value->SetBoolField(TEXT("retryable"), false);
		Value->SetStringField(TEXT("transaction_state"), State);
		Value->SetStringField(TEXT("message"), Message);
		return Value;
	}

	TSharedPtr<FJsonObject> BaseReceipt(
		const FString& TransactionId,
		const FString& CorrelationId,
		const TCHAR* State)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("transaction_id"), TransactionId);
		Result->SetStringField(TEXT("correlation_id"), CorrelationId);
		Result->SetBoolField(TEXT("received_by_bridge"), true);
		Result->SetStringField(TEXT("received_at"), FDateTime::UtcNow().ToIso8601());
		Result->SetStringField(TEXT("state"), State);
		Result->SetObjectField(TEXT("evidence"), MakeShared<FJsonObject>());
		return Result;
	}

	void SetFailure(
		TSharedPtr<FJsonObject>& Receipt,
		const TCHAR* Phase,
		const FString& Code,
		const TCHAR* State,
		const FString& Message)
	{
		Receipt->SetStringField(TEXT("state"), State);
		Receipt->SetObjectField(TEXT("failure"), Failure(Phase, Code, State, Message));
	}

	TSharedPtr<FJsonObject> Attempt(bool bAttempted, bool bSucceeded)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetBoolField(TEXT("attempted"), bAttempted);
		Value->SetBoolField(TEXT("succeeded"), bSucceeded);
		return Value;
	}

	UEdGraph* ResolveTargetGraph(UBlueprint* Blueprint, const FString& Kind, const FString& Name)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		UEdGraph* Match = nullptr;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph || !Graph->GetName().Equals(Name, ESearchCase::CaseSensitive)) continue;
			const bool bFunction = Blueprint->FunctionGraphs.Contains(Graph);
			if ((Kind == TEXT("function") && bFunction) || (Kind == TEXT("graph") && !bFunction))
			{
				if (Match) return nullptr;
				Match = Graph;
			}
		}
		return Match;
	}

	FString NormalizeGuid(const FString& Value)
	{
		return Value.Replace(TEXT("-"), TEXT("")).ToLower();
	}

	TSharedPtr<FJsonValue> SemanticDefault(const FString& Type, const FString& Classification, const FString& Value)
	{
		if (Classification == TEXT("execution")) return MakeShared<FJsonValueNull>();
		if (Type == TEXT("bool"))
		{
			if (Value == TEXT("true")) return MakeShared<FJsonValueBoolean>(true);
			if (Value == TEXT("false")) return MakeShared<FJsonValueBoolean>(false);
			return MakeShared<FJsonValueNull>();
		}
		return Value.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Value));
	}

	TSharedPtr<FJsonObject> SemanticTopology(UEdGraph* Graph, const FString& GraphType, bool& bComplete)
	{
		UE_MCP_BlueprintTopology::FGraphSerializationOptions Options;
		Options.GraphIdentity = Graph->GetPathName();
		Options.NormalizedGraphType = GraphType;
		Options.FunctionName = GraphType == TEXT("function") ? Graph->GetName() : FString();
		Options.bIncludeFullBlueprintMetadata = true;
		Options.bRejectDuplicateNodeScopedPinIdentity = true;
		const UE_MCP_BlueprintTopology::FSerializedGraphTopology Serialized =
			UE_MCP_BlueprintTopology::SerializeGraph(Graph, Options);
		bComplete = Serialized.IsComplete();
		if (!bComplete || !Serialized.Graph.IsValid()) return nullptr;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("topology_version"), TopologyVersion);
		TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
		GraphJson->SetStringField(TEXT("graph_identity"), Serialized.Graph->GetStringField(TEXT("graphIdentity")));
		GraphJson->SetStringField(TEXT("graph_guid"), Serialized.Graph->GetStringField(TEXT("graphGuid")));
		GraphJson->SetStringField(TEXT("graph_name"), Serialized.Graph->GetStringField(TEXT("graphName")));
		GraphJson->SetStringField(TEXT("graph_type"), Serialized.Graph->GetStringField(TEXT("graphType")));
		GraphJson->SetStringField(TEXT("schema_class"), Serialized.Graph->GetStringField(TEXT("schemaClass")));
		Result->SetObjectField(TEXT("graph"), GraphJson);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		const TArray<TSharedPtr<FJsonValue>>* SourceNodes = ArrayField(Serialized.Graph, TEXT("nodes"));
		if (!SourceNodes) return nullptr;
		for (const TSharedPtr<FJsonValue>& SourceValue : *SourceNodes)
		{
			const TSharedPtr<FJsonObject> Source = SourceValue->AsObject();
			if (!Source.IsValid()) return nullptr;
			TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("node_id"), Source->GetStringField(TEXT("id")));
			Node->SetStringField(TEXT("existing_node_guid"), Source->GetStringField(TEXT("nodeGuid")));
			Node->SetStringField(TEXT("node_class"), Source->GetStringField(TEXT("nodeClass")));
			Node->SetStringField(TEXT("semantic_type"), Source->GetStringField(TEXT("kind")));
			Node->SetObjectField(TEXT("semantic_properties"), MakeShared<FJsonObject>());
			if (const TSharedPtr<FJsonObject> Variable = ObjectField(Source, TEXT("variable")))
			{
				TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
				Reference->SetStringField(TEXT("owner"), Variable->GetStringField(TEXT("owner")));
				Reference->SetStringField(TEXT("name"), Variable->GetStringField(TEXT("name")));
				Node->SetObjectField(TEXT("member_reference"), Reference);
			}
			if (const TSharedPtr<FJsonObject> Called = ObjectField(Source, TEXT("calledFunction")))
			{
				TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
				Reference->SetStringField(TEXT("owner"), Called->GetStringField(TEXT("owner")));
				Reference->SetStringField(TEXT("name"), Called->GetStringField(TEXT("name")));
				Node->SetObjectField(TEXT("function_reference"), Reference);
			}
			TArray<TSharedPtr<FJsonValue>> Pins;
			const TArray<TSharedPtr<FJsonValue>>* SourcePins = ArrayField(Source, TEXT("pins"));
			if (!SourcePins) return nullptr;
			for (const TSharedPtr<FJsonValue>& PinValue : *SourcePins)
			{
				const TSharedPtr<FJsonObject> SourcePin = PinValue->AsObject();
				if (!SourcePin.IsValid()) return nullptr;
				TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
				const FString Type = SourcePin->GetStringField(TEXT("type"));
				const FString Classification = SourcePin->GetStringField(TEXT("classification"));
				Pin->SetStringField(TEXT("pin_id"), SourcePin->GetStringField(TEXT("id")));
				Pin->SetStringField(TEXT("role"), Classification);
				Pin->SetStringField(TEXT("name"), SourcePin->GetStringField(TEXT("name")));
				Pin->SetStringField(TEXT("direction"), SourcePin->GetStringField(TEXT("direction")));
				TSharedPtr<FJsonObject> TypeJson = MakeShared<FJsonObject>();
				TypeJson->SetStringField(TEXT("category"), Type);
				const FString Container = SourcePin->GetStringField(TEXT("containerType"));
				TypeJson->SetStringField(TEXT("container"), Container == TEXT("none") ? TEXT("scalar") : Container);
				TypeJson->SetBoolField(TEXT("is_reference"), false);
				TypeJson->SetBoolField(TEXT("is_const"), false);
				Pin->SetObjectField(TEXT("type"), TypeJson);
				Pin->SetField(TEXT("default_value"), SemanticDefault(Type, Classification,
					SourcePin->GetStringField(TEXT("defaultValue"))));
				Pins.Add(MakeShared<FJsonValueObject>(Pin));
			}
			Pins.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("pin_id")) < B->AsObject()->GetStringField(TEXT("pin_id"));
			});
			Node->SetArrayField(TEXT("pins"), Pins);
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
		}
		Nodes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("node_id")) < B->AsObject()->GetStringField(TEXT("node_id"));
		});
		Result->SetArrayField(TEXT("nodes"), Nodes);

		TArray<TSharedPtr<FJsonValue>> Connections;
		const TArray<TSharedPtr<FJsonValue>>* SourceConnections = ArrayField(Serialized.Graph, TEXT("connections"));
		if (!SourceConnections) return nullptr;
		for (const TSharedPtr<FJsonValue>& SourceValue : *SourceConnections)
		{
			const TSharedPtr<FJsonObject> Source = SourceValue->AsObject();
			TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
			Connection->SetStringField(TEXT("from_node_id"), Source->GetStringField(TEXT("sourceNodeId")));
			Connection->SetStringField(TEXT("from_pin_id"), Source->GetStringField(TEXT("sourcePinId")));
			Connection->SetStringField(TEXT("to_node_id"), Source->GetStringField(TEXT("targetNodeId")));
			Connection->SetStringField(TEXT("to_pin_id"), Source->GetStringField(TEXT("targetPinId")));
			Connection->SetStringField(TEXT("classification"), Source->GetStringField(TEXT("classification")));
			Connections.Add(MakeShared<FJsonValueObject>(Connection));
		}
		Connections.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return CanonicalJsonObject(A->AsObject()) < CanonicalJsonObject(B->AsObject());
		});
		Result->SetArrayField(TEXT("connections"), Connections);
		return Result;
	}

	UEdGraphNode* ResolveNode(UEdGraph* Graph, const FString& Guid)
	{
		UEdGraphNode* Match = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || NormalizeGuid(Node->NodeGuid.ToString(EGuidFormats::Digits)) != NormalizeGuid(Guid)) continue;
			if (Match) return nullptr;
			Match = Node;
		}
		return Match;
	}

	UEdGraphPin* ResolvePin(UEdGraphNode* Node, const FString& PinId, const FString& PinName)
	{
		UEdGraphPin* Match = nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input
				|| NormalizeGuid(Pin->PinId.ToString(EGuidFormats::Digits)) != NormalizeGuid(PinId)
				|| Pin->PinName.ToString() != PinName) continue;
			if (Match) return nullptr;
			Match = Pin;
		}
		return Match;
	}

	TSharedPtr<FJsonObject> FindSemanticPin(
		const TSharedPtr<FJsonObject>& Topology,
		const FString& NodeGuid,
		const FString& PinId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = ArrayField(Topology, TEXT("nodes"));
		if (!Nodes) return nullptr;
		for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			if (!Node.IsValid() || NormalizeGuid(Node->GetStringField(TEXT("existing_node_guid"))) != NormalizeGuid(NodeGuid)) continue;
			const TArray<TSharedPtr<FJsonValue>>* Pins = ArrayField(Node, TEXT("pins"));
			if (!Pins) return nullptr;
			for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
			{
				const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
				if (Pin.IsValid() && NormalizeGuid(Pin->GetStringField(TEXT("pin_id"))) == NormalizeGuid(PinId)) return Pin;
			}
		}
		return nullptr;
	}

	bool CompileWithoutSave(UBlueprint* Blueprint, TSharedPtr<FJsonObject>& Evidence)
	{
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave, &Log);
		const bool bSuccess = Log.NumErrors == 0 && Blueprint->Status != EBlueprintStatus::BS_Error;
		Evidence = Attempt(true, bSuccess);
		Evidence->SetNumberField(TEXT("errors"), Log.NumErrors);
		Evidence->SetNumberField(TEXT("warnings"), Log.NumWarnings);
		Evidence->SetBoolField(TEXT("save_requested"), false);
		return bSuccess;
	}

	FString FencingFile(const FString& TargetIdentity)
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SpaceheadBuilder"), TEXT("Fencing"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		return FPaths::Combine(Directory, Sha256(TargetIdentity) + TEXT(".txt"));
	}

	bool AcceptFencing(
		const FString& TargetIdentity,
		const FString& TransactionId,
		const FString& Token,
		int64 Sequence)
	{
		if (TargetIdentity.IsEmpty() || TransactionId.IsEmpty() || Token.IsEmpty() || Sequence < 1) return false;
		const FString Path = FencingFile(TargetIdentity);
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *Path))
		{
			TArray<FString> Fields;
			Existing.ParseIntoArray(Fields, TEXT("|"), false);
			if (Fields.Num() != 3 || !Fields[0].IsNumeric()) return false;
			const int64 ExistingSequence = FCString::Atoi64(*Fields[0]);
			if (Sequence <= ExistingSequence) return false;
		}
		const FString Temporary = Path + TEXT(".tmp");
		const FString Value = FString::Printf(TEXT("%lld|%s|%s"), Sequence, *Token, *TransactionId);
		const FTCHARToUTF8 Utf8(*Value);
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Temporary, false, false));
		if (!Handle.IsValid()
			|| !Handle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length())
			|| !Handle->Flush(true))
		{
			Handle.Reset();
			IFileManager::Get().Delete(*Temporary, false, true, true);
			return false;
		}
		Handle.Reset();
		return IFileManager::Get().Move(*Path, *Temporary, true, true, false, true);
	}

	TSharedPtr<FJsonObject> Readiness()
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("contract_version"), ContractVersion);
		Result->SetBoolField(TEXT("ready"), true);
		Result->SetStringField(TEXT("bridge_identity"), EndpointIdentity);
		Result->SetStringField(TEXT("environment_digest"), Sha256(
			EngineIdentity() + TEXT("|") + UeMcpVersion + TEXT("|") + BridgeVersion + TEXT("|") + PluginBuildIdentity));
		Result->SetStringField(TEXT("observed_at"), FDateTime::UtcNow().ToIso8601());
		Result->SetStringField(TEXT("raw_method_identity"), RawMethodIdentity);
		return Result;
	}

	TSharedPtr<FJsonObject> Discovery()
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("discovery_version"), TEXT("spacehead.capability-discovery@1.0"));
		Result->SetStringField(TEXT("ue_version"), EngineIdentity());
		Result->SetStringField(TEXT("ue_mcp_version"), UeMcpVersion);
		Result->SetStringField(TEXT("bridge_version"), BridgeVersion);
		Result->SetStringField(TEXT("plugin_build_identity"), PluginBuildIdentity);
		Result->SetStringField(TEXT("discovered_at"), FDateTime::UtcNow().ToIso8601());
		Result->SetBoolField(TEXT("qualified"), false);
		TSharedPtr<FJsonObject> Capability = MakeShared<FJsonObject>();
		Capability->SetStringField(TEXT("semantic_capability_id"), SemanticCapability);
		Capability->SetStringField(TEXT("capability_version"), CapabilityVersion);
		Capability->SetStringField(TEXT("toolset_identity"), TEXT("blueprint"));
		Capability->SetStringField(TEXT("toolset_version"), UeMcpVersion);
		Capability->SetStringField(TEXT("tool_identity"), TEXT("blueprint"));
		Capability->SetStringField(TEXT("action_identity"), RawMethodIdentity);
		Capability->SetStringField(TEXT("raw_method_identity"), RawMethodIdentity);
		Capability->SetStringField(TEXT("input_schema_digest"), Sha256(InputSchemaIdentity));
		Capability->SetStringField(TEXT("output_schema_digest"), Sha256(OutputSchemaIdentity));
		Capability->SetBoolField(TEXT("enabled"), true);
		Capability->SetBoolField(TEXT("available"), true);
		Result->SetArrayField(TEXT("capabilities"), { MakeShared<FJsonValueObject>(Capability) });
		return Result;
	}

	TSharedPtr<FJsonObject> CanonicalizeFixtureTopology(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Result = CloneObject(Source);
		if (!Result.IsValid() || Result->GetStringField(TEXT("topology_version")) != TopologyVersion) return nullptr;
		Result->RemoveField(TEXT("layout"));
		const TArray<TSharedPtr<FJsonValue>>* SourceNodes = ArrayField(Result, TEXT("nodes"));
		const TArray<TSharedPtr<FJsonValue>>* SourceConnections = ArrayField(Result, TEXT("connections"));
		if (!SourceNodes || !SourceConnections) return nullptr;
		TArray<TSharedPtr<FJsonValue>> Nodes = *SourceNodes;
		for (const TSharedPtr<FJsonValue>& NodeValue : Nodes)
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* SourcePins = ArrayField(Node, TEXT("pins"));
			if (!Node.IsValid() || !SourcePins) return nullptr;
			TArray<TSharedPtr<FJsonValue>> Pins = *SourcePins;
			Pins.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("pin_id")) < B->AsObject()->GetStringField(TEXT("pin_id"));
			});
			Node->SetArrayField(TEXT("pins"), Pins);
		}
		Nodes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("node_id")) < B->AsObject()->GetStringField(TEXT("node_id"));
		});
		TArray<TSharedPtr<FJsonValue>> Connections = *SourceConnections;
		Connections.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject> Left = A->AsObject();
			const TSharedPtr<FJsonObject> Right = B->AsObject();
			const FString LeftKey = Left->GetStringField(TEXT("from_node_id")) + TEXT("|")
				+ Left->GetStringField(TEXT("from_pin_id")) + TEXT("|")
				+ Left->GetStringField(TEXT("to_node_id")) + TEXT("|")
				+ Left->GetStringField(TEXT("to_pin_id")) + TEXT("|")
				+ Left->GetStringField(TEXT("classification"));
			const FString RightKey = Right->GetStringField(TEXT("from_node_id")) + TEXT("|")
				+ Right->GetStringField(TEXT("from_pin_id")) + TEXT("|")
				+ Right->GetStringField(TEXT("to_node_id")) + TEXT("|")
				+ Right->GetStringField(TEXT("to_pin_id")) + TEXT("|")
				+ Right->GetStringField(TEXT("classification"));
			return LeftKey < RightKey;
		});
		Result->SetArrayField(TEXT("nodes"), Nodes);
		Result->SetArrayField(TEXT("connections"), Connections);
		return Result;
	}
}

TSharedPtr<FJsonValue> FBlueprintHandlers::ApplyAtomicBuildPlan(const TSharedPtr<FJsonObject>& Params)
{
	FString Contract;
	FString RequestKind;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("contract_version"), Contract)
		|| Contract != ContractVersion
		|| !Params->TryGetStringField(TEXT("request_kind"), RequestKind))
		return MCPError(TEXT("INVALID_ATOMIC_BRIDGE_CONTRACT"));
	if (RequestKind == TEXT("readiness")) return MCPResult(Readiness());
	if (RequestKind == TEXT("discover")) return MCPResult(Discovery());
	if (RequestKind == TEXT("canonical_vectors"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Vectors = ArrayField(Params, TEXT("vectors"));
		if (!Vectors || Vectors->Num() < 1 || Vectors->Num() > 16) return MCPError(TEXT("INVALID_CANONICAL_VECTOR_SET"));
		TArray<TSharedPtr<FJsonValue>> Results;
		for (const TSharedPtr<FJsonValue>& Value : *Vectors)
		{
			const TSharedPtr<FJsonObject> Canonical = CanonicalizeFixtureTopology(Value->AsObject());
			if (!Canonical.IsValid()) return MCPError(TEXT("INVALID_CANONICAL_TOPOLOGY_VECTOR"));
			const FString Text = CanonicalJsonObject(Canonical);
			TSharedPtr<FJsonObject> VectorResult = MakeShared<FJsonObject>();
			VectorResult->SetStringField(TEXT("canonical"), Text);
			VectorResult->SetStringField(TEXT("sha256"), Sha256(Text));
			Results.Add(MakeShared<FJsonValueObject>(VectorResult));
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("topology_version"), TopologyVersion);
		Result->SetArrayField(TEXT("vectors"), Results);
		return MCPResult(Result);
	}

	FString TransactionId;
	FString CorrelationId;
	if (!Params->TryGetStringField(TEXT("transaction_id"), TransactionId) || TransactionId.IsEmpty()
		|| !Params->TryGetStringField(TEXT("correlation_id"), CorrelationId) || CorrelationId.IsEmpty())
		return MCPError(TEXT("INVALID_ATOMIC_CORRELATION"));
	const FString CacheKey = TransactionId + TEXT("|") + CorrelationId;
	if (RequestKind == TEXT("status"))
	{
		if (const FCachedAtomicExecution* Cached = CorrelatedExecutions.Find(CacheKey)) return MCPResult(Cached->Receipt);
		TSharedPtr<FJsonObject> Unknown = BaseReceipt(TransactionId, CorrelationId, TEXT("UNKNOWN"));
		Unknown->SetBoolField(TEXT("received_by_bridge"), false);
		SetFailure(Unknown, TEXT("TRANSPORT_UNKNOWN"), TEXT("CORRELATED_STATUS_NOT_FOUND"), TEXT("UNKNOWN"),
			TEXT("The bridge has no process-memory receipt for this correlation"));
		return MCPResult(Unknown);
	}
	if (RequestKind != TEXT("dispatch")) return MCPError(TEXT("INVALID_ATOMIC_REQUEST_KIND"));

	const TSharedPtr<FJsonObject> BuildSpec = ObjectField(Params, TEXT("build_spec"));
	const TSharedPtr<FJsonObject> Plan = ObjectField(Params, TEXT("plan"));
	const TSharedPtr<FJsonObject> Fencing = ObjectField(Params, TEXT("fencing"));
	TSharedPtr<FJsonObject> Receipt = BaseReceipt(TransactionId, CorrelationId, TEXT("FAILED_PRE_MUTATION"));
	if (!BuildSpec.IsValid() || !Plan.IsValid() || !Fencing.IsValid())
	{
		SetFailure(Receipt, TEXT("SPEC"), TEXT("INVALID_ATOMIC_PAYLOAD"), TEXT("FAILED_PRE_MUTATION"), TEXT("BuildSpec, BuildPlan, and fencing are required"));
		return MCPResult(Receipt);
	}
	FString PlanHash;
	FString SpecHash;
	if (!Plan->TryGetStringField(TEXT("build_plan_canonical_hash"), PlanHash) || !ValidHash(PlanHash)
		|| !Plan->TryGetStringField(TEXT("build_spec_canonical_hash"), SpecHash) || !ValidHash(SpecHash)
		|| Sha256(CanonicalJsonObject(WithoutField(Plan, TEXT("build_plan_canonical_hash")))) != PlanHash
		|| Sha256(CanonicalJsonObject(BuildSpec)) != SpecHash)
	{
		SetFailure(Receipt, TEXT("SPEC"), TEXT("CANONICAL_HASH_MISMATCH"), TEXT("FAILED_PRE_MUTATION"), TEXT("Canonical BuildSpec or BuildPlan hash verification failed"));
		return MCPResult(Receipt);
	}
	if (const FCachedAtomicExecution* Cached = CorrelatedExecutions.Find(CacheKey))
	{
		if (Cached->PlanHash != PlanHash) return MCPError(TEXT("ATOMIC_CORRELATION_CONFLICT"));
		return MCPResult(Cached->Receipt);
	}
	CorrelatedExecutions.Add(CacheKey, { CorrelationId, PlanHash, Receipt });

	const TSharedPtr<FJsonObject> Target = ObjectField(Plan, TEXT("target"));
	const TSharedPtr<FJsonObject> TestHooks = ObjectField(Params, TEXT("test_hooks"));
	const TArray<TSharedPtr<FJsonValue>>* Selectors = ArrayField(Target, TEXT("graph_selectors"));
	const TArray<TSharedPtr<FJsonValue>>* Operations = ArrayField(Plan, TEXT("operations"));
	const TSharedPtr<FJsonObject> Operation = Operations && Operations->Num() == 1 ? (*Operations)[0]->AsObject() : nullptr;
	const TSharedPtr<FJsonObject> Payload = ObjectField(Operation, TEXT("semantic_payload"));
	FString PackagePath;
	FString BlueprintName;
	FString Capability;
	FString Version;
	FString SelectorKind;
	FString SelectorName;
	FString NodeGuid;
	FString PinId;
	FString PinName;
	FString OperationId;
	bool bExpected = false;
	bool bDesired = false;
	if (!Target.IsValid() || !Selectors || Selectors->Num() != 1 || !Operation.IsValid() || !Payload.IsValid()
		|| !Target->TryGetStringField(TEXT("package_path"), PackagePath)
		|| !Target->TryGetStringField(TEXT("blueprint_name"), BlueprintName)
		|| !PackagePath.StartsWith(TEXT("/Game/Tests/Builder/"))
		|| PackagePath != TEXT("/Game/Tests/Builder/") + BlueprintName
		|| !Operation->TryGetStringField(TEXT("semantic_capability_id"), Capability) || Capability != SemanticCapability
		|| !Operation->TryGetStringField(TEXT("operation_version"), Version) || Version != OperationVersion
		|| !(*Selectors)[0]->AsObject()->TryGetStringField(TEXT("kind"), SelectorKind)
		|| !(*Selectors)[0]->AsObject()->TryGetStringField(TEXT("name"), SelectorName)
		|| !Payload->TryGetStringField(TEXT("node_guid"), NodeGuid)
		|| !Payload->TryGetStringField(TEXT("pin_id"), PinId)
		|| !Payload->TryGetStringField(TEXT("pin_name"), PinName)
		|| !Operation->TryGetStringField(TEXT("operation_id"), OperationId) || OperationId.IsEmpty()
		|| !Payload->TryGetBoolField(TEXT("expected_current_value"), bExpected)
		|| !Payload->TryGetBoolField(TEXT("desired_value"), bDesired)
		|| bExpected == bDesired)
	{
		SetFailure(Receipt, TEXT("SPEC"), TEXT("UNQUALIFIED_ATOMIC_PLAN"), TEXT("FAILED_PRE_MUTATION"), TEXT("Only one disposable bool set_pin_default plan is executable"));
		return MCPResult(Receipt);
	}
	const TSharedPtr<FJsonObject> SpecTarget = ObjectField(BuildSpec, TEXT("target"));
	const TArray<TSharedPtr<FJsonValue>>* SpecOperations = ArrayField(BuildSpec, TEXT("operations"));
	const TSharedPtr<FJsonObject> SpecOperation = SpecOperations && SpecOperations->Num() == 1
		? (*SpecOperations)[0]->AsObject() : nullptr;
	const TSharedPtr<FJsonObject> SpecPayload = ObjectField(SpecOperation, TEXT("payload"));
	const TSharedPtr<FJsonObject> SpecPin = ObjectField(SpecPayload, TEXT("pin"));
	const TSharedPtr<FJsonObject> SpecNode = ObjectField(SpecPin, TEXT("node"));
	const TSharedPtr<FJsonObject> SpecPolicy = ObjectField(BuildSpec, TEXT("policy"));
	FString SpecVersion;
	FString SpecRequestId;
	FString SpecOperationId;
	FString SpecFamily;
	FString SpecKind;
	FString SpecOperationVersion;
	FString SpecNodeKind;
	FString SpecNodeGuid;
	FString SpecPinName;
	FString SpecPinDirection;
	FString Atomicity;
	bool bSpecDesired = false;
	bool bCompileWithoutSave = false;
	bool bSaveAfterVerification = false;
	bool bRollbackOnFailure = false;
	if (!SpecTarget.IsValid() || CanonicalJsonObject(SpecTarget) != CanonicalJsonObject(Target)
		|| !SpecOperation.IsValid() || !SpecPayload.IsValid() || !SpecPin.IsValid() || !SpecNode.IsValid() || !SpecPolicy.IsValid()
		|| !BuildSpec->TryGetStringField(TEXT("spec_version"), SpecVersion)
		|| SpecVersion != TEXT("spacehead.blueprint-build-spec@1.0")
		|| !BuildSpec->TryGetStringField(TEXT("request_id"), SpecRequestId) || SpecRequestId.IsEmpty()
		|| !SpecOperation->TryGetStringField(TEXT("operation_id"), SpecOperationId) || SpecOperationId != OperationId
		|| !SpecOperation->TryGetStringField(TEXT("family"), SpecFamily) || SpecFamily != TEXT("graph")
		|| !SpecOperation->TryGetStringField(TEXT("kind"), SpecKind) || SpecKind != TEXT("set_pin_default")
		|| !SpecOperation->TryGetStringField(TEXT("operation_version"), SpecOperationVersion) || SpecOperationVersion != OperationVersion
		|| !SpecNode->TryGetStringField(TEXT("kind"), SpecNodeKind) || SpecNodeKind != TEXT("existing")
		|| !SpecNode->TryGetStringField(TEXT("node_guid"), SpecNodeGuid) || NormalizeGuid(SpecNodeGuid) != NormalizeGuid(NodeGuid)
		|| !SpecPin->TryGetStringField(TEXT("pin_name"), SpecPinName) || SpecPinName != PinName
		|| !SpecPin->TryGetStringField(TEXT("direction"), SpecPinDirection) || SpecPinDirection != TEXT("input")
		|| !SpecPayload->TryGetBoolField(TEXT("value"), bSpecDesired) || bSpecDesired != bDesired
		|| !SpecPolicy->TryGetStringField(TEXT("atomicity"), Atomicity) || Atomicity != TEXT("ONE_BLUEPRINT_PACKAGE")
		|| !SpecPolicy->TryGetBoolField(TEXT("compile_without_save"), bCompileWithoutSave) || !bCompileWithoutSave
		|| !SpecPolicy->TryGetBoolField(TEXT("save_after_verification_only"), bSaveAfterVerification) || !bSaveAfterVerification
		|| !SpecPolicy->TryGetBoolField(TEXT("rollback_on_failure"), bRollbackOnFailure) || !bRollbackOnFailure)
	{
		SetFailure(Receipt, TEXT("SPEC"), TEXT("BUILD_SPEC_PLAN_MISMATCH"), TEXT("FAILED_PRE_MUTATION"),
			TEXT("Canonical BuildSpec semantics do not exactly match the executable BuildPlan"));
		return MCPResult(Receipt);
	}

	FString TargetIdentity;
	FString FencingToken;
	FString LockOwner;
	double FencingSequenceNumber = 0;
	const FString ExpectedTargetIdentity = PackagePath + TEXT(":") + BlueprintName;
	if (!Fencing->TryGetStringField(TEXT("target_identity"), TargetIdentity)
		|| TargetIdentity != ExpectedTargetIdentity
		|| !Fencing->TryGetStringField(TEXT("fencing_token"), FencingToken)
		|| !Fencing->TryGetStringField(TEXT("lock_owner_request_id"), LockOwner) || LockOwner != SpecRequestId
		|| !Fencing->TryGetNumberField(TEXT("fencing_sequence"), FencingSequenceNumber)
		|| FencingSequenceNumber < 1 || FMath::FloorToDouble(FencingSequenceNumber) != FencingSequenceNumber
		|| !AcceptFencing(TargetIdentity, TransactionId, FencingToken, static_cast<int64>(FencingSequenceNumber)))
	{
		SetFailure(Receipt, TEXT("CONCURRENCY"), TEXT("STALE_OR_INVALID_FENCING"), TEXT("FAILED_PRE_MUTATION"), TEXT("Bridge fencing validation failed closed"));
		return MCPResult(Receipt);
	}

	const TSharedPtr<FJsonObject> BaselineTopology = ObjectField(Payload, TEXT("baseline_semantic_topology"));
	FString BaselineSemanticHash;
	if (!BaselineTopology.IsValid()
		|| !Payload->TryGetStringField(TEXT("baseline_semantic_fingerprint"), BaselineSemanticHash)
		|| !ValidHash(BaselineSemanticHash)
		|| Sha256(CanonicalJsonObject(BaselineTopology)) != BaselineSemanticHash)
	{
		SetFailure(Receipt, TEXT("BASELINE"), TEXT("INVALID_SEMANTIC_BASELINE"), TEXT("FAILED_PRE_MUTATION"), TEXT("Canonical semantic baseline integrity failed"));
		return MCPResult(Receipt);
	}

	UBlueprint* Blueprint = LoadBlueprint(PackagePath);
	UEdGraph* Graph = Blueprint ? ResolveTargetGraph(Blueprint, SelectorKind, SelectorName) : nullptr;
	UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
	const bool bDirtyBefore = Package && Package->IsDirty();
	bool bPreComplete = false;
	TSharedPtr<FJsonObject> PreTopology = Graph ? SemanticTopology(Graph,
		BaselineTopology->GetObjectField(TEXT("graph"))->GetStringField(TEXT("graph_type")), bPreComplete) : nullptr;
	const bool bDirtyAfterPreflight = Package && Package->IsDirty();
	UEdGraphNode* Node = Graph ? ResolveNode(Graph, NodeGuid) : nullptr;
	UEdGraphPin* Pin = Node ? ResolvePin(Node, PinId, PinName) : nullptr;
	const FString ExpectedDefault = bExpected ? TEXT("true") : TEXT("false");
	const FString DesiredDefault = bDesired ? TEXT("true") : TEXT("false");
	const bool bPreflight = Blueprint && Graph && Package && Node && Pin && bPreComplete
		&& !bDirtyBefore && !bDirtyAfterPreflight
		&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
		&& Pin->PinType.ContainerType == EPinContainerType::None
		&& !Pin->bDefaultValueIsReadOnly && Pin->LinkedTo.IsEmpty()
		&& Pin->DefaultValue == ExpectedDefault
		&& CanonicalJsonObject(PreTopology) == CanonicalJsonObject(BaselineTopology);
	TSharedPtr<FJsonObject> Evidence = ObjectField(Receipt, TEXT("evidence"));
	TSharedPtr<FJsonObject> DirtyState = MakeShared<FJsonObject>();
	DirtyState->SetBoolField(TEXT("before"), bDirtyBefore);
	DirtyState->SetBoolField(TEXT("after_preflight"), bDirtyAfterPreflight);
	Evidence->SetObjectField(TEXT("dirty_state"), DirtyState);
	TSharedPtr<FJsonObject> Preflight = Attempt(true, bPreflight);
	Preflight->SetBoolField(TEXT("target_resolved"), Blueprint && Graph && Node && Pin);
	Preflight->SetBoolField(TEXT("semantic_baseline_exact"), bPreflight);
	Preflight->SetStringField(TEXT("handler_version"), HandlerVersion);
	Preflight->SetStringField(TEXT("topology_version"), TopologyVersion);
	Evidence->SetObjectField(TEXT("preflight"), Preflight);
	Evidence->SetStringField(TEXT("pre_semantic_fingerprint"), bPreComplete ? Sha256(CanonicalJsonObject(PreTopology)) : FString());
	if (!bPreflight)
	{
		SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("STALE_PLAN_OR_TARGET_MISMATCH"), TEXT("FAILED_PRE_MUTATION"), TEXT("Live Unreal preflight did not exactly match the BuildPlan"));
		return MCPResult(Receipt);
	}

	const FString OriginalDefault = Pin->DefaultValue;
	TSharedPtr<FJsonObject> ExpectedPost = CloneObject(BaselineTopology);
	TSharedPtr<FJsonObject> ExpectedPin = FindSemanticPin(ExpectedPost, NodeGuid, PinId);
	if (!ExpectedPost.IsValid() || !ExpectedPin.IsValid())
	{
		SetFailure(Receipt, TEXT("INTERNAL_CONTRACT"), TEXT("EXPECTED_DELTA_APPLICATION_FAILED"), TEXT("FAILED_PRE_MUTATION"), TEXT("Expected semantic delta could not be applied"));
		return MCPResult(Receipt);
	}
	ExpectedPin->SetBoolField(TEXT("default_value"), bDesired);

	auto Rollback = [&](const TCHAR* FailurePhase, const FString& FailureCode, bool bPersistedStateUncertain) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> RollbackEvidence = Attempt(true, false);
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema) Schema->TrySetDefaultValue(*Pin, OriginalDefault);
		TSharedPtr<FJsonObject> RollbackCompile;
		const bool bCompileRestored = Schema && Pin->DefaultValue == OriginalDefault
			&& CompileWithoutSave(Blueprint, RollbackCompile);
		bool bRollbackComplete = false;
		const TSharedPtr<FJsonObject> RollbackTopology = SemanticTopology(Graph,
			BaselineTopology->GetObjectField(TEXT("graph"))->GetStringField(TEXT("graph_type")), bRollbackComplete);
		const bool bSemanticRestored = bCompileRestored && bRollbackComplete
			&& CanonicalJsonObject(RollbackTopology) == CanonicalJsonObject(BaselineTopology);
		if (bSemanticRestored && !bPersistedStateUncertain) Package->SetDirtyFlag(false);
		const bool bCleanRestored = !Package->IsDirty();
		const bool bRestored = bSemanticRestored && bCleanRestored && !bPersistedStateUncertain;
		RollbackEvidence->SetBoolField(TEXT("succeeded"), bRestored);
		RollbackEvidence->SetBoolField(TEXT("semantic_restoration_verified"), bSemanticRestored);
		RollbackEvidence->SetBoolField(TEXT("clean_state_restored"), bCleanRestored);
		RollbackEvidence->SetBoolField(TEXT("persisted_state_uncertain"), bPersistedStateUncertain);
		RollbackEvidence->SetObjectField(TEXT("compile"), RollbackCompile.IsValid() ? RollbackCompile : Attempt(false, false));
		Evidence->SetObjectField(TEXT("rollback"), RollbackEvidence);
		DirtyState->SetBoolField(TEXT("final"), Package->IsDirty());
		if (bRestored)
		{
			SetFailure(Receipt, FailurePhase, FailureCode, TEXT("RESTORED"), TEXT("Mutation failed and the explicit before-state was restored and verified"));
		}
		else
		{
			SetFailure(Receipt, TEXT("ROLLBACK"), TEXT("RESTORATION_UNPROVEN"), TEXT("QUARANTINED"), TEXT("Mutation failed and exact restoration could not be proven"));
		}
		return MCPResult(Receipt);
	};

	Blueprint->Modify();
	Node->Modify();
	const UEdGraphSchema* Schema = Graph->GetSchema();
	TSharedPtr<FJsonObject> Mutation = Attempt(true, false);
	Evidence->SetObjectField(TEXT("mutation"), Mutation);
	if (!Schema) return Rollback(TEXT("MUTATION"), TEXT("GRAPH_SCHEMA_UNAVAILABLE"), false);
	Schema->TrySetDefaultValue(*Pin, DesiredDefault);
	const bool bMutated = Pin->DefaultValue == DesiredDefault;
	Mutation->SetBoolField(TEXT("succeeded"), bMutated);
	Mutation->SetNumberField(TEXT("changed_pin_defaults"), bMutated ? 1 : 0);
	if (!bMutated) return Rollback(TEXT("MUTATION"), TEXT("SET_PIN_DEFAULT_FAILED"), false);
	bool bForceFailureAfterMutation = false;
	if (TestHooks.IsValid()) TestHooks->TryGetBoolField(TEXT("force_failure_after_mutation"), bForceFailureAfterMutation);
	if (bForceFailureAfterMutation)
	{
		Mutation->SetBoolField(TEXT("test_failure_injected"), true);
		return Rollback(TEXT("TEST_INJECTION"), TEXT("FORCED_FAILURE_AFTER_MUTATION"), false);
	}

	TSharedPtr<FJsonObject> CompileEvidence;
	const bool bCompiled = CompileWithoutSave(Blueprint, CompileEvidence);
	Evidence->SetObjectField(TEXT("compile"), CompileEvidence);
	if (!bCompiled) return Rollback(TEXT("COMPILE"), TEXT("BLUEPRINT_COMPILE_FAILED"), false);

	bool bPostComplete = false;
	const TSharedPtr<FJsonObject> PostTopology = SemanticTopology(Graph,
		BaselineTopology->GetObjectField(TEXT("graph"))->GetStringField(TEXT("graph_type")), bPostComplete);
	const FString PostFingerprint = bPostComplete ? Sha256(CanonicalJsonObject(PostTopology)) : FString();
	Evidence->SetStringField(TEXT("post_semantic_fingerprint"), PostFingerprint);
	const bool bVerified = bPostComplete && CanonicalJsonObject(PostTopology) == CanonicalJsonObject(ExpectedPost);
	TSharedPtr<FJsonObject> Verification = Attempt(true, bVerified);
	Verification->SetBoolField(TEXT("exact_expected_delta"), bVerified);
	Verification->SetBoolField(TEXT("unchanged_invariants"), bVerified);
	Verification->SetNumberField(TEXT("changed_pin_defaults"), bVerified ? 1 : 0);
	Evidence->SetObjectField(TEXT("verification"), Verification);
	if (!bVerified) return Rollback(TEXT("VERIFY"), TEXT("EXACT_SEMANTIC_DELTA_FAILED"), false);

	TSharedPtr<FJsonObject> SaveEvidence = Attempt(true, false);
	Evidence->SetObjectField(TEXT("save"), SaveEvidence);
	const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
	SaveEvidence->SetBoolField(TEXT("succeeded"), bSaved);
	if (!bSaved) return Rollback(TEXT("SAVE"), TEXT("BLUEPRINT_SAVE_FAILED"), true);

	bool bPersistedComplete = false;
	const TSharedPtr<FJsonObject> PersistedTopology = SemanticTopology(Graph,
		BaselineTopology->GetObjectField(TEXT("graph"))->GetStringField(TEXT("graph_type")), bPersistedComplete);
	const bool bPersisted = bPersistedComplete && !Package->IsDirty()
		&& CanonicalJsonObject(PersistedTopology) == CanonicalJsonObject(ExpectedPost);
	SaveEvidence->SetBoolField(TEXT("persisted_verified"), bPersisted);
	SaveEvidence->SetBoolField(TEXT("package_clean"), !Package->IsDirty());
	DirtyState->SetBoolField(TEXT("final"), Package->IsDirty());
	Evidence->SetObjectField(TEXT("rollback"), Attempt(false, false));
	if (!bPersisted)
	{
		SetFailure(Receipt, TEXT("SAVE"), TEXT("PERSISTED_STATE_UNPROVEN"), TEXT("QUARANTINED"), TEXT("Save returned but persisted clean state could not be proven"));
		return MCPResult(Receipt);
	}
	Receipt->SetStringField(TEXT("state"), TEXT("SUCCESS"));
	return MCPResult(Receipt);
}
