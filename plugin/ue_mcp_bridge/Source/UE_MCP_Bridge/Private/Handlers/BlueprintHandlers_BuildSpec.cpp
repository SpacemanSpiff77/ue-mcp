#include "BlueprintHandlers.h"
#include "BlueprintTopologySerializer.h"
#include "HandlerUtils.h"
#include "AtomicBridgeBuildIdentity.generated.h"

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
#include "Interfaces/IPluginManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_CallFunction.h"
#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	const TCHAR* ContractVersion = TEXT("spacehead.blueprint-atomic-bridge@2.0");
	const TCHAR* EndpointIdentity = TEXT("blueprint.apply-atomic-build-plan@1.0");
	const TCHAR* RawMethodIdentity = TEXT("apply_atomic_build_plan");
	const TCHAR* SemanticCapability = TEXT("graph.set-pin-default");
	const TCHAR* AddNodeCapability = TEXT("graph.add-node");
	const TCHAR* ConnectPinsCapability = TEXT("graph.connect-pins");
	const TCHAR* DisconnectPinsCapability = TEXT("graph.disconnect-pins");
	const TCHAR* CallFunctionCapability = TEXT("graph.add-call-function-standard");
	const TCHAR* CapabilityVersion = TEXT("1.0");
	const TCHAR* OperationVersion = TEXT("graph.set-pin-default@1.0");
	const TCHAR* HandlerVersion = TEXT("spacehead.graph.set-pin-default-handler@1.0");
	const TCHAR* TopologyVersion = TEXT("spacehead.blueprint-semantic-topology@1.0");
	const TCHAR* TopologyV2Version = TEXT("spacehead.blueprint-semantic-topology@2.0");
	const TCHAR* UeMcpVersion = TEXT("1.1.36");
	const TCHAR* BridgeVersion = TEXT("0.3.0");
	const TCHAR* InputSchemaVersion = TEXT("spacehead.blueprint-atomic-bridge.request@2.0");
	const TCHAR* OutputSchemaVersion = TEXT("spacehead.blueprint-atomic-bridge.receipt@2.0");
	const TCHAR* TestHookVersion = TEXT("spacehead.blueprint-atomic-bridge.test-hooks@2.0");
	const TCHAR* DurableStatusVersion = TEXT("spacehead.blueprint-atomic-bridge.durable-status@1.0");

	struct FCachedAtomicExecution
	{
		FString CorrelationId;
		FString PlanHash;
		TSharedPtr<FJsonObject> Receipt;
	};

	TMap<FString, FCachedAtomicExecution> CorrelatedExecutions;

	enum class EDurableExecutionRead
	{
		Missing,
		Valid,
		Corrupt,
	};

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

	bool IsSchemaAnnotation(const FString& Key)
	{
		return Key == TEXT("$comment") || Key == TEXT("$id") || Key == TEXT("$schema")
			|| Key == TEXT("description") || Key == TEXT("examples") || Key == TEXT("title")
			|| Key == TEXT("x-spacehead-schema-version");
	}

	bool IsUnorderedSchemaArray(const FString& Key)
	{
		return Key == TEXT("allOf") || Key == TEXT("anyOf") || Key == TEXT("enum")
			|| Key == TEXT("oneOf") || Key == TEXT("required") || Key == TEXT("type");
	}

	FString CanonicalSchemaJsonValue(const TSharedPtr<FJsonValue>& Value, const FString& ParentKey = FString());

	FString CanonicalSchemaJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid()) return TEXT("null");
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Fields;
		for (const auto& Pair : Object->Values)
		{
			const FString Key(*Pair.Key);
			if (!IsSchemaAnnotation(Key)) Fields.Emplace(Key, Pair.Value);
		}
		Fields.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A,
			const TPair<FString, TSharedPtr<FJsonValue>>& B) { return A.Key < B.Key; });
		FString Result = TEXT("{");
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			if (Index > 0) Result += TEXT(",");
			Result += ScalarJson(MakeShared<FJsonValueString>(Fields[Index].Key));
			Result += TEXT(":");
			Result += CanonicalSchemaJsonValue(Fields[Index].Value, Fields[Index].Key);
		}
		return Result + TEXT("}");
	}

	FString CanonicalSchemaJsonValue(const TSharedPtr<FJsonValue>& Value, const FString& ParentKey)
	{
		if (!Value.IsValid() || Value->IsNull()) return TEXT("null");
		if (Value->Type == EJson::Object) return CanonicalSchemaJsonObject(Value->AsObject());
		if (Value->Type == EJson::Array)
		{
			TArray<FString> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Items.Add(CanonicalSchemaJsonValue(Item));
			}
			if (IsUnorderedSchemaArray(ParentKey)) Items.Sort();
			return TEXT("[") + FString::Join(Items, TEXT(",")) + TEXT("]");
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

	bool AtomicSchemaDigest(const TCHAR* FileName, const TCHAR* ExpectedVersion, FString& Digest)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UE_MCP_Bridge"));
		if (!Plugin.IsValid()) return false;
		const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Contracts"), TEXT("AtomicBlueprint"), FileName);
		FString SchemaText;
		TSharedPtr<FJsonObject> Schema;
		if (!FFileHelper::LoadFileToString(SchemaText, *Path)) return false;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaText);
		if (!FJsonSerializer::Deserialize(Reader, Schema) || !Schema.IsValid()) return false;
		FString Version;
		if (!Schema->TryGetStringField(TEXT("x-spacehead-schema-version"), Version)
			|| Version != ExpectedVersion) return false;
		Digest = Sha256(CanonicalSchemaJsonObject(Schema));
		return ValidHash(Digest);
	}

	bool ExactBridgeBuildIdentityAvailable()
	{
		const FString GitCommit = UE_MCP_AtomicBuildIdentity::GitCommit;
		const FString Fingerprint = UE_MCP_AtomicBuildIdentity::BuildFingerprint;
		if (GitCommit.Len() != 40 || !ValidHash(Fingerprint)) return false;
		for (TCHAR Character : GitCommit) if (!FChar::IsHexDigit(Character)) return false;
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
		Result->SetStringField(TEXT("contract_version"), ContractVersion);
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

	TSharedPtr<FJsonObject> SemanticTopologyV2(UEdGraph* Graph, const FString& GraphType, bool& bComplete)
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

		auto ReferenceV2 = [](const TSharedPtr<FJsonObject>& Source)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("owner"), Source->GetStringField(TEXT("owner")));
			Result->SetStringField(TEXT("name"), Source->GetStringField(TEXT("name")));
			Result->SetStringField(TEXT("guid"), Source->GetStringField(TEXT("memberGuid")));
			Result->SetBoolField(TEXT("self_context"), Source->GetBoolField(TEXT("selfContext")));
			if (Source->HasField(TEXT("authoritativeOwner")))
			{
				Result->SetStringField(TEXT("authoritative_owner"), Source->GetStringField(TEXT("authoritativeOwner")));
				Result->SetStringField(TEXT("declaring_owner"), Source->GetStringField(TEXT("declaringOwner")));
				Result->SetStringField(TEXT("native_member"), Source->GetStringField(TEXT("nativeMember")));
			}
			return Result;
		};
		auto TypeMemberV2 = [](const TSharedPtr<FJsonObject>& Source)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("owner"), Source->GetStringField(TEXT("memberParent")));
			Result->SetStringField(TEXT("name"), Source->GetStringField(TEXT("memberName")));
			Result->SetStringField(TEXT("guid"), Source->GetStringField(TEXT("memberGuid")));
			return Result;
		};
		auto TerminalV2 = [](const TSharedPtr<FJsonObject>& Source)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("category"), Source->GetStringField(TEXT("category")));
			Result->SetStringField(TEXT("subcategory"), Source->GetStringField(TEXT("subCategory")));
			Result->SetStringField(TEXT("subcategory_object"), Source->GetStringField(TEXT("subCategoryObject")));
			Result->SetBoolField(TEXT("is_const"), Source->GetBoolField(TEXT("isConst")));
			Result->SetBoolField(TEXT("is_weak_reference"), Source->GetBoolField(TEXT("isWeakPointer")));
			Result->SetBoolField(TEXT("is_uobject_wrapper"), Source->GetBoolField(TEXT("isUObjectWrapper")));
			return Result;
		};

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("topology_version"), TopologyV2Version);
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
				Reference->SetStringField(TEXT("guid"), Variable->GetStringField(TEXT("memberGuid")));
				Reference->SetBoolField(TEXT("self_context"), Variable->GetBoolField(TEXT("selfContext")));
				Node->SetObjectField(TEXT("member_reference"), Reference);
			}
			if (const TSharedPtr<FJsonObject> Called = ObjectField(Source, TEXT("calledFunction")))
			{
				Node->SetObjectField(TEXT("function_reference"), ReferenceV2(Called));
				Node->SetStringField(TEXT("call_mode"), Called->HasField(TEXT("callMode"))
					? Called->GetStringField(TEXT("callMode"))
					: Called->GetBoolField(TEXT("selfContext")) ? TEXT("self-context") : TEXT("external-context"));
			}

			TArray<TSharedPtr<FJsonValue>> Pins;
			const TArray<TSharedPtr<FJsonValue>>* SourcePins = ArrayField(Source, TEXT("pins"));
			if (!SourcePins) return nullptr;
			for (const TSharedPtr<FJsonValue>& PinValue : *SourcePins)
			{
				const TSharedPtr<FJsonObject> SourcePin = PinValue->AsObject();
				const TSharedPtr<FJsonObject> SourceType = ObjectField(SourcePin, TEXT("typeInfo"));
				const TSharedPtr<FJsonObject> SourceMember = ObjectField(SourceType, TEXT("subCategoryMemberReference"));
				const TSharedPtr<FJsonObject> SourceTerminal = ObjectField(SourceType, TEXT("valueTerminalType"));
				if (!SourcePin.IsValid() || !SourceType.IsValid() || !SourceMember.IsValid() || !SourceTerminal.IsValid())
					return nullptr;
				TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
				Pin->SetStringField(TEXT("pin_id"), SourcePin->GetStringField(TEXT("id")));
				Pin->SetStringField(TEXT("role"), SourcePin->GetStringField(TEXT("classification")));
				Pin->SetStringField(TEXT("name"), SourcePin->GetStringField(TEXT("name")));
				Pin->SetStringField(TEXT("direction"), SourcePin->GetStringField(TEXT("direction")));
				TSharedPtr<FJsonObject> Type = MakeShared<FJsonObject>();
				Type->SetStringField(TEXT("category"), SourceType->GetStringField(TEXT("category")));
				Type->SetStringField(TEXT("subcategory"), SourceType->GetStringField(TEXT("subCategory")));
				Type->SetStringField(TEXT("subcategory_object"), SourceType->GetStringField(TEXT("subCategoryObject")));
				Type->SetObjectField(TEXT("subcategory_member_reference"), TypeMemberV2(SourceMember));
				const FString Container = SourceType->GetStringField(TEXT("containerType"));
				Type->SetStringField(TEXT("container"), Container == TEXT("none") ? TEXT("scalar") : Container);
				Type->SetObjectField(TEXT("value_terminal_type"), TerminalV2(SourceTerminal));
				Type->SetBoolField(TEXT("is_reference"), SourceType->GetBoolField(TEXT("isReference")));
				Type->SetBoolField(TEXT("is_const"), SourceType->GetBoolField(TEXT("isConst")));
				Type->SetBoolField(TEXT("is_weak_reference"), SourceType->GetBoolField(TEXT("isWeakPointer")));
				Type->SetBoolField(TEXT("is_uobject_wrapper"), SourceType->GetBoolField(TEXT("isUObjectWrapper")));
				Type->SetBoolField(TEXT("serialize_as_single_precision_float"),
					SourceType->GetBoolField(TEXT("serializeAsSinglePrecisionFloat")));
				Pin->SetObjectField(TEXT("type"), Type);
				Pin->SetStringField(TEXT("default_value"), SourcePin->GetStringField(TEXT("defaultValue")));
				Pin->SetStringField(TEXT("default_object"), SourcePin->GetStringField(TEXT("defaultObject")));
				Pin->SetStringField(TEXT("default_text"), SourcePin->GetStringField(TEXT("defaultTextValue")));
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
			if (!Source.IsValid()) return nullptr;
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

	UEdGraphPin* ResolvePin(
		UEdGraphNode* Node,
		const FString& PinId,
		const FString& PinName,
		EEdGraphPinDirection Direction = EGPD_Input)
	{
		UEdGraphPin* Match = nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != Direction
				|| NormalizeGuid(Pin->PinId.ToString(EGuidFormats::Digits)) != NormalizeGuid(PinId)
				|| Pin->PinName.ToString() != PinName) continue;
			if (Match) return nullptr;
			Match = Pin;
		}
		return Match;
	}

	FString PropertyDirection(const FProperty* Property)
	{
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm)) return TEXT("return");
		if (Property->HasAnyPropertyFlags(CPF_OutParm))
			return Property->HasAnyPropertyFlags(CPF_ReferenceParm | CPF_ConstParm) ? TEXT("inout") : TEXT("output");
		return TEXT("input");
	}

	FString PinObjectPath(const TWeakObjectPtr<UObject>& Object)
	{
		const UObject* Value = Object.Get();
		return Value ? Value->GetPathName() : FString();
	}

	FString PinContainerName(EPinContainerType Type)
	{
		switch (Type)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set: return TEXT("set");
		case EPinContainerType::Map: return TEXT("map");
		default: return TEXT("scalar");
		}
	}

	bool ExactExpectedPinType(const TSharedPtr<FJsonObject>& Expected, const FEdGraphPinType& Actual)
	{
		if (!Expected.IsValid()) return false;
		FString Category, Subcategory, SubcategoryObject, Container;
		bool bReference = false, bConst = false, bWeak = false, bWrapper = false;
		if (!Expected->TryGetStringField(TEXT("category"), Category)
			|| !Expected->TryGetStringField(TEXT("subcategory"), Subcategory)
			|| !Expected->TryGetStringField(TEXT("subcategory_object"), SubcategoryObject)
			|| !Expected->TryGetStringField(TEXT("container"), Container)
			|| !Expected->TryGetBoolField(TEXT("reference"), bReference)
			|| !Expected->TryGetBoolField(TEXT("const"), bConst)
			|| !Expected->TryGetBoolField(TEXT("weak_reference"), bWeak)
			|| !Expected->TryGetBoolField(TEXT("uobject_wrapper"), bWrapper)) return false;
		if (Category != Actual.PinCategory.ToString().ToLower()
			|| Subcategory != Actual.PinSubCategory.ToString()
			|| SubcategoryObject != PinObjectPath(Actual.PinSubCategoryObject)
			|| Container != PinContainerName(Actual.ContainerType)
			|| bReference != Actual.bIsReference || bConst != Actual.bIsConst
			|| bWeak != Actual.bIsWeakPointer || bWrapper != Actual.bIsUObjectWrapper) return false;
		const TSharedPtr<FJsonObject> Terminal = ObjectField(Expected, TEXT("map_value_type"));
		FString TerminalCategory, TerminalSubcategory, TerminalObject;
		bool bTerminalConst = false, bTerminalWeak = false, bTerminalWrapper = false;
		return Terminal.IsValid()
			&& Terminal->TryGetStringField(TEXT("category"), TerminalCategory)
			&& Terminal->TryGetStringField(TEXT("subcategory"), TerminalSubcategory)
			&& Terminal->TryGetStringField(TEXT("subcategory_object"), TerminalObject)
			&& Terminal->TryGetBoolField(TEXT("const"), bTerminalConst)
			&& Terminal->TryGetBoolField(TEXT("weak_reference"), bTerminalWeak)
			&& Terminal->TryGetBoolField(TEXT("uobject_wrapper"), bTerminalWrapper)
			&& TerminalCategory == Actual.PinValueType.TerminalCategory.ToString()
			&& TerminalSubcategory == Actual.PinValueType.TerminalSubCategory.ToString()
			&& TerminalObject == PinObjectPath(Actual.PinValueType.TerminalSubCategoryObject)
			&& bTerminalConst == Actual.PinValueType.bTerminalIsConst
			&& bTerminalWeak == Actual.PinValueType.bTerminalIsWeakPointer
			&& bTerminalWrapper == Actual.PinValueType.bTerminalIsUObjectWrapper;
	}

	bool ExactFunctionParameters(UFunction* Function, const TArray<TSharedPtr<FJsonValue>>* Expected)
	{
		if (!Function || !Expected) return false;
		TArray<FProperty*> Parameters;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It) Parameters.Add(*It);
		if (Parameters.Num() != Expected->Num()) return false;
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			FProperty* Property = Parameters[Index];
			const TSharedPtr<FJsonObject> Json = (*Expected)[Index]->AsObject();
			const TSharedPtr<FJsonObject> ExpectedType = ObjectField(Json, TEXT("pin_type"));
			double ExpectedIndex = -1;
			FString Name, Direction, PropertyClass;
			bool bConst = false, bReference = false, bOut = false, bReturn = false, bRequired = false, bHasDefault = false;
			FEdGraphPinType PinType;
			const FString DefaultKey = FString::Printf(TEXT("CPP_Default_%s"), *Property->GetName());
			const bool bLiveDefault = Function->HasMetaData(*DefaultKey);
			if (!Json.IsValid() || !Json->TryGetNumberField(TEXT("index"), ExpectedIndex)
				|| static_cast<int32>(ExpectedIndex) != Index
				|| !Json->TryGetStringField(TEXT("name"), Name) || Name != Property->GetName()
				|| !Json->TryGetStringField(TEXT("direction"), Direction) || Direction != PropertyDirection(Property)
				|| !Json->TryGetStringField(TEXT("property_class"), PropertyClass) || PropertyClass != Property->GetClass()->GetName()
				|| !Json->TryGetBoolField(TEXT("const"), bConst) || bConst != Property->HasAnyPropertyFlags(CPF_ConstParm)
				|| !Json->TryGetBoolField(TEXT("reference"), bReference) || bReference != Property->HasAnyPropertyFlags(CPF_ReferenceParm)
				|| !Json->TryGetBoolField(TEXT("out"), bOut) || bOut != Property->HasAnyPropertyFlags(CPF_OutParm)
				|| !Json->TryGetBoolField(TEXT("return"), bReturn) || bReturn != Property->HasAnyPropertyFlags(CPF_ReturnParm)
				|| !Json->TryGetBoolField(TEXT("required"), bRequired) || bRequired != (Direction == TEXT("input") && !bLiveDefault)
				|| !Json->TryGetBoolField(TEXT("has_default"), bHasDefault) || bHasDefault != bLiveDefault
				|| !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType)
				|| !ExactExpectedPinType(ExpectedType, PinType)) return false;
		}
		return true;
	}

	UFunction* ResolveExactCallFunction(const TSharedPtr<FJsonObject>& Payload, FString& FailureCode)
	{
		FailureCode = TEXT("FUNCTION_RESOLUTION_MISMATCH");
		FString CandidateId, SemanticMemberId, AuthoritativeOwner, DeclaringOwner, NativeMember, CallMode;
		FString ExpectedSpawner, ExpectedNode, SignatureDigest, MetadataDigest, BlueprintMemberGuid;
		const TSharedPtr<FJsonObject> Flags = ObjectField(Payload, TEXT("function_flags"));
		const TSharedPtr<FJsonObject> BehavioralMetadata = ObjectField(Payload, TEXT("behavioral_metadata"));
		const TArray<TSharedPtr<FJsonValue>>* ExpectedParameters = ArrayField(Payload, TEXT("expected_parameters"));
		bool bCallable = false, bPure = false, bStatic = false, bConst = false;
		if (!Payload.IsValid()
			|| !Payload->TryGetStringField(TEXT("candidate_id"), CandidateId) || !ValidHash(CandidateId)
			|| !Payload->TryGetStringField(TEXT("semantic_member_id"), SemanticMemberId) || !ValidHash(SemanticMemberId)
			|| !Payload->TryGetStringField(TEXT("authoritative_owner"), AuthoritativeOwner) || AuthoritativeOwner.IsEmpty()
			|| !Payload->TryGetStringField(TEXT("declaring_owner"), DeclaringOwner) || DeclaringOwner.IsEmpty()
			|| !Payload->TryGetStringField(TEXT("native_member"), NativeMember) || NativeMember.IsEmpty()
			|| !Payload->TryGetStringField(TEXT("call_mode"), CallMode) || (CallMode != TEXT("static") && CallMode != TEXT("instance"))
			|| !Payload->TryGetStringField(TEXT("expected_spawner_class"), ExpectedSpawner)
			|| ExpectedSpawner != TEXT("/Script/BlueprintGraph.BlueprintFunctionNodeSpawner")
			|| !Payload->TryGetStringField(TEXT("expected_k2_class"), ExpectedNode)
			|| ExpectedNode != TEXT("/Script/BlueprintGraph.K2Node_CallFunction")
			|| !Payload->TryGetStringField(TEXT("signature_digest"), SignatureDigest) || !ValidHash(SignatureDigest)
			|| !Payload->TryGetStringField(TEXT("metadata_digest"), MetadataDigest) || !ValidHash(MetadataDigest)
			|| !Flags.IsValid() || !BehavioralMetadata.IsValid() || !ExpectedParameters
			|| !Flags->TryGetBoolField(TEXT("blueprint_callable"), bCallable) || !bCallable
			|| !Flags->TryGetBoolField(TEXT("blueprint_pure"), bPure)
			|| !Flags->TryGetBoolField(TEXT("static"), bStatic)
			|| !Flags->TryGetBoolField(TEXT("const"), bConst)
			|| bStatic != (CallMode == TEXT("static"))) return nullptr;
		Payload->TryGetStringField(TEXT("blueprint_member_guid"), BlueprintMemberGuid);
		UClass* OwnerClass = FindObject<UClass>(nullptr, *AuthoritativeOwner);
		if (!OwnerClass) OwnerClass = LoadObject<UClass>(nullptr, *AuthoritativeOwner);
		UFunction* Function = OwnerClass ? OwnerClass->FindFunctionByName(*NativeMember) : nullptr;
		if (!Function || Function->GetName() != NativeMember
			|| !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable)
			|| Function->HasAnyFunctionFlags(FUNC_BlueprintPure) != bPure
			|| Function->HasAnyFunctionFlags(FUNC_Static) != bStatic
			|| Function->HasAnyFunctionFlags(FUNC_Const) != bConst) return nullptr;
		UClass* LiveDeclaring = Function->GetOwnerClass();
		UClass* LiveAuthoritative = LiveDeclaring ? LiveDeclaring->GetAuthoritativeClass() : nullptr;
		if (!LiveAuthoritative || LiveAuthoritative->GetPathName() != AuthoritativeOwner) return nullptr;
		const bool bExactDeclaring = LiveDeclaring && LiveDeclaring->GetPathName() == DeclaringOwner;
		if (!bExactDeclaring)
		{
			FGuid LiveGuid;
			const bool bBlueprintEquivalent = !BlueprintMemberGuid.IsEmpty() && DeclaringOwner.Contains(TEXT(".SKEL_"))
				&& UBlueprint::GetGuidFromClassByFieldName<UFunction>(LiveDeclaring, Function->GetFName(), LiveGuid)
				&& LiveGuid.ToString(EGuidFormats::Digits).Equals(BlueprintMemberGuid, ESearchCase::IgnoreCase);
			if (!bBlueprintEquivalent) return nullptr;
		}
		if (!ExactFunctionParameters(Function, ExpectedParameters))
		{
			FailureCode = TEXT("SIGNATURE_MISMATCH");
			return nullptr;
		}
		static const TArray<FString> DeniedMetadata = {
			TEXT("Latent"), TEXT("WorldContext"), TEXT("DefaultToSelf"), TEXT("AutoCreateRefTerm"),
			TEXT("ExpandEnumAsExecs"), TEXT("ExpandBoolAsExecs"), TEXT("DeterminesOutputType"),
			TEXT("DynamicOutputParam"), TEXT("CustomStructureParam"), TEXT("ArrayParm"), TEXT("SetParam"),
			TEXT("MapParam"), TEXT("CommutativeAssociativeBinaryOperator"), TEXT("CustomThunk"), TEXT("Variadic"),
			TEXT("BlueprintInternalUseOnly"), TEXT("DeprecatedFunction") };
		for (const FString& Key : DeniedMetadata) if (Function->HasMetaData(*Key))
		{
			FailureCode = TEXT("CALLFUNCTION_NOT_ADMITTED");
			return nullptr;
		}
		for (const auto& Pair : BehavioralMetadata->Values)
			if (!Function->HasMetaData(*Pair.Key) || Function->GetMetaData(*Pair.Key) != Pair.Value->AsString())
			{
				FailureCode = TEXT("METADATA_MISMATCH");
				return nullptr;
			}
		return Function;
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

	FString DurableExecutionFile(const FString& TransactionId, const FString& CorrelationId)
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SpaceheadBuilder"), TEXT("Transactions"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		return FPaths::Combine(Directory, Sha256(TransactionId + TEXT("|") + CorrelationId) + TEXT(".json"));
	}

	bool AtomicReplaceUtf8(const FString& Path, const FString& Value)
	{
		const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
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
		if (IFileManager::Get().Move(*Path, *Temporary, true, true, false, true)) return true;
		IFileManager::Get().Delete(*Temporary, false, true, true);
		return false;
	}

	bool PersistDurableExecution(
		const FString& TransactionId,
		const FString& CorrelationId,
		const FString& PlanHash,
		const TSharedPtr<FJsonObject>& Fencing,
		const TSharedPtr<FJsonObject>& Receipt)
	{
		if (!ValidHash(PlanHash) || !Receipt.IsValid()) return false;
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("status_version"), DurableStatusVersion);
		Envelope->SetStringField(TEXT("transaction_id"), TransactionId);
		Envelope->SetStringField(TEXT("correlation_id"), CorrelationId);
		Envelope->SetStringField(TEXT("plan_hash"), PlanHash);
		Envelope->SetStringField(TEXT("bridge_git_commit"), UE_MCP_AtomicBuildIdentity::GitCommit);
		Envelope->SetStringField(TEXT("bridge_build_fingerprint"), UE_MCP_AtomicBuildIdentity::BuildFingerprint);
		Envelope->SetStringField(TEXT("recorded_at"), FDateTime::UtcNow().ToIso8601());
		Envelope->SetObjectField(TEXT("fencing"), Fencing.IsValid() ? CloneObject(Fencing) : MakeShared<FJsonObject>());
		Envelope->SetObjectField(TEXT("receipt"), CloneObject(Receipt));
		Envelope->SetStringField(TEXT("integrity_sha256"), Sha256(CanonicalJsonObject(Envelope)));
		return AtomicReplaceUtf8(DurableExecutionFile(TransactionId, CorrelationId), CompactJson(Envelope) + TEXT("\n"));
	}

	EDurableExecutionRead ReadDurableExecution(
		const FString& TransactionId,
		const FString& CorrelationId,
		FCachedAtomicExecution& Execution)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *DurableExecutionFile(TransactionId, CorrelationId)))
			return EDurableExecutionRead::Missing;
		TSharedPtr<FJsonObject> Envelope;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid()) return EDurableExecutionRead::Corrupt;
		FString Version;
		FString StoredTransaction;
		FString StoredCorrelation;
		FString PlanHash;
		FString GitCommit;
		FString Fingerprint;
		FString Integrity;
		FString ReceiptTransaction;
		FString ReceiptCorrelation;
		const TSharedPtr<FJsonObject> Receipt = ObjectField(Envelope, TEXT("receipt"));
		if (!Envelope->TryGetStringField(TEXT("status_version"), Version) || Version != DurableStatusVersion
			|| !Envelope->TryGetStringField(TEXT("transaction_id"), StoredTransaction) || StoredTransaction != TransactionId
			|| !Envelope->TryGetStringField(TEXT("correlation_id"), StoredCorrelation) || StoredCorrelation != CorrelationId
			|| !Envelope->TryGetStringField(TEXT("plan_hash"), PlanHash) || !ValidHash(PlanHash)
			|| !Envelope->TryGetStringField(TEXT("bridge_git_commit"), GitCommit) || GitCommit != UE_MCP_AtomicBuildIdentity::GitCommit
			|| !Envelope->TryGetStringField(TEXT("bridge_build_fingerprint"), Fingerprint) || Fingerprint != UE_MCP_AtomicBuildIdentity::BuildFingerprint
			|| !Envelope->TryGetStringField(TEXT("integrity_sha256"), Integrity) || !ValidHash(Integrity)
			|| Sha256(CanonicalJsonObject(WithoutField(Envelope, TEXT("integrity_sha256")))) != Integrity
			|| !Receipt.IsValid()
			|| !Receipt->TryGetStringField(TEXT("transaction_id"), ReceiptTransaction) || ReceiptTransaction != TransactionId
			|| !Receipt->TryGetStringField(TEXT("correlation_id"), ReceiptCorrelation) || ReceiptCorrelation != CorrelationId)
			return EDurableExecutionRead::Corrupt;
		Execution = { CorrelationId, PlanHash, Receipt };
		return EDurableExecutionRead::Valid;
	}

	bool IsKnownFaultCheckpoint(const FString& Checkpoint)
	{
		return Checkpoint == TEXT("BEFORE_PREFLIGHT")
			|| Checkpoint == TEXT("AFTER_PREFLIGHT_BEFORE_MUTATION")
			|| Checkpoint == TEXT("AFTER_MUTATION")
			|| Checkpoint == TEXT("AFTER_FIRST_MUTATION")
			|| Checkpoint == TEXT("AFTER_MIDDLE_MUTATION")
			|| Checkpoint == TEXT("AFTER_FINAL_MUTATION")
			|| Checkpoint == TEXT("BEFORE_COMPILE")
			|| Checkpoint == TEXT("AFTER_COMPILE_BEFORE_VERIFY")
			|| Checkpoint == TEXT("VERIFICATION_FAILURE")
			|| Checkpoint == TEXT("AFTER_VERIFY_BEFORE_SAVE")
			|| Checkpoint == TEXT("SAVE_FAILURE")
			|| Checkpoint == TEXT("AFTER_SAVE_BEFORE_FINAL_RECEIPT")
			|| Checkpoint == TEXT("ROLLBACK_FAILURE")
			|| Checkpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT");
	}

	bool ReadTestFaultCheckpoint(const TSharedPtr<FJsonObject>& Hooks, const FString& PackagePath, FString& Checkpoint)
	{
		if (!Hooks.IsValid()) return true;
		FString Version;
		return FString(FApp::GetProjectName()).Equals(TEXT("ue_mcp"), ESearchCase::CaseSensitive)
			&& PackagePath.StartsWith(TEXT("/Game/Tests/Builder/"))
			&& Hooks->Values.Num() == 2
			&& Hooks->TryGetStringField(TEXT("test_hook_version"), Version) && Version == TestHookVersion
			&& Hooks->TryGetStringField(TEXT("checkpoint"), Checkpoint)
			&& IsKnownFaultCheckpoint(Checkpoint);
	}

	TSharedPtr<FJsonObject> Readiness()
	{
		FString InputDigest;
		FString OutputDigest;
		const bool bSchemasAvailable = AtomicSchemaDigest(TEXT("request-v2.schema.json"), InputSchemaVersion, InputDigest)
			&& AtomicSchemaDigest(TEXT("receipt-v2.schema.json"), OutputSchemaVersion, OutputDigest);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("contract_version"), ContractVersion);
		Result->SetBoolField(TEXT("ready"), ExactBridgeBuildIdentityAvailable() && bSchemasAvailable);
		Result->SetStringField(TEXT("bridge_identity"), EndpointIdentity);
		Result->SetStringField(TEXT("environment_digest"), Sha256(
			EngineIdentity() + TEXT("|") + UeMcpVersion + TEXT("|") + BridgeVersion + TEXT("|")
			+ UE_MCP_AtomicBuildIdentity::GitCommit + TEXT("|") + UE_MCP_AtomicBuildIdentity::PluginBuildIdentity
			+ TEXT("|") + InputDigest + TEXT("|") + OutputDigest));
		Result->SetStringField(TEXT("observed_at"), FDateTime::UtcNow().ToIso8601());
		Result->SetStringField(TEXT("raw_method_identity"), RawMethodIdentity);
		return Result;
	}

	TSharedPtr<FJsonObject> Discovery()
	{
		FString InputDigest;
		FString OutputDigest;
		const bool bInputSchemaAvailable = AtomicSchemaDigest(TEXT("request-v2.schema.json"), InputSchemaVersion, InputDigest);
		const bool bOutputSchemaAvailable = AtomicSchemaDigest(TEXT("receipt-v2.schema.json"), OutputSchemaVersion, OutputDigest);
		const bool bBuildIdentityAvailable = ExactBridgeBuildIdentityAvailable();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("discovery_version"), TEXT("spacehead.capability-discovery@1.0"));
		Result->SetStringField(TEXT("ue_version"), EngineIdentity());
		Result->SetStringField(TEXT("ue_mcp_version"), UeMcpVersion);
		Result->SetStringField(TEXT("bridge_version"), BridgeVersion);
		if (!FString(UE_MCP_AtomicBuildIdentity::GitCommit).IsEmpty())
			Result->SetStringField(TEXT("bridge_git_commit"), UE_MCP_AtomicBuildIdentity::GitCommit);
		Result->SetStringField(TEXT("bridge_build_fingerprint"), UE_MCP_AtomicBuildIdentity::BuildFingerprint);
		Result->SetStringField(TEXT("plugin_build_identity"), UE_MCP_AtomicBuildIdentity::PluginBuildIdentity);
		Result->SetStringField(TEXT("discovered_at"), FDateTime::UtcNow().ToIso8601());
		Result->SetBoolField(TEXT("qualified"), false);
		auto CapabilityJson = [&](const TCHAR* SemanticId)
		{
			TSharedPtr<FJsonObject> Capability = MakeShared<FJsonObject>();
			Capability->SetStringField(TEXT("semantic_capability_id"), SemanticId);
			Capability->SetStringField(TEXT("capability_version"), CapabilityVersion);
			Capability->SetStringField(TEXT("toolset_identity"), TEXT("blueprint"));
			Capability->SetStringField(TEXT("toolset_version"), UeMcpVersion);
			Capability->SetStringField(TEXT("tool_identity"), TEXT("blueprint"));
			Capability->SetStringField(TEXT("action_identity"), RawMethodIdentity);
			Capability->SetStringField(TEXT("raw_method_identity"), RawMethodIdentity);
			if (bInputSchemaAvailable)
			{
				Capability->SetStringField(TEXT("input_schema_version"), InputSchemaVersion);
				Capability->SetStringField(TEXT("input_schema_digest"), InputDigest);
			}
			if (bOutputSchemaAvailable)
			{
				Capability->SetStringField(TEXT("output_schema_version"), OutputSchemaVersion);
				Capability->SetStringField(TEXT("output_schema_digest"), OutputDigest);
			}
			Capability->SetBoolField(TEXT("enabled"), true);
			Capability->SetBoolField(TEXT("available"), bBuildIdentityAvailable && bInputSchemaAvailable && bOutputSchemaAvailable);
			return MakeShared<FJsonValueObject>(Capability);
		};
		Result->SetArrayField(TEXT("capabilities"), {
			CapabilityJson(SemanticCapability),
			CapabilityJson(AddNodeCapability),
			CapabilityJson(ConnectPinsCapability),
			CapabilityJson(DisconnectPinsCapability),
			CapabilityJson(CallFunctionCapability),
		});
		return Result;
	}

	TSharedPtr<FJsonObject> CanonicalizeFixtureTopology(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Result = CloneObject(Source);
		if (!Result.IsValid()) return nullptr;
		const FString Version = Result->GetStringField(TEXT("topology_version"));
		if (Version != TopologyVersion && Version != TopologyV2Version) return nullptr;
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
		FString RequestedTopologyVersion;
		for (const TSharedPtr<FJsonValue>& Value : *Vectors)
		{
			const TSharedPtr<FJsonObject> Source = Value->AsObject();
			if (!Source.IsValid()) return MCPError(TEXT("INVALID_CANONICAL_TOPOLOGY_VECTOR"));
			const FString Version = Source->GetStringField(TEXT("topology_version"));
			if (RequestedTopologyVersion.IsEmpty()) RequestedTopologyVersion = Version;
			if (Version != RequestedTopologyVersion) return MCPError(TEXT("MIXED_CANONICAL_TOPOLOGY_VERSIONS"));
			const TSharedPtr<FJsonObject> Canonical = CanonicalizeFixtureTopology(Source);
			if (!Canonical.IsValid()) return MCPError(TEXT("INVALID_CANONICAL_TOPOLOGY_VECTOR"));
			const FString Text = CanonicalJsonObject(Canonical);
			TSharedPtr<FJsonObject> VectorResult = MakeShared<FJsonObject>();
			VectorResult->SetStringField(TEXT("canonical"), Text);
			VectorResult->SetStringField(TEXT("sha256"), Sha256(Text));
			Results.Add(MakeShared<FJsonValueObject>(VectorResult));
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("topology_version"), RequestedTopologyVersion);
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
		FCachedAtomicExecution Durable;
		const EDurableExecutionRead DurableRead = ReadDurableExecution(TransactionId, CorrelationId, Durable);
		if (DurableRead == EDurableExecutionRead::Valid)
		{
			CorrelatedExecutions.Add(CacheKey, Durable);
			return MCPResult(Durable.Receipt);
		}
		TSharedPtr<FJsonObject> Unknown = BaseReceipt(TransactionId, CorrelationId, TEXT("UNKNOWN"));
		Unknown->SetBoolField(TEXT("received_by_bridge"), false);
		SetFailure(Unknown, TEXT("TRANSPORT_UNKNOWN"),
			DurableRead == EDurableExecutionRead::Corrupt ? TEXT("CORRELATED_STATUS_CORRUPT") : TEXT("CORRELATED_STATUS_NOT_FOUND"),
			TEXT("UNKNOWN"), DurableRead == EDurableExecutionRead::Corrupt
				? TEXT("Durable correlated status failed integrity or build-identity verification")
				: TEXT("The bridge has no durable receipt for this correlation"));
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
	FCachedAtomicExecution Durable;
	const EDurableExecutionRead DurableRead = ReadDurableExecution(TransactionId, CorrelationId, Durable);
	if (DurableRead == EDurableExecutionRead::Valid)
	{
		if (Durable.PlanHash != PlanHash) return MCPError(TEXT("ATOMIC_CORRELATION_CONFLICT"));
		CorrelatedExecutions.Add(CacheKey, Durable);
		return MCPResult(Durable.Receipt);
	}
	if (DurableRead == EDurableExecutionRead::Corrupt)
	{
		SetFailure(Receipt, TEXT("INTERNAL_CONTRACT"), TEXT("DURABLE_STATUS_CORRUPT"), TEXT("QUARANTINED"),
			TEXT("A corrupt durable transaction record denies mutation"));
		return MCPResult(Receipt);
	}
	CorrelatedExecutions.Add(CacheKey, { CorrelationId, PlanHash, Receipt });

	const TSharedPtr<FJsonObject> Target = ObjectField(Plan, TEXT("target"));
	const TSharedPtr<FJsonObject> TestHooks = ObjectField(Params, TEXT("test_hooks"));
	auto Complete = [&](TSharedPtr<FJsonObject> CompletedReceipt, bool bSuppressFinalReceipt = false) -> TSharedPtr<FJsonValue>
	{
		if (!PersistDurableExecution(TransactionId, CorrelationId, PlanHash, Fencing, CompletedReceipt))
		{
			SetFailure(CompletedReceipt, TEXT("INTERNAL_CONTRACT"), TEXT("DURABLE_STATUS_WRITE_FAILED"), TEXT("QUARANTINED"),
				TEXT("Atomic outcome could not be durably recorded"));
		}
		CorrelatedExecutions.Add(CacheKey, { CorrelationId, PlanHash, CompletedReceipt });
		return bSuppressFinalReceipt ? MCPError(TEXT("TEST_ONLY_FINAL_RECEIPT_SUPPRESSED")) : MCPResult(CompletedReceipt);
	};
	const TArray<TSharedPtr<FJsonValue>>* Selectors = ArrayField(Target, TEXT("graph_selectors"));
	const TArray<TSharedPtr<FJsonValue>>* Operations = ArrayField(Plan, TEXT("operations"));
	FString FirstOperationVersion;
	if (Operations && Operations->Num() == 1 && (*Operations)[0]->AsObject().IsValid())
		(*Operations)[0]->AsObject()->TryGetStringField(TEXT("operation_version"), FirstOperationVersion);
	if (Operations && (Operations->Num() > 1 || FirstOperationVersion == TEXT("graph.add-call-function-standard@1.0")))
	{
		const TSharedPtr<FJsonObject> SpecTarget = ObjectField(BuildSpec, TEXT("target"));
		const TArray<TSharedPtr<FJsonValue>>* SpecOperations = ArrayField(BuildSpec, TEXT("operations"));
		const TSharedPtr<FJsonObject> SpecPolicy = ObjectField(BuildSpec, TEXT("policy"));
		FString PackagePath, BlueprintName, SelectorKind, SelectorName, SpecVersion, SpecRequestId, Atomicity;
		bool bCompileWithoutSave = false, bSaveAfterVerification = false, bRollbackOnFailure = false;
		if (!Target.IsValid() || !SpecTarget.IsValid() || !Selectors || Selectors->Num() != 1
			|| !SpecOperations || SpecOperations->Num() != Operations->Num() || Operations->Num() > 8
			|| !SpecPolicy.IsValid() || CanonicalJsonObject(SpecTarget) != CanonicalJsonObject(Target)
			|| !Target->TryGetStringField(TEXT("package_path"), PackagePath)
			|| !Target->TryGetStringField(TEXT("blueprint_name"), BlueprintName)
			|| !PackagePath.StartsWith(TEXT("/Game/Tests/Builder/"))
			|| PackagePath != TEXT("/Game/Tests/Builder/") + BlueprintName
			|| !(*Selectors)[0]->AsObject()->TryGetStringField(TEXT("kind"), SelectorKind) || SelectorKind != TEXT("graph")
			|| !(*Selectors)[0]->AsObject()->TryGetStringField(TEXT("name"), SelectorName)
			|| !BuildSpec->TryGetStringField(TEXT("spec_version"), SpecVersion) || SpecVersion != TEXT("spacehead.blueprint-build-spec@1.0")
			|| !BuildSpec->TryGetStringField(TEXT("request_id"), SpecRequestId) || SpecRequestId.IsEmpty()
			|| !SpecPolicy->TryGetStringField(TEXT("atomicity"), Atomicity) || Atomicity != TEXT("ONE_BLUEPRINT_PACKAGE")
			|| !SpecPolicy->TryGetBoolField(TEXT("compile_without_save"), bCompileWithoutSave) || !bCompileWithoutSave
			|| !SpecPolicy->TryGetBoolField(TEXT("save_after_verification_only"), bSaveAfterVerification) || !bSaveAfterVerification
			|| !SpecPolicy->TryGetBoolField(TEXT("rollback_on_failure"), bRollbackOnFailure) || !bRollbackOnFailure)
		{
			SetFailure(Receipt, TEXT("SPEC"), TEXT("BUILD_SPEC_PLAN_MISMATCH"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Canonical Stage C BuildSpec and BuildPlan envelope mismatch"));
			return Complete(Receipt);
		}

		TMap<FString, TSharedPtr<FJsonObject>> SpecById;
		for (const TSharedPtr<FJsonValue>& Value : *SpecOperations)
		{
			const TSharedPtr<FJsonObject> SpecOperation = Value->AsObject();
			FString Id;
			if (!SpecOperation.IsValid() || !SpecOperation->TryGetStringField(TEXT("operation_id"), Id)
				|| Id.IsEmpty() || SpecById.Contains(Id))
			{
				SetFailure(Receipt, TEXT("SPEC"), TEXT("INVALID_OPERATION_IDENTITY"), TEXT("FAILED_PRE_MUTATION"),
					TEXT("Stage C operation identities must be unique and non-empty"));
				return Complete(Receipt);
			}
			SpecById.Add(Id, SpecOperation);
		}

		const TSharedPtr<FJsonObject> FirstPayload = ObjectField((*Operations)[0]->AsObject(), TEXT("semantic_payload"));
		const TSharedPtr<FJsonObject> BaselineTopology = ObjectField(FirstPayload, TEXT("baseline_semantic_topology"));
		FString BaselineSemanticHash;
		if (!BaselineTopology.IsValid() || !FirstPayload->TryGetStringField(TEXT("baseline_semantic_fingerprint"), BaselineSemanticHash)
			|| !ValidHash(BaselineSemanticHash) || Sha256(CanonicalJsonObject(BaselineTopology)) != BaselineSemanticHash)
		{
			SetFailure(Receipt, TEXT("BASELINE"), TEXT("INVALID_SEMANTIC_BASELINE"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Canonical Stage C semantic baseline integrity failed"));
			return Complete(Receipt);
		}
		for (const TSharedPtr<FJsonValue>& Value : *Operations)
		{
			const TSharedPtr<FJsonObject> Candidate = ObjectField(Value->AsObject(), TEXT("semantic_payload"));
			FString Hash;
			if (!Candidate.IsValid() || !Candidate->TryGetStringField(TEXT("baseline_semantic_fingerprint"), Hash)
				|| Hash != BaselineSemanticHash)
			{
				SetFailure(Receipt, TEXT("BASELINE"), TEXT("INCONSISTENT_OPERATION_BASELINE"), TEXT("FAILED_PRE_MUTATION"),
					TEXT("Every Stage C operation must bind the same exact baseline"));
				return Complete(Receipt);
			}
		}

		FString TargetIdentity, FencingToken, LockOwner;
		double FencingSequenceNumber = 0;
		const FString ExpectedTargetIdentity = PackagePath + TEXT(":") + BlueprintName;
		if (!Fencing->TryGetStringField(TEXT("target_identity"), TargetIdentity) || TargetIdentity != ExpectedTargetIdentity
			|| !Fencing->TryGetStringField(TEXT("fencing_token"), FencingToken)
			|| !Fencing->TryGetStringField(TEXT("lock_owner_request_id"), LockOwner) || LockOwner != SpecRequestId
			|| !Fencing->TryGetNumberField(TEXT("fencing_sequence"), FencingSequenceNumber)
			|| FencingSequenceNumber < 1 || FMath::FloorToDouble(FencingSequenceNumber) != FencingSequenceNumber
			|| !AcceptFencing(TargetIdentity, TransactionId, FencingToken, static_cast<int64>(FencingSequenceNumber)))
		{
			SetFailure(Receipt, TEXT("CONCURRENCY"), TEXT("STALE_OR_INVALID_FENCING"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Bridge fencing validation failed closed"));
			return Complete(Receipt);
		}
		FString FaultCheckpoint;
		if (!ReadTestFaultCheckpoint(TestHooks, PackagePath, FaultCheckpoint))
		{
			SetFailure(Receipt, TEXT("SPEC"), TEXT("TEST_HOOKS_NOT_AUTHORIZED"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Fault injection requires the versioned disposable ue_mcp test-project gate"));
			return Complete(Receipt);
		}

		TSharedPtr<FJsonObject> Evidence = ObjectField(Receipt, TEXT("evidence"));
		Evidence->SetNumberField(TEXT("dispatch_count"), 1);
		Evidence->SetNumberField(TEXT("operation_count"), Operations->Num());
		TArray<TSharedPtr<FJsonValue>> OrderedIds;
		for (const TSharedPtr<FJsonValue>& Value : *Operations)
			OrderedIds.Add(MakeShared<FJsonValueString>(Value->AsObject()->GetStringField(TEXT("operation_id"))));
		Evidence->SetArrayField(TEXT("ordered_operation_ids"), OrderedIds);
		if (!FaultCheckpoint.IsEmpty()) Evidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		TSharedPtr<FJsonObject> Received = CloneObject(Receipt);
		Received->SetStringField(TEXT("state"), TEXT("UNKNOWN"));
		SetFailure(Received, TEXT("TRANSPORT_UNKNOWN"), TEXT("EXECUTION_IN_PROGRESS_OR_INTERRUPTED"), TEXT("UNKNOWN"),
			TEXT("The bridge durably received the bounded build but no terminal outcome is yet recorded"));
		if (!PersistDurableExecution(TransactionId, CorrelationId, PlanHash, Fencing, Received))
		{
			SetFailure(Receipt, TEXT("INTERNAL_CONTRACT"), TEXT("DURABLE_RECEIPT_NOT_ESTABLISHED"), TEXT("QUARANTINED"),
				TEXT("Mutation was denied because durable correlated evidence could not be established"));
			return Complete(Receipt);
		}
		if (FaultCheckpoint == TEXT("BEFORE_PREFLIGHT"))
		{
			Evidence->SetObjectField(TEXT("preflight"), Attempt(false, false));
			SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("FORCED_BEFORE_PREFLIGHT"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Test-only failure injected before live preflight"));
			return Complete(Receipt);
		}

		UBlueprint* Blueprint = LoadBlueprint(PackagePath);
		UEdGraph* Graph = Blueprint ? ResolveTargetGraph(Blueprint, SelectorKind, SelectorName) : nullptr;
		UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		const FString GraphType = BaselineTopology->GetObjectField(TEXT("graph"))->GetStringField(TEXT("graph_type"));
		const bool bDirtyBefore = Package && Package->IsDirty();
		bool bPreComplete = false;
		const TSharedPtr<FJsonObject> PreTopology = Graph ? SemanticTopology(Graph, GraphType, bPreComplete) : nullptr;
		bool bPreV2Complete = false;
		const TSharedPtr<FJsonObject> PreTopologyV2 = Graph ? SemanticTopologyV2(Graph, GraphType, bPreV2Complete) : nullptr;
		const bool bPreflightSucceeded = Blueprint && Graph && Package && Cast<UEdGraphSchema_K2>(Schema)
			&& !bDirtyBefore && bPreComplete && bPreV2Complete
			&& CanonicalJsonObject(PreTopology) == CanonicalJsonObject(BaselineTopology);
		TSharedPtr<FJsonObject> DirtyState = MakeShared<FJsonObject>();
		DirtyState->SetBoolField(TEXT("before"), bDirtyBefore);
		DirtyState->SetBoolField(TEXT("after_preflight"), Package && Package->IsDirty());
		Evidence->SetObjectField(TEXT("dirty_state"), DirtyState);
		TSharedPtr<FJsonObject> PreflightEvidence = Attempt(true, bPreflightSucceeded);
		PreflightEvidence->SetBoolField(TEXT("target_resolved"), Blueprint && Graph && Package);
		PreflightEvidence->SetBoolField(TEXT("semantic_baseline_exact"), bPreflightSucceeded);
		PreflightEvidence->SetStringField(TEXT("handler_version"), TEXT("spacehead.graph.multi-operation-handler@1.0"));
		PreflightEvidence->SetStringField(TEXT("topology_version"), TopologyVersion);
		Evidence->SetObjectField(TEXT("preflight"), PreflightEvidence);
		Evidence->SetStringField(TEXT("pre_semantic_fingerprint"), bPreComplete ? Sha256(CanonicalJsonObject(PreTopology)) : FString());
		Evidence->SetStringField(TEXT("semantic_topology_v2_version"), TopologyV2Version);
		Evidence->SetStringField(TEXT("pre_semantic_fingerprint_v2"),
			bPreV2Complete ? Sha256(CanonicalJsonObject(PreTopologyV2)) : FString());
		if (!bPreflightSucceeded)
		{
			SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("STALE_PLAN_OR_TARGET_MISMATCH"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Live Unreal preflight did not exactly match the bounded Stage C baseline"));
			return Complete(Receipt);
		}
		if (FaultCheckpoint == TEXT("AFTER_PREFLIGHT_BEFORE_MUTATION"))
		{
			SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("FORCED_AFTER_PREFLIGHT"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Test-only failure injected after live preflight"));
			return Complete(Receipt);
		}

		TMap<FString, UFunction*> PlannedCallFunctions;
		for (const TSharedPtr<FJsonValue>& Value : *Operations)
		{
			const TSharedPtr<FJsonObject> PlannedOperation = Value->AsObject();
			FString OperationId, Version, Capability;
			if (!PlannedOperation.IsValid()
				|| !PlannedOperation->TryGetStringField(TEXT("operation_id"), OperationId)
				|| !PlannedOperation->TryGetStringField(TEXT("operation_version"), Version)
				|| Version != TEXT("graph.add-call-function-standard@1.0")) continue;
			const TSharedPtr<FJsonObject> Payload = ObjectField(PlannedOperation, TEXT("semantic_payload"));
			const TSharedPtr<FJsonObject> SpecOperation = SpecById.FindRef(OperationId);
			const TSharedPtr<FJsonObject> SpecPayload = ObjectField(SpecOperation, TEXT("payload"));
			const TSharedPtr<FJsonObject> RequestedFunction = ObjectField(SpecPayload, TEXT("function"));
			FString SpecKind, SpecVersionValue, NodeClass, SemanticType, AdmissionState;
			FString CandidateId, AuthoritativeOwner, NativeMember, RequestedCandidate, RequestedOwner, RequestedMember;
			if (RequestedFunction.IsValid())
			{
				RequestedFunction->TryGetStringField(TEXT("candidate_id"), RequestedCandidate);
				RequestedFunction->TryGetStringField(TEXT("owner"), RequestedOwner);
				RequestedFunction->TryGetStringField(TEXT("member"), RequestedMember);
			}
			if (!PlannedOperation->TryGetStringField(TEXT("semantic_capability_id"), Capability)
				|| Capability != CallFunctionCapability || !SpecOperation.IsValid() || !SpecPayload.IsValid()
				|| !SpecOperation->TryGetStringField(TEXT("kind"), SpecKind) || SpecKind != TEXT("add_node")
				|| !SpecOperation->TryGetStringField(TEXT("operation_version"), SpecVersionValue) || SpecVersionValue != Version
				|| !SpecPayload->TryGetStringField(TEXT("node_class"), NodeClass) || NodeClass != TEXT("K2Node_CallFunction")
				|| !SpecPayload->TryGetStringField(TEXT("semantic_type"), SemanticType) || SemanticType != TEXT("call-function-standard")
				|| !Payload.IsValid() || !Payload->TryGetStringField(TEXT("candidate_id"), CandidateId)
				|| !Payload->TryGetStringField(TEXT("authoritative_owner"), AuthoritativeOwner)
				|| !Payload->TryGetStringField(TEXT("native_member"), NativeMember)
				|| !Payload->TryGetStringField(TEXT("admission_state"), AdmissionState)
				|| AdmissionState != TEXT("ADMITTED_FOR_CALLFUNCTION_V1")
				|| (!RequestedCandidate.IsEmpty() && RequestedCandidate != CandidateId)
				|| (RequestedCandidate.IsEmpty() && (RequestedOwner != AuthoritativeOwner || RequestedMember != NativeMember)))
			{
				SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("FUNCTION_RESOLUTION_MISMATCH"), TEXT("FAILED_PRE_MUTATION"),
					TEXT("CallFunction BuildSpec, BuildPlan, and admission identities do not match"));
				return Complete(Receipt);
			}
			FString FunctionFailure;
			UFunction* Function = ResolveExactCallFunction(Payload, FunctionFailure);
			if (!Function)
			{
				SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), FunctionFailure, TEXT("FAILED_PRE_MUTATION"),
					TEXT("The exact live UFunction no longer matches the accepted catalog candidate"));
				return Complete(Receipt);
			}
			PlannedCallFunctions.Add(OperationId, Function);
		}

		enum class EAppliedKind { AddedNode, SetDefault, Connected, Disconnected };
		struct FAppliedOperation
		{
			EAppliedKind Kind;
			FString OperationId;
			UEdGraphNode* Node = nullptr;
			UEdGraphPin* From = nullptr;
			UEdGraphPin* To = nullptr;
			FString PreviousDefault;
		};
		TArray<FAppliedOperation> Applied;
		TMap<FString, UEdGraphNode*> LogicalNodes;
		TArray<TSharedPtr<FJsonValue>> PerOperationResults;
		TSharedPtr<FJsonObject> ExpectedPost = CloneObject(BaselineTopology);
		TSharedPtr<FJsonObject> ExpectedPostV2 = CloneObject(PreTopologyV2);

		auto ResolveReference = [&](const TSharedPtr<FJsonObject>& Reference) -> UEdGraphNode*
		{
			FString Kind, Identity;
			if (!Reference.IsValid() || !Reference->TryGetStringField(TEXT("kind"), Kind)) return nullptr;
			if (Kind == TEXT("existing") && Reference->TryGetStringField(TEXT("guid"), Identity)) return ResolveNode(Graph, Identity);
			if (Kind == TEXT("logical") && Reference->TryGetStringField(TEXT("logical_id"), Identity)) return LogicalNodes.FindRef(Identity);
			return nullptr;
		};
		auto ResolveSemanticPin = [&](UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction,
			const FString& Category, const FString& ExactId) -> UEdGraphPin*
		{
			if (!Node) return nullptr;
			UEdGraphPin* Match = nullptr;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != Direction || Pin->PinName.ToString() != Name
					|| Pin->PinType.ContainerType != EPinContainerType::None) continue;
				const bool bCategory = Pin->PinType.PinCategory.ToString().Equals(Category, ESearchCase::IgnoreCase);
				if (!bCategory || (!ExactId.IsEmpty() && NormalizeGuid(Pin->PinId.ToString(EGuidFormats::Digits)) != NormalizeGuid(ExactId))) continue;
				if (Match) return nullptr;
				Match = Pin;
			}
			return Match;
		};
		auto SemanticNodeId = [&](const TSharedPtr<FJsonObject>& Topology, UEdGraphNode* LiveNode) -> FString
		{
			const TArray<TSharedPtr<FJsonValue>>* Nodes = ArrayField(Topology, TEXT("nodes"));
			const FString Guid = LiveNode ? LiveNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
			if (Nodes) for (const TSharedPtr<FJsonValue>& Value : *Nodes)
			{
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				if (Node.IsValid() && NormalizeGuid(Node->GetStringField(TEXT("existing_node_guid"))) == NormalizeGuid(Guid))
					return Node->GetStringField(TEXT("node_id"));
			}
			return FString();
		};
		auto AppendExpectedNode = [&](TSharedPtr<FJsonObject>& Expected, const TSharedPtr<FJsonObject>& Current,
			const FString& CreatedGuid) -> bool
		{
			const TArray<TSharedPtr<FJsonValue>>* CurrentNodes = ArrayField(Current, TEXT("nodes"));
			const TArray<TSharedPtr<FJsonValue>>* ExpectedNodes = ArrayField(Expected, TEXT("nodes"));
			TSharedPtr<FJsonObject> CreatedSemantic;
			if (CurrentNodes) for (const TSharedPtr<FJsonValue>& NodeValue : *CurrentNodes)
			{
				const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
				if (Node.IsValid() && NormalizeGuid(Node->GetStringField(TEXT("existing_node_guid"))) == NormalizeGuid(CreatedGuid))
					CreatedSemantic = CloneObject(Node);
			}
			if (!CreatedSemantic.IsValid() || !ExpectedNodes) return false;
			TArray<TSharedPtr<FJsonValue>> Updated = *ExpectedNodes;
			Updated.Add(MakeShared<FJsonValueObject>(CreatedSemantic));
			Expected->SetArrayField(TEXT("nodes"), Updated);
			Expected = CanonicalizeFixtureTopology(Expected);
			return Expected.IsValid();
		};

		auto RollbackWholeBuild = [&](const TCHAR* Phase, const FString& Code, const FString& FailingOperation,
			bool bPersistedStateUncertain) -> TSharedPtr<FJsonValue>
		{
			const bool bForceFailure = FaultCheckpoint == TEXT("ROLLBACK_FAILURE");
			bool bActionsRestored = !bForceFailure;
			if (!bForceFailure) for (int32 Index = Applied.Num() - 1; Index >= 0; --Index)
			{
				const FAppliedOperation& Action = Applied[Index];
				if (Action.Kind == EAppliedKind::AddedNode)
				{
					if (!Action.Node || !Graph->Nodes.Contains(Action.Node)) { bActionsRestored = false; continue; }
					Graph->RemoveNode(Action.Node);
				}
				else if (Action.Kind == EAppliedKind::SetDefault)
				{
					if (!Action.To) bActionsRestored = false;
					else { Schema->TrySetDefaultValue(*Action.To, Action.PreviousDefault); if (Action.To->DefaultValue != Action.PreviousDefault) bActionsRestored = false; }
				}
				else if (Action.Kind == EAppliedKind::Connected)
				{
					if (!Action.From || !Action.To) bActionsRestored = false;
					else Schema->BreakSinglePinLink(Action.From, Action.To);
				}
				else
				{
					if (!Action.From || !Action.To || !Schema->TryCreateConnection(Action.From, Action.To)) bActionsRestored = false;
				}
			}
			Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			TSharedPtr<FJsonObject> RollbackCompile;
			const bool bCompileRestored = bActionsRestored && CompileWithoutSave(Blueprint, RollbackCompile);
			bool bRollbackComplete = false;
			const TSharedPtr<FJsonObject> RollbackTopology = SemanticTopology(Graph, GraphType, bRollbackComplete);
			bool bRollbackV2Complete = false;
			const TSharedPtr<FJsonObject> RollbackTopologyV2 = SemanticTopologyV2(Graph, GraphType, bRollbackV2Complete);
			const bool bSemanticRestored = bCompileRestored && bRollbackComplete && bRollbackV2Complete
				&& CanonicalJsonObject(RollbackTopology) == CanonicalJsonObject(BaselineTopology)
				&& CanonicalJsonObject(RollbackTopologyV2) == CanonicalJsonObject(PreTopologyV2);
			if (bSemanticRestored && !bPersistedStateUncertain) Package->SetDirtyFlag(false);
			const bool bClean = !Package->IsDirty();
			const bool bRestored = bSemanticRestored && bClean && !bPersistedStateUncertain;
			TSharedPtr<FJsonObject> RollbackEvidence = Attempt(true, bRestored);
			RollbackEvidence->SetBoolField(TEXT("semantic_restoration_verified"), bSemanticRestored);
			RollbackEvidence->SetBoolField(TEXT("clean_state_restored"), bClean);
			RollbackEvidence->SetBoolField(TEXT("persisted_state_uncertain"), bPersistedStateUncertain);
			RollbackEvidence->SetStringField(TEXT("failing_operation_id"), FailingOperation);
			RollbackEvidence->SetObjectField(TEXT("compile"), RollbackCompile.IsValid() ? RollbackCompile : Attempt(false, false));
			Evidence->SetObjectField(TEXT("rollback"), RollbackEvidence);
			DirtyState->SetBoolField(TEXT("final"), Package->IsDirty());
			Evidence->SetArrayField(TEXT("per_operation_results"), PerOperationResults);
			if (bRestored) SetFailure(Receipt, Phase, Code, TEXT("RESTORED"), TEXT("Entire Stage C build restored to the exact semantic baseline"));
			else SetFailure(Receipt, TEXT("ROLLBACK"), TEXT("RESTORATION_UNPROVEN"), TEXT("QUARANTINED"),
				TEXT("Exact whole-build restoration or persisted-state safety could not be proven"));
			return Complete(Receipt, bRestored && FaultCheckpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT"));
		};

		FString FailingOperation;
		FString FailingType;
		FString FailureCode;
		for (int32 Index = 0; Index < Operations->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Operation = (*Operations)[Index]->AsObject();
			const TSharedPtr<FJsonObject> Payload = ObjectField(Operation, TEXT("semantic_payload"));
			FString OperationId, Version, Capability;
			TSharedPtr<FJsonObject> SpecOperation;
			FString SpecKind, SpecVersionValue;
			bool bSucceeded = false, bMutationOccurred = false;
			if (!Operation.IsValid() || !Payload.IsValid()
				|| !Operation->TryGetStringField(TEXT("operation_id"), OperationId)
				|| !Operation->TryGetStringField(TEXT("operation_version"), Version)
				|| !Operation->TryGetStringField(TEXT("semantic_capability_id"), Capability)
				|| !(SpecOperation = SpecById.FindRef(OperationId)).IsValid()
				|| !SpecOperation->TryGetStringField(TEXT("kind"), SpecKind)
				|| !SpecOperation->TryGetStringField(TEXT("operation_version"), SpecVersionValue) || SpecVersionValue != Version)
			{
				FailingOperation = OperationId; FailingType = Version; FailureCode = TEXT("BUILD_SPEC_PLAN_MISMATCH");
			}
			else if (Version == TEXT("graph.add-node@1.0") && Capability == AddNodeCapability && SpecKind == TEXT("add_node"))
			{
				FString LogicalId, NodeClass, SemanticType, CreatedGuid;
				double X = 0, Y = 0;
				const TSharedPtr<FJsonObject> Position = ObjectField(Payload, TEXT("position"));
				FGuid ParsedGuid;
				if (Position.IsValid() && Payload->TryGetStringField(TEXT("logical_id"), LogicalId)
					&& Payload->TryGetStringField(TEXT("node_class"), NodeClass) && NodeClass == TEXT("K2Node_IfThenElse")
					&& Payload->TryGetStringField(TEXT("semantic_type"), SemanticType) && SemanticType == TEXT("branch")
					&& Payload->TryGetStringField(TEXT("created_node_guid"), CreatedGuid) && FGuid::Parse(CreatedGuid, ParsedGuid)
					&& Position->TryGetNumberField(TEXT("x"), X) && Position->TryGetNumberField(TEXT("y"), Y)
					&& !LogicalNodes.Contains(LogicalId) && !ResolveNode(Graph, CreatedGuid))
				{
					Graph->Modify(); Blueprint->Modify();
					UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
					Branch->NodeGuid = ParsedGuid; Branch->NodePosX = static_cast<int32>(X); Branch->NodePosY = static_cast<int32>(Y);
					Graph->AddNode(Branch, false, false); Branch->AllocateDefaultPins();
					LogicalNodes.Add(LogicalId, Branch);
					Applied.Add({ EAppliedKind::AddedNode, OperationId, Branch });
					bSucceeded = Branch->Pins.Num() == 4
						&& ResolveSemanticPin(Branch, TEXT("execute"), EGPD_Input, TEXT("exec"), FString())
						&& ResolveSemanticPin(Branch, TEXT("Condition"), EGPD_Input, TEXT("bool"), FString())
						&& ResolveSemanticPin(Branch, TEXT("then"), EGPD_Output, TEXT("exec"), FString())
						&& ResolveSemanticPin(Branch, TEXT("else"), EGPD_Output, TEXT("exec"), FString());
					bMutationOccurred = true;
					if (bSucceeded)
					{
						bool bCurrentComplete = false;
						const TSharedPtr<FJsonObject> Current = SemanticTopology(Graph, GraphType, bCurrentComplete);
						bool bCurrentV2Complete = false;
						const TSharedPtr<FJsonObject> CurrentV2 = SemanticTopologyV2(Graph, GraphType, bCurrentV2Complete);
						bSucceeded = bCurrentComplete && bCurrentV2Complete
							&& AppendExpectedNode(ExpectedPost, Current, CreatedGuid)
							&& AppendExpectedNode(ExpectedPostV2, CurrentV2, CreatedGuid);
					}
				}
				if (!bSucceeded) FailureCode = TEXT("ADD_NODE_FAILED");
			}
			else if (Version == TEXT("graph.add-call-function-standard@1.0")
				&& Capability == CallFunctionCapability && SpecKind == TEXT("add_node"))
			{
				FString LogicalId, NodeClass, SemanticType, CreatedGuid, AuthoritativeOwner, DeclaringOwner, NativeMember, CallMode;
				double X = 0, Y = 0;
				const TSharedPtr<FJsonObject> Position = ObjectField(Payload, TEXT("position"));
				const TSharedPtr<FJsonObject> Flags = ObjectField(Payload, TEXT("function_flags"));
				const TArray<TSharedPtr<FJsonValue>>* ExpectedParameters = ArrayField(Payload, TEXT("expected_parameters"));
				bool bPure = false, bStatic = false;
				FGuid ParsedGuid;
				UFunction* Function = PlannedCallFunctions.FindRef(OperationId);
				if (Position.IsValid() && Flags.IsValid() && ExpectedParameters && Function
					&& Payload->TryGetStringField(TEXT("logical_id"), LogicalId)
					&& Payload->TryGetStringField(TEXT("node_class"), NodeClass) && NodeClass == TEXT("K2Node_CallFunction")
					&& Payload->TryGetStringField(TEXT("semantic_type"), SemanticType) && SemanticType == TEXT("call-function-standard")
					&& Payload->TryGetStringField(TEXT("authoritative_owner"), AuthoritativeOwner)
					&& Payload->TryGetStringField(TEXT("declaring_owner"), DeclaringOwner)
					&& Payload->TryGetStringField(TEXT("native_member"), NativeMember)
					&& Payload->TryGetStringField(TEXT("call_mode"), CallMode)
					&& Flags->TryGetBoolField(TEXT("blueprint_pure"), bPure)
					&& Flags->TryGetBoolField(TEXT("static"), bStatic)
					&& Payload->TryGetStringField(TEXT("created_node_guid"), CreatedGuid) && FGuid::Parse(CreatedGuid, ParsedGuid)
					&& Position->TryGetNumberField(TEXT("x"), X) && Position->TryGetNumberField(TEXT("y"), Y)
					&& !LogicalNodes.Contains(LogicalId) && !ResolveNode(Graph, CreatedGuid))
				{
					Graph->Modify(); Blueprint->Modify();
					UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
					Call->NodeGuid = ParsedGuid; Call->NodePosX = static_cast<int32>(X); Call->NodePosY = static_cast<int32>(Y);
					Call->SetFromFunction(Function);
					Graph->AddNode(Call, false, false);
					Call->PostPlacedNewNode();
					Call->AllocateDefaultPins();
					LogicalNodes.Add(LogicalId, Call);
					Applied.Add({ EAppliedKind::AddedNode, OperationId, Call });
					bMutationOccurred = true;
					bSucceeded = Call->GetClass() == UK2Node_CallFunction::StaticClass()
						&& Call->GetTargetFunction() == Function && Call->NodeGuid == ParsedGuid
						&& Call->Pins.Num() == ExpectedParameters->Num() + (bPure ? 0 : 2) + (bStatic ? 0 : 1);
					for (const TSharedPtr<FJsonValue>& ParameterValue : *ExpectedParameters)
					{
						const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
						const TSharedPtr<FJsonObject> ExpectedType = ObjectField(Parameter, TEXT("pin_type"));
						FString Name, Direction;
						UEdGraphPin* Match = nullptr;
						if (!Parameter.IsValid() || !Parameter->TryGetStringField(TEXT("name"), Name)
							|| !Parameter->TryGetStringField(TEXT("direction"), Direction)) { bSucceeded = false; continue; }
						const EEdGraphPinDirection PinDirection = Direction == TEXT("input") || Direction == TEXT("inout")
							? EGPD_Input : EGPD_Output;
						for (UEdGraphPin* Pin : Call->Pins) if (Pin && Pin->PinName.ToString() == Name && Pin->Direction == PinDirection)
						{
							if (Match) { Match = nullptr; break; }
							Match = Pin;
						}
						if (!Match || !ExactExpectedPinType(ExpectedType, Match->PinType)) bSucceeded = false;
					}
					if (!bPure)
					{
						if (!ResolveSemanticPin(Call, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input, TEXT("exec"), FString())
							|| !ResolveSemanticPin(Call, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output, TEXT("exec"), FString()))
							bSucceeded = false;
					}
					if (!bStatic)
					{
						int32 SelfPins = 0;
						for (UEdGraphPin* Pin : Call->Pins) if (Pin && Pin->Direction == EGPD_Input
							&& Pin->PinName == UEdGraphSchema_K2::PN_Self) ++SelfPins;
						if (SelfPins != 1) bSucceeded = false;
					}
					if (bSucceeded)
					{
						bool bCurrentComplete = false, bCurrentV2Complete = false;
						const TSharedPtr<FJsonObject> Current = SemanticTopology(Graph, GraphType, bCurrentComplete);
						const TSharedPtr<FJsonObject> CurrentV2 = SemanticTopologyV2(Graph, GraphType, bCurrentV2Complete);
						const FString NodeId = SemanticNodeId(CurrentV2, Call);
						const TArray<TSharedPtr<FJsonValue>>* NodesV2 = ArrayField(CurrentV2, TEXT("nodes"));
						TSharedPtr<FJsonObject> NodeV2;
						if (NodesV2) for (const TSharedPtr<FJsonValue>& NodeValue : *NodesV2)
							if (NodeValue->AsObject()->GetStringField(TEXT("node_id")) == NodeId) NodeV2 = NodeValue->AsObject();
						const TSharedPtr<FJsonObject> FunctionReference = ObjectField(NodeV2, TEXT("function_reference"));
						bSucceeded = bCurrentComplete && bCurrentV2Complete && NodeV2.IsValid() && FunctionReference.IsValid()
							&& NodeV2->GetStringField(TEXT("node_class")) == TEXT("K2Node_CallFunction")
							&& NodeV2->GetStringField(TEXT("call_mode")) == CallMode
							&& FunctionReference->GetStringField(TEXT("authoritative_owner")) == AuthoritativeOwner
							&& FunctionReference->GetStringField(TEXT("native_member")) == NativeMember
							&& AppendExpectedNode(ExpectedPost, Current, CreatedGuid)
							&& AppendExpectedNode(ExpectedPostV2, CurrentV2, CreatedGuid);
					}
				}
				if (!bSucceeded) FailureCode = TEXT("CALLFUNCTION_CONSTRUCTION_FAILED");
			}
			else if (Version == TEXT("graph.set-pin-default@1.0") && Capability == SemanticCapability && SpecKind == TEXT("set_pin_default"))
			{
				const TSharedPtr<FJsonObject> Reference = ObjectField(Payload, TEXT("node_reference"));
				FString Name, Direction, Category, PinId;
				bool Expected = false, Desired = false;
				Payload->TryGetStringField(TEXT("pin_id"), PinId);
				UEdGraphNode* Node = ResolveReference(Reference);
				UEdGraphPin* Pin = Payload->TryGetStringField(TEXT("pin_name"), Name)
					&& Payload->TryGetStringField(TEXT("pin_direction"), Direction) && Direction == TEXT("input")
					&& Payload->TryGetStringField(TEXT("pin_category"), Category) && Category == TEXT("bool")
					? ResolveSemanticPin(Node, Name, EGPD_Input, Category, PinId) : nullptr;
				if (Pin && Pin->LinkedTo.IsEmpty() && Payload->TryGetBoolField(TEXT("expected_current_value"), Expected)
					&& Payload->TryGetBoolField(TEXT("desired_value"), Desired) && Expected != Desired
					&& Pin->DefaultValue == (Expected ? TEXT("true") : TEXT("false")))
				{
					const FString Previous = Pin->DefaultValue;
					Schema->TrySetDefaultValue(*Pin, Desired ? TEXT("true") : TEXT("false"));
					bSucceeded = Pin->DefaultValue == (Desired ? TEXT("true") : TEXT("false"));
					bMutationOccurred = bSucceeded;
					if (bSucceeded)
					{
						Applied.Add({ EAppliedKind::SetDefault, OperationId, nullptr, nullptr, Pin, Previous });
						const FString NodeGuidValue = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
						const FString PinGuidValue = Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower);
						const TSharedPtr<FJsonObject> ExpectedPin = FindSemanticPin(ExpectedPost, NodeGuidValue, PinGuidValue);
						const TSharedPtr<FJsonObject> ExpectedPinV2 = FindSemanticPin(ExpectedPostV2, NodeGuidValue, PinGuidValue);
						if (!ExpectedPin.IsValid() || !ExpectedPinV2.IsValid()) bSucceeded = false;
						else
						{
							ExpectedPin->SetBoolField(TEXT("default_value"), Desired);
							ExpectedPinV2->SetStringField(TEXT("default_value"), Desired ? TEXT("true") : TEXT("false"));
						}
					}
				}
				if (!bSucceeded) FailureCode = TEXT("SET_PIN_DEFAULT_FAILED");
			}
			else if ((Version == TEXT("graph.connect-pins@1.0") || Version == TEXT("graph.disconnect-pins@1.0"))
				&& ((Version == TEXT("graph.connect-pins@1.0") && Capability == ConnectPinsCapability && SpecKind == TEXT("connect_pins"))
					|| (Version == TEXT("graph.disconnect-pins@1.0") && Capability == DisconnectPinsCapability && SpecKind == TEXT("disconnect_pins"))))
			{
				const TSharedPtr<FJsonObject> From = ObjectField(Payload, TEXT("from"));
				const TSharedPtr<FJsonObject> To = ObjectField(Payload, TEXT("to"));
				const TSharedPtr<FJsonObject> FromRef = ObjectField(From, TEXT("node_reference"));
				const TSharedPtr<FJsonObject> ToRef = ObjectField(To, TEXT("node_reference"));
				FString FromName, ToName, FromCategory, ToCategory, FromId, ToId;
				if (From.IsValid()) From->TryGetStringField(TEXT("pin_id"), FromId);
				if (To.IsValid()) To->TryGetStringField(TEXT("pin_id"), ToId);
				UEdGraphNode* FromNode = ResolveReference(FromRef);
				UEdGraphNode* ToNode = ResolveReference(ToRef);
				UEdGraphPin* FromPin = From.IsValid() && From->TryGetStringField(TEXT("pin_name"), FromName)
					&& From->TryGetStringField(TEXT("category"), FromCategory)
					? ResolveSemanticPin(FromNode, FromName, EGPD_Output, FromCategory, FromId) : nullptr;
				UEdGraphPin* ToPin = To.IsValid() && To->TryGetStringField(TEXT("pin_name"), ToName)
					&& To->TryGetStringField(TEXT("category"), ToCategory)
					? ResolveSemanticPin(ToNode, ToName, EGPD_Input, ToCategory, ToId) : nullptr;
				const bool bConnected = FromPin && ToPin && FromPin->LinkedTo.Contains(ToPin) && ToPin->LinkedTo.Contains(FromPin);
				ECanCreateConnectionResponse CompatibilityResponse = CONNECT_RESPONSE_DISALLOW;
				if (FromPin && ToPin) CompatibilityResponse = Schema->CanCreateConnection(FromPin, ToPin).Response.GetValue();
				if (Version == TEXT("graph.connect-pins@1.0") && FromPin && ToPin && FromNode != ToNode
					&& FromCategory.Equals(ToCategory, ESearchCase::IgnoreCase) && !bConnected
					&& FromPin->LinkedTo.IsEmpty() && ToPin->LinkedTo.IsEmpty()
					&& CompatibilityResponse == CONNECT_RESPONSE_MAKE)
				{
					bSucceeded = Schema->TryCreateConnection(FromPin, ToPin); bMutationOccurred = bSucceeded;
					if (bSucceeded) Applied.Add({ EAppliedKind::Connected, OperationId, nullptr, FromPin, ToPin });
				}
				else if (Version == TEXT("graph.disconnect-pins@1.0") && bConnected)
				{
					Schema->BreakSinglePinLink(FromPin, ToPin); bSucceeded = true; bMutationOccurred = true;
					Applied.Add({ EAppliedKind::Disconnected, OperationId, nullptr, FromPin, ToPin });
				}
				if (!bSucceeded) FailureCode = Version == TEXT("graph.connect-pins@1.0")
					&& FromPin && ToPin && CompatibilityResponse != CONNECT_RESPONSE_MAKE
					? TEXT("UNQUALIFIED_CONVERSION_REQUIRED")
					: Version == TEXT("graph.connect-pins@1.0") ? TEXT("CONNECT_PINS_FAILED") : TEXT("DISCONNECT_PINS_FAILED");
				if (bSucceeded)
				{
					bool bCurrentComplete = false;
					const TSharedPtr<FJsonObject> Current = SemanticTopology(Graph, GraphType, bCurrentComplete);
					const FString FromNodeId = SemanticNodeId(Current, FromNode);
					const FString ToNodeId = SemanticNodeId(Current, ToNode);
					const TSharedPtr<FJsonObject> FromSemanticPin = FindSemanticPin(Current,
						FromNode->NodeGuid.ToString(EGuidFormats::Digits), FromPin->PinId.ToString(EGuidFormats::Digits));
					const TSharedPtr<FJsonObject> ToSemanticPin = FindSemanticPin(Current,
						ToNode->NodeGuid.ToString(EGuidFormats::Digits), ToPin->PinId.ToString(EGuidFormats::Digits));
					const TArray<TSharedPtr<FJsonValue>>* ExpectedConnections = ArrayField(ExpectedPost, TEXT("connections"));
					const TArray<TSharedPtr<FJsonValue>>* ExpectedConnectionsV2 = ArrayField(ExpectedPostV2, TEXT("connections"));
					if (!bCurrentComplete || FromNodeId.IsEmpty() || ToNodeId.IsEmpty()
						|| !FromSemanticPin.IsValid() || !ToSemanticPin.IsValid() || !ExpectedConnections || !ExpectedConnectionsV2) bSucceeded = false;
					else
					{
						TSharedPtr<FJsonObject> ExpectedConnection = MakeShared<FJsonObject>();
						ExpectedConnection->SetStringField(TEXT("from_node_id"), FromNodeId);
						ExpectedConnection->SetStringField(TEXT("from_pin_id"), FromSemanticPin->GetStringField(TEXT("pin_id")));
						ExpectedConnection->SetStringField(TEXT("to_node_id"), ToNodeId);
						ExpectedConnection->SetStringField(TEXT("to_pin_id"), ToSemanticPin->GetStringField(TEXT("pin_id")));
						ExpectedConnection->SetStringField(TEXT("classification"),
							FromCategory == TEXT("exec") ? TEXT("execution") : TEXT("data"));
						TArray<TSharedPtr<FJsonValue>> Updated = *ExpectedConnections;
						TArray<TSharedPtr<FJsonValue>> UpdatedV2 = *ExpectedConnectionsV2;
						if (Version == TEXT("graph.connect-pins@1.0")) Updated.Add(MakeShared<FJsonValueObject>(ExpectedConnection));
						else Updated.RemoveAll([&](const TSharedPtr<FJsonValue>& Value)
							{ return CanonicalJsonObject(Value->AsObject()) == CanonicalJsonObject(ExpectedConnection); });
						if (Version == TEXT("graph.connect-pins@1.0")) UpdatedV2.Add(MakeShared<FJsonValueObject>(CloneObject(ExpectedConnection)));
						else UpdatedV2.RemoveAll([&](const TSharedPtr<FJsonValue>& Value)
							{ return CanonicalJsonObject(Value->AsObject()) == CanonicalJsonObject(ExpectedConnection); });
						ExpectedPost->SetArrayField(TEXT("connections"), Updated);
						ExpectedPostV2->SetArrayField(TEXT("connections"), UpdatedV2);
						ExpectedPost = CanonicalizeFixtureTopology(ExpectedPost);
						ExpectedPostV2 = CanonicalizeFixtureTopology(ExpectedPostV2);
					}
				}
			}
			else FailureCode = TEXT("UNQUALIFIED_STAGE_C_OPERATION");

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("operation_id"), OperationId);
			Result->SetStringField(TEXT("operation_type"), Version);
			Result->SetBoolField(TEXT("succeeded"), bSucceeded);
			Result->SetBoolField(TEXT("mutation_occurred"), bMutationOccurred);
			if (!bSucceeded) Result->SetStringField(TEXT("failure_code"), FailureCode);
			PerOperationResults.Add(MakeShared<FJsonValueObject>(Result));
			if (!bSucceeded)
			{
				FailingOperation = OperationId; FailingType = Version;
				if (Applied.IsEmpty())
				{
					Evidence->SetArrayField(TEXT("per_operation_results"), PerOperationResults);
					SetFailure(Receipt, TEXT("MUTATION"), FailureCode, TEXT("FAILED_PRE_MUTATION"), TEXT("Stage C operation failed before any mutation"));
					return Complete(Receipt);
				}
				return RollbackWholeBuild(TEXT("MUTATION"), FailureCode, FailingOperation, false);
			}
			const int32 Middle = Operations->Num() / 2;
			if ((FaultCheckpoint == TEXT("AFTER_FIRST_MUTATION") && Index == 0)
				|| (FaultCheckpoint == TEXT("AFTER_MIDDLE_MUTATION") && Index == Middle)
				|| (FaultCheckpoint == TEXT("AFTER_FINAL_MUTATION") && Index == Operations->Num() - 1)
				|| (FaultCheckpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT") && Index == Operations->Num() - 1))
				return RollbackWholeBuild(TEXT("MUTATION"), TEXT("FORCED_OPERATION_FAILURE"), OperationId, false);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		TSharedPtr<FJsonObject> MutationEvidence = Attempt(true, true);
		MutationEvidence->SetNumberField(TEXT("operation_count"), Operations->Num());
		Evidence->SetObjectField(TEXT("mutation"), MutationEvidence);
		Evidence->SetArrayField(TEXT("per_operation_results"), PerOperationResults);
		if (FaultCheckpoint == TEXT("AFTER_MUTATION"))
			return RollbackWholeBuild(TEXT("MUTATION"), TEXT("FORCED_FAILURE_AFTER_MUTATION"), Operations->Last()->AsObject()->GetStringField(TEXT("operation_id")), false);

		TSharedPtr<FJsonObject> CompileEvidence;
		const bool bCompiled = FaultCheckpoint == TEXT("BEFORE_COMPILE") ? false : CompileWithoutSave(Blueprint, CompileEvidence);
		if (!CompileEvidence.IsValid()) CompileEvidence = Attempt(true, false);
		Evidence->SetObjectField(TEXT("compile"), CompileEvidence);
		if (!bCompiled) return RollbackWholeBuild(TEXT("COMPILE"), TEXT("BLUEPRINT_COMPILE_FAILED"), FailingOperation, false);
		if (FaultCheckpoint == TEXT("AFTER_COMPILE_BEFORE_VERIFY"))
			return RollbackWholeBuild(TEXT("VERIFY"), TEXT("FORCED_AFTER_COMPILE_BEFORE_VERIFY"), FailingOperation, false);

		bool bPostComplete = false;
		const TSharedPtr<FJsonObject> PostTopology = SemanticTopology(Graph, GraphType, bPostComplete);
		bool bPostV2Complete = false;
		const TSharedPtr<FJsonObject> PostTopologyV2 = SemanticTopologyV2(Graph, GraphType, bPostV2Complete);
		const FString PostFingerprint = bPostComplete ? Sha256(CanonicalJsonObject(PostTopology)) : FString();
		Evidence->SetStringField(TEXT("post_semantic_fingerprint"), PostFingerprint);
		Evidence->SetStringField(TEXT("post_semantic_fingerprint_v2"),
			bPostV2Complete ? Sha256(CanonicalJsonObject(PostTopologyV2)) : FString());
		const TSharedPtr<FJsonObject> CanonicalExpectedPost = CanonicalizeFixtureTopology(ExpectedPost);
		const TSharedPtr<FJsonObject> CanonicalExpectedPostV2 = CanonicalizeFixtureTopology(ExpectedPostV2);
		bool bVerified = bPostComplete && bPostV2Complete && CanonicalExpectedPost.IsValid() && CanonicalExpectedPostV2.IsValid()
			&& CanonicalJsonObject(PostTopology) == CanonicalJsonObject(CanonicalExpectedPost)
			&& CanonicalJsonObject(PostTopologyV2) == CanonicalJsonObject(CanonicalExpectedPostV2)
			&& FaultCheckpoint != TEXT("VERIFICATION_FAILURE");
		const TArray<TSharedPtr<FJsonValue>>* BaselineNodes = ArrayField(BaselineTopology, TEXT("nodes"));
		const TArray<TSharedPtr<FJsonValue>>* PostNodes = ArrayField(PostTopology, TEXT("nodes"));
		if (!BaselineNodes || !PostNodes || PostNodes->Num() != BaselineNodes->Num() + LogicalNodes.Num()) bVerified = false;
		for (const auto& Pair : LogicalNodes)
		{
			if (UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Pair.Value))
			{
				if (Branch->Pins.Num() != 4) bVerified = false;
			}
			else if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Pair.Value))
			{
				if (Call->GetClass() != UK2Node_CallFunction::StaticClass() || !Call->GetTargetFunction()) bVerified = false;
			}
			else bVerified = false;
		}
		for (const FAppliedOperation& Action : Applied)
		{
			if (Action.Kind == EAppliedKind::SetDefault && (!Action.To || Action.To->DefaultValue == Action.PreviousDefault)) bVerified = false;
			if (Action.Kind == EAppliedKind::Connected && (!Action.From || !Action.To || !Action.From->LinkedTo.Contains(Action.To))) bVerified = false;
			if (Action.Kind == EAppliedKind::Disconnected && (!Action.From || !Action.To || Action.From->LinkedTo.Contains(Action.To))) bVerified = false;
		}
		TSharedPtr<FJsonObject> VerificationEvidence = Attempt(true, bVerified);
		VerificationEvidence->SetBoolField(TEXT("exact_expected_delta"), bVerified);
		VerificationEvidence->SetBoolField(TEXT("unchanged_invariants"), bVerified);
		Evidence->SetObjectField(TEXT("verification"), VerificationEvidence);
		if (!bVerified) return RollbackWholeBuild(TEXT("VERIFY"), TEXT("EXACT_SEMANTIC_DELTA_FAILED"), FailingOperation, false);
		if (FaultCheckpoint == TEXT("AFTER_VERIFY_BEFORE_SAVE"))
			return RollbackWholeBuild(TEXT("SAVE"), TEXT("FORCED_AFTER_VERIFY_BEFORE_SAVE"), FailingOperation, false);

		TSharedPtr<FJsonObject> SaveEvidence = Attempt(true, false);
		Evidence->SetObjectField(TEXT("save"), SaveEvidence);
		if (FaultCheckpoint == TEXT("SAVE_FAILURE"))
			return RollbackWholeBuild(TEXT("SAVE"), TEXT("FORCED_SAVE_FAILURE"), FailingOperation, false);
		const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
		SaveEvidence->SetBoolField(TEXT("succeeded"), bSaved);
		if (!bSaved) return RollbackWholeBuild(TEXT("SAVE"), TEXT("BLUEPRINT_SAVE_FAILED"), FailingOperation, true);
		bool bPersistedComplete = false;
		const TSharedPtr<FJsonObject> PersistedTopology = SemanticTopology(Graph, GraphType, bPersistedComplete);
		bool bPersistedV2Complete = false;
		const TSharedPtr<FJsonObject> PersistedTopologyV2 = SemanticTopologyV2(Graph, GraphType, bPersistedV2Complete);
		Evidence->SetStringField(TEXT("persisted_semantic_fingerprint_v2"),
			bPersistedV2Complete ? Sha256(CanonicalJsonObject(PersistedTopologyV2)) : FString());
		const bool bPersisted = bPersistedComplete && bPersistedV2Complete && !Package->IsDirty()
			&& CanonicalJsonObject(PersistedTopology) == CanonicalJsonObject(PostTopology)
			&& CanonicalJsonObject(PersistedTopologyV2) == CanonicalJsonObject(PostTopologyV2);
		SaveEvidence->SetBoolField(TEXT("persisted_verified"), bPersisted);
		SaveEvidence->SetBoolField(TEXT("package_clean"), !Package->IsDirty());
		DirtyState->SetBoolField(TEXT("final"), Package->IsDirty());
		Evidence->SetObjectField(TEXT("rollback"), Attempt(false, false));
		TSharedPtr<FJsonObject> Observed = MakeShared<FJsonObject>();
		Observed->SetStringField(TEXT("semantic_fingerprint"), PostFingerprint);
		Observed->SetBoolField(TEXT("verified"), bPersisted);
		Evidence->SetObjectField(TEXT("observed_result"), Observed);
		if (!bPersisted)
		{
			SetFailure(Receipt, TEXT("SAVE"), TEXT("PERSISTED_STATE_UNPROVEN"), TEXT("QUARANTINED"),
				TEXT("Save returned but persisted Stage C state could not be proven"));
			return Complete(Receipt);
		}
		Receipt->SetStringField(TEXT("state"), TEXT("SUCCESS"));
		return Complete(Receipt, FaultCheckpoint == TEXT("AFTER_SAVE_BEFORE_FINAL_RECEIPT"));
	}
	const TSharedPtr<FJsonObject> Operation = Operations && Operations->Num() == 1 ? (*Operations)[0]->AsObject() : nullptr;
	const TSharedPtr<FJsonObject> Payload = ObjectField(Operation, TEXT("semantic_payload"));
	FString CandidateOperationVersion;
	if (Operation.IsValid()
		&& Operation->TryGetStringField(TEXT("operation_version"), CandidateOperationVersion)
		&& CandidateOperationVersion != OperationVersion)
	{
		enum class EStageBOperation { AddNode, ConnectPins, DisconnectPins };
		EStageBOperation StageBOperation;
		const TCHAR* ExpectedCapability = nullptr;
		const TCHAR* ExpectedSpecKind = nullptr;
		const TCHAR* ExpectedHandler = nullptr;
		if (CandidateOperationVersion == TEXT("graph.add-node@1.0"))
		{
			StageBOperation = EStageBOperation::AddNode;
			ExpectedCapability = AddNodeCapability;
			ExpectedSpecKind = TEXT("add_node");
			ExpectedHandler = TEXT("spacehead.graph.add-node-handler@1.0");
		}
		else if (CandidateOperationVersion == TEXT("graph.connect-pins@1.0"))
		{
			StageBOperation = EStageBOperation::ConnectPins;
			ExpectedCapability = ConnectPinsCapability;
			ExpectedSpecKind = TEXT("connect_pins");
			ExpectedHandler = TEXT("spacehead.graph.connect-pins-handler@1.0");
		}
		else if (CandidateOperationVersion == TEXT("graph.disconnect-pins@1.0"))
		{
			StageBOperation = EStageBOperation::DisconnectPins;
			ExpectedCapability = DisconnectPinsCapability;
			ExpectedSpecKind = TEXT("disconnect_pins");
			ExpectedHandler = TEXT("spacehead.graph.disconnect-pins-handler@1.0");
		}
		else
		{
			SetFailure(Receipt, TEXT("SPEC"), TEXT("UNQUALIFIED_ATOMIC_PLAN"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("The graph operation is outside the qualified Stage B vocabulary"));
			return Complete(Receipt);
		}

		const TSharedPtr<FJsonObject> SpecTarget = ObjectField(BuildSpec, TEXT("target"));
		const TArray<TSharedPtr<FJsonValue>>* SpecOperations = ArrayField(BuildSpec, TEXT("operations"));
		const TSharedPtr<FJsonObject> SpecOperation = SpecOperations && SpecOperations->Num() == 1
			? (*SpecOperations)[0]->AsObject() : nullptr;
		const TSharedPtr<FJsonObject> SpecPayload = ObjectField(SpecOperation, TEXT("payload"));
		const TSharedPtr<FJsonObject> SpecPolicy = ObjectField(BuildSpec, TEXT("policy"));
		FString PackagePath;
		FString BlueprintName;
		FString Capability;
		FString HandlerIdentity;
		FString SelectorKind;
		FString SelectorName;
		FString OperationId;
		FString SpecVersion;
		FString SpecRequestId;
		FString SpecOperationId;
		FString SpecFamily;
		FString SpecKind;
		FString SpecOperationVersion;
		FString Atomicity;
		bool bCompileWithoutSave = false;
		bool bSaveAfterVerification = false;
		bool bRollbackOnFailure = false;
		if (!Target.IsValid() || !Selectors || Selectors->Num() != 1 || !Operation.IsValid() || !Payload.IsValid()
			|| !SpecTarget.IsValid() || !SpecOperation.IsValid() || !SpecPayload.IsValid() || !SpecPolicy.IsValid()
			|| CanonicalJsonObject(SpecTarget) != CanonicalJsonObject(Target)
			|| !Target->TryGetStringField(TEXT("package_path"), PackagePath)
			|| !Target->TryGetStringField(TEXT("blueprint_name"), BlueprintName)
			|| !PackagePath.StartsWith(TEXT("/Game/Tests/Builder/"))
			|| PackagePath != TEXT("/Game/Tests/Builder/") + BlueprintName
			|| !Operation->TryGetStringField(TEXT("semantic_capability_id"), Capability) || Capability != ExpectedCapability
			|| !Operation->TryGetStringField(TEXT("operation_id"), OperationId) || OperationId.IsEmpty()
			|| !(*Selectors)[0]->AsObject()->TryGetStringField(TEXT("kind"), SelectorKind)
			|| !(*Selectors)[0]->AsObject()->TryGetStringField(TEXT("name"), SelectorName)
			|| !BuildSpec->TryGetStringField(TEXT("spec_version"), SpecVersion)
			|| SpecVersion != TEXT("spacehead.blueprint-build-spec@1.0")
			|| !BuildSpec->TryGetStringField(TEXT("request_id"), SpecRequestId) || SpecRequestId.IsEmpty()
			|| !SpecOperation->TryGetStringField(TEXT("operation_id"), SpecOperationId) || SpecOperationId != OperationId
			|| !SpecOperation->TryGetStringField(TEXT("family"), SpecFamily) || SpecFamily != TEXT("graph")
			|| !SpecOperation->TryGetStringField(TEXT("kind"), SpecKind) || SpecKind != ExpectedSpecKind
			|| !SpecOperation->TryGetStringField(TEXT("operation_version"), SpecOperationVersion)
			|| SpecOperationVersion != CandidateOperationVersion
			|| !SpecPolicy->TryGetStringField(TEXT("atomicity"), Atomicity) || Atomicity != TEXT("ONE_BLUEPRINT_PACKAGE")
			|| !SpecPolicy->TryGetBoolField(TEXT("compile_without_save"), bCompileWithoutSave) || !bCompileWithoutSave
			|| !SpecPolicy->TryGetBoolField(TEXT("save_after_verification_only"), bSaveAfterVerification) || !bSaveAfterVerification
			|| !SpecPolicy->TryGetBoolField(TEXT("rollback_on_failure"), bRollbackOnFailure) || !bRollbackOnFailure)
		{
			SetFailure(Receipt, TEXT("SPEC"), TEXT("BUILD_SPEC_PLAN_MISMATCH"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Canonical Stage B BuildSpec semantics do not exactly match the executable BuildPlan"));
			return Complete(Receipt);
		}

		const TSharedPtr<FJsonObject> BaselineTopology = ObjectField(Payload, TEXT("baseline_semantic_topology"));
		FString BaselineSemanticHash;
		if (!BaselineTopology.IsValid()
			|| !Payload->TryGetStringField(TEXT("baseline_semantic_fingerprint"), BaselineSemanticHash)
			|| !ValidHash(BaselineSemanticHash)
			|| Sha256(CanonicalJsonObject(BaselineTopology)) != BaselineSemanticHash)
		{
			SetFailure(Receipt, TEXT("BASELINE"), TEXT("INVALID_SEMANTIC_BASELINE"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Canonical semantic baseline integrity failed"));
			return Complete(Receipt);
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
			SetFailure(Receipt, TEXT("CONCURRENCY"), TEXT("STALE_OR_INVALID_FENCING"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Bridge fencing validation failed closed"));
			return Complete(Receipt);
		}

		FString FaultCheckpoint;
		if (!ReadTestFaultCheckpoint(TestHooks, PackagePath, FaultCheckpoint))
		{
			SetFailure(Receipt, TEXT("SPEC"), TEXT("TEST_HOOKS_NOT_AUTHORIZED"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Fault injection requires the versioned disposable ue_mcp test-project gate"));
			return Complete(Receipt);
		}
		TSharedPtr<FJsonObject> Evidence = ObjectField(Receipt, TEXT("evidence"));
		Evidence->SetNumberField(TEXT("dispatch_count"), 1);
		Evidence->SetStringField(TEXT("operation_type"), CandidateOperationVersion);
		Evidence->SetObjectField(TEXT("requested_identity"), CloneObject(SpecPayload));
		if (!FaultCheckpoint.IsEmpty()) Evidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		TSharedPtr<FJsonObject> Received = CloneObject(Receipt);
		Received->SetStringField(TEXT("state"), TEXT("UNKNOWN"));
		SetFailure(Received, TEXT("TRANSPORT_UNKNOWN"), TEXT("EXECUTION_IN_PROGRESS_OR_INTERRUPTED"), TEXT("UNKNOWN"),
			TEXT("The bridge durably received the transaction but no terminal outcome is yet recorded"));
		if (!PersistDurableExecution(TransactionId, CorrelationId, PlanHash, Fencing, Received))
		{
			SetFailure(Receipt, TEXT("INTERNAL_CONTRACT"), TEXT("DURABLE_RECEIPT_NOT_ESTABLISHED"), TEXT("QUARANTINED"),
				TEXT("Mutation was denied because durable correlated evidence could not be established"));
			return Complete(Receipt);
		}
		if (FaultCheckpoint == TEXT("BEFORE_PREFLIGHT"))
		{
			TSharedPtr<FJsonObject> PreflightEvidence = Attempt(false, false);
			PreflightEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			PreflightEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
			Evidence->SetObjectField(TEXT("preflight"), PreflightEvidence);
			SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("FORCED_BEFORE_PREFLIGHT"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Test-only failure injected before live preflight"));
			return Complete(Receipt);
		}

		UBlueprint* Blueprint = LoadBlueprint(PackagePath);
		UEdGraph* Graph = Blueprint ? ResolveTargetGraph(Blueprint, SelectorKind, SelectorName) : nullptr;
		UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		const bool bDirtyBefore = Package && Package->IsDirty();
		bool bPreComplete = false;
		const FString GraphType = BaselineTopology->GetObjectField(TEXT("graph"))->GetStringField(TEXT("graph_type"));
		TSharedPtr<FJsonObject> PreTopology = Graph ? SemanticTopology(Graph, GraphType, bPreComplete) : nullptr;
		bool bPreV2Complete = false;
		const TSharedPtr<FJsonObject> PreTopologyV2 = Graph ? SemanticTopologyV2(Graph, GraphType, bPreV2Complete) : nullptr;
		const bool bDirtyAfterPreflight = Package && Package->IsDirty();

		FString CreatedNodeGuid;
		FString LogicalId;
		FString NodeClass;
		FString SemanticType;
		double PositionX = 0;
		double PositionY = 0;
		FGuid ParsedCreatedGuid;
		FString FromNodeGuid;
		FString FromNodeId;
		FString FromPinId;
		FString FromPinName;
		FString ToNodeGuid;
		FString ToNodeId;
		FString ToPinId;
		FString ToPinName;
		UEdGraphNode* FromNode = nullptr;
		UEdGraphNode* ToNode = nullptr;
		UEdGraphPin* FromPin = nullptr;
		UEdGraphPin* ToPin = nullptr;
		UEdGraphNode* AddedNode = nullptr;
		TSharedPtr<FJsonObject> ExpectedPost = CloneObject(BaselineTopology);
		bool bOperationPreflight = false;

		if (StageBOperation == EStageBOperation::AddNode)
		{
			const TSharedPtr<FJsonObject> Position = ObjectField(Payload, TEXT("position"));
			const TSharedPtr<FJsonObject> SpecPosition = ObjectField(SpecPayload, TEXT("position"));
			FString SpecLogicalId;
			FString SpecNodeClass;
			FString SpecSemanticType;
			double SpecX = 0;
			double SpecY = 0;
			bOperationPreflight = Position.IsValid() && SpecPosition.IsValid()
				&& Payload->TryGetStringField(TEXT("logical_id"), LogicalId)
				&& Payload->TryGetStringField(TEXT("node_class"), NodeClass) && NodeClass == TEXT("K2Node_IfThenElse")
				&& Payload->TryGetStringField(TEXT("semantic_type"), SemanticType) && SemanticType == TEXT("branch")
				&& Payload->TryGetStringField(TEXT("created_node_guid"), CreatedNodeGuid)
				&& FGuid::Parse(CreatedNodeGuid, ParsedCreatedGuid) && ParsedCreatedGuid.IsValid()
				&& Position->TryGetNumberField(TEXT("x"), PositionX) && Position->TryGetNumberField(TEXT("y"), PositionY)
				&& FMath::FloorToDouble(PositionX) == PositionX && FMath::Abs(PositionX) <= 1000000
				&& FMath::FloorToDouble(PositionY) == PositionY && FMath::Abs(PositionY) <= 1000000
				&& SpecPayload->TryGetStringField(TEXT("logical_id"), SpecLogicalId) && SpecLogicalId == LogicalId
				&& SpecPayload->TryGetStringField(TEXT("node_class"), SpecNodeClass) && SpecNodeClass == NodeClass
				&& SpecPayload->TryGetStringField(TEXT("semantic_type"), SpecSemanticType) && SpecSemanticType == SemanticType
				&& SpecPosition->TryGetNumberField(TEXT("x"), SpecX) && SpecX == PositionX
				&& SpecPosition->TryGetNumberField(TEXT("y"), SpecY) && SpecY == PositionY
				&& Graph && !ResolveNode(Graph, CreatedNodeGuid) && Cast<UEdGraphSchema_K2>(Schema);
		}
		else
		{
			const TSharedPtr<FJsonObject> SpecFrom = ObjectField(SpecPayload, TEXT("from"));
			const TSharedPtr<FJsonObject> SpecTo = ObjectField(SpecPayload, TEXT("to"));
			const TSharedPtr<FJsonObject> SpecFromNode = ObjectField(SpecFrom, TEXT("node"));
			const TSharedPtr<FJsonObject> SpecToNode = ObjectField(SpecTo, TEXT("node"));
			FString SpecFromKind;
			FString SpecFromGuid;
			FString SpecFromName;
			FString SpecFromDirection;
			FString SpecToKind;
			FString SpecToGuid;
			FString SpecToName;
			FString SpecToDirection;
			FString Classification;
			const bool bPayloadValid = Payload->TryGetStringField(TEXT("from_node_guid"), FromNodeGuid)
				&& Payload->TryGetStringField(TEXT("from_node_id"), FromNodeId)
				&& Payload->TryGetStringField(TEXT("from_pin_id"), FromPinId)
				&& Payload->TryGetStringField(TEXT("from_pin_name"), FromPinName)
				&& Payload->TryGetStringField(TEXT("to_node_guid"), ToNodeGuid)
				&& Payload->TryGetStringField(TEXT("to_node_id"), ToNodeId)
				&& Payload->TryGetStringField(TEXT("to_pin_id"), ToPinId)
				&& Payload->TryGetStringField(TEXT("to_pin_name"), ToPinName)
				&& Payload->TryGetStringField(TEXT("classification"), Classification) && Classification == TEXT("execution")
				&& SpecFrom.IsValid() && SpecTo.IsValid() && SpecFromNode.IsValid() && SpecToNode.IsValid()
				&& SpecFromNode->TryGetStringField(TEXT("kind"), SpecFromKind) && SpecFromKind == TEXT("existing")
				&& SpecFromNode->TryGetStringField(TEXT("node_guid"), SpecFromGuid)
				&& NormalizeGuid(SpecFromGuid) == NormalizeGuid(FromNodeGuid)
				&& SpecFrom->TryGetStringField(TEXT("pin_name"), SpecFromName) && SpecFromName == FromPinName
				&& SpecFrom->TryGetStringField(TEXT("direction"), SpecFromDirection) && SpecFromDirection == TEXT("output")
				&& SpecToNode->TryGetStringField(TEXT("kind"), SpecToKind) && SpecToKind == TEXT("existing")
				&& SpecToNode->TryGetStringField(TEXT("node_guid"), SpecToGuid)
				&& NormalizeGuid(SpecToGuid) == NormalizeGuid(ToNodeGuid)
				&& SpecTo->TryGetStringField(TEXT("pin_name"), SpecToName) && SpecToName == ToPinName
				&& SpecTo->TryGetStringField(TEXT("direction"), SpecToDirection) && SpecToDirection == TEXT("input");
			FromNode = Graph ? ResolveNode(Graph, FromNodeGuid) : nullptr;
			ToNode = Graph ? ResolveNode(Graph, ToNodeGuid) : nullptr;
			FromPin = FromNode ? ResolvePin(FromNode, FromPinId, FromPinName, EGPD_Output) : nullptr;
			ToPin = ToNode ? ResolvePin(ToNode, ToPinId, ToPinName, EGPD_Input) : nullptr;
			const bool bExactExecutionPins = FromPin && ToPin && FromNode != ToNode
				&& FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				&& ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				&& FromPin->PinType.ContainerType == EPinContainerType::None
				&& ToPin->PinType.ContainerType == EPinContainerType::None
				&& Cast<UEdGraphSchema_K2>(Schema);
			const bool bLinked = bExactExecutionPins && FromPin->LinkedTo.Contains(ToPin) && ToPin->LinkedTo.Contains(FromPin);
			if (StageBOperation == EStageBOperation::ConnectPins)
			{
				const FPinConnectionResponse Response = bExactExecutionPins
					? Schema->CanCreateConnection(FromPin, ToPin)
					: FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, FText::GetEmpty());
				bOperationPreflight = bPayloadValid && bExactExecutionPins && !bLinked
					&& FromPin->LinkedTo.IsEmpty() && ToPin->LinkedTo.IsEmpty()
					&& Response.Response == CONNECT_RESPONSE_MAKE;
			}
			else
			{
				bOperationPreflight = bPayloadValid && bExactExecutionPins && bLinked;
			}

			const TSharedPtr<FJsonObject> ExpectedConnection = MakeShared<FJsonObject>();
			ExpectedConnection->SetStringField(TEXT("from_node_id"), FromNodeId);
			ExpectedConnection->SetStringField(TEXT("from_pin_id"), FromPinId);
			ExpectedConnection->SetStringField(TEXT("to_node_id"), ToNodeId);
			ExpectedConnection->SetStringField(TEXT("to_pin_id"), ToPinId);
			ExpectedConnection->SetStringField(TEXT("classification"), TEXT("execution"));
			const TArray<TSharedPtr<FJsonValue>>* BaselineConnections = ArrayField(ExpectedPost, TEXT("connections"));
			TArray<TSharedPtr<FJsonValue>> UpdatedConnections = BaselineConnections ? *BaselineConnections : TArray<TSharedPtr<FJsonValue>>();
			if (StageBOperation == EStageBOperation::ConnectPins)
			{
				UpdatedConnections.Add(MakeShared<FJsonValueObject>(ExpectedConnection));
			}
			else
			{
				UpdatedConnections.RemoveAll([&](const TSharedPtr<FJsonValue>& Value)
				{
					return CanonicalJsonObject(Value->AsObject()) == CanonicalJsonObject(ExpectedConnection);
				});
			}
			UpdatedConnections.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return CanonicalJsonObject(A->AsObject()) < CanonicalJsonObject(B->AsObject());
			});
			ExpectedPost->SetArrayField(TEXT("connections"), UpdatedConnections);
		}

		const bool bPreflightSucceeded = Blueprint && Graph && Package && Schema && bPreComplete && bPreV2Complete
			&& !bDirtyBefore && !bDirtyAfterPreflight && bOperationPreflight
			&& CanonicalJsonObject(PreTopology) == CanonicalJsonObject(BaselineTopology);
		TSharedPtr<FJsonObject> DirtyState = MakeShared<FJsonObject>();
		DirtyState->SetBoolField(TEXT("before"), bDirtyBefore);
		DirtyState->SetBoolField(TEXT("after_preflight"), bDirtyAfterPreflight);
		Evidence->SetObjectField(TEXT("dirty_state"), DirtyState);
		TSharedPtr<FJsonObject> PreflightEvidence = Attempt(true, bPreflightSucceeded);
		PreflightEvidence->SetBoolField(TEXT("target_resolved"), Blueprint && Graph && Package && bOperationPreflight);
		PreflightEvidence->SetBoolField(TEXT("semantic_baseline_exact"), bPreflightSucceeded);
		PreflightEvidence->SetStringField(TEXT("handler_version"), ExpectedHandler);
		PreflightEvidence->SetStringField(TEXT("topology_version"), TopologyVersion);
		Evidence->SetObjectField(TEXT("preflight"), PreflightEvidence);
		Evidence->SetStringField(TEXT("pre_semantic_fingerprint"), bPreComplete ? Sha256(CanonicalJsonObject(PreTopology)) : FString());
		Evidence->SetStringField(TEXT("semantic_topology_v2_version"), TopologyV2Version);
		Evidence->SetStringField(TEXT("pre_semantic_fingerprint_v2"),
			bPreV2Complete ? Sha256(CanonicalJsonObject(PreTopologyV2)) : FString());
		if (!bPreflightSucceeded)
		{
			SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("STALE_PLAN_OR_UNSUPPORTED_GRAPH_BEHAVIOR"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Live Unreal preflight did not exactly match the Stage B plan or safe schema behavior"));
			return Complete(Receipt);
		}
		if (FaultCheckpoint == TEXT("AFTER_PREFLIGHT_BEFORE_MUTATION"))
		{
			PreflightEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			PreflightEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
			SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("FORCED_AFTER_PREFLIGHT"), TEXT("FAILED_PRE_MUTATION"),
				TEXT("Test-only failure injected after live preflight and before mutation"));
			return Complete(Receipt);
		}

		TSharedPtr<FJsonObject> ResolvedIdentity = MakeShared<FJsonObject>();
		if (StageBOperation == EStageBOperation::AddNode)
		{
			ResolvedIdentity->SetStringField(TEXT("created_node_guid"), CreatedNodeGuid);
			ResolvedIdentity->SetStringField(TEXT("node_class"), NodeClass);
			ResolvedIdentity->SetStringField(TEXT("semantic_type"), SemanticType);
		}
		else
		{
			ResolvedIdentity->SetStringField(TEXT("from_node_guid"), FromNodeGuid);
			ResolvedIdentity->SetStringField(TEXT("from_pin_id"), FromPinId);
			ResolvedIdentity->SetStringField(TEXT("to_node_guid"), ToNodeGuid);
			ResolvedIdentity->SetStringField(TEXT("to_pin_id"), ToPinId);
		}
		Evidence->SetObjectField(TEXT("resolved_identity"), ResolvedIdentity);
		TSharedPtr<FJsonObject> IntendedResult = MakeShared<FJsonObject>();
		IntendedResult->SetStringField(TEXT("operation"), ExpectedSpecKind);
		IntendedResult->SetStringField(TEXT("baseline_fingerprint"), BaselineSemanticHash);
		Evidence->SetObjectField(TEXT("intended_result"), IntendedResult);

		auto VerifyAddedNode = [&](const TSharedPtr<FJsonObject>& Topology) -> bool
		{
			if (!Topology.IsValid()) return false;
			const TArray<TSharedPtr<FJsonValue>>* PostNodes = ArrayField(Topology, TEXT("nodes"));
			const TArray<TSharedPtr<FJsonValue>>* BaselineNodes = ArrayField(BaselineTopology, TEXT("nodes"));
			if (!PostNodes || !BaselineNodes || PostNodes->Num() != BaselineNodes->Num() + 1) return false;
			TArray<TSharedPtr<FJsonValue>> Remaining;
			TSharedPtr<FJsonObject> CreatedSemantic;
			for (const TSharedPtr<FJsonValue>& Value : *PostNodes)
			{
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				if (Node.IsValid() && NormalizeGuid(Node->GetStringField(TEXT("existing_node_guid"))) == NormalizeGuid(CreatedNodeGuid))
				{
					if (CreatedSemantic.IsValid()) return false;
					CreatedSemantic = Node;
				}
				else Remaining.Add(Value);
			}
			if (!CreatedSemantic.IsValid()
				|| CreatedSemantic->GetStringField(TEXT("node_class")) != TEXT("K2Node_IfThenElse")
				|| CreatedSemantic->GetStringField(TEXT("semantic_type")) != TEXT("branch")) return false;
			TSharedPtr<FJsonObject> WithoutCreated = CloneObject(Topology);
			WithoutCreated->SetArrayField(TEXT("nodes"), Remaining);
			if (CanonicalJsonObject(WithoutCreated) != CanonicalJsonObject(BaselineTopology)) return false;
			UEdGraphNode* LiveNode = ResolveNode(Graph, CreatedNodeGuid);
			if (!LiveNode || !LiveNode->IsA<UK2Node_IfThenElse>()
				|| LiveNode->NodePosX != static_cast<int32>(PositionX)
				|| LiveNode->NodePosY != static_cast<int32>(PositionY)
				|| LiveNode->Pins.Num() != 4) return false;
			int32 ExecuteInputs = 0, BoolInputs = 0, ThenOutputs = 0, ElseOutputs = 0;
			for (UEdGraphPin* LivePin : LiveNode->Pins)
			{
				if (!LivePin || !LivePin->PinId.IsValid()) return false;
				if (LivePin->Direction == EGPD_Input && LivePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
					&& LivePin->PinName == UEdGraphSchema_K2::PN_Execute) ++ExecuteInputs;
				else if (LivePin->Direction == EGPD_Input && LivePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
					&& LivePin->PinName == UEdGraphSchema_K2::PN_Condition) ++BoolInputs;
				else if (LivePin->Direction == EGPD_Output && LivePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
					&& LivePin->PinName == UEdGraphSchema_K2::PN_Then) ++ThenOutputs;
				else if (LivePin->Direction == EGPD_Output && LivePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
					&& LivePin->PinName == UEdGraphSchema_K2::PN_Else) ++ElseOutputs;
				else return false;
			}
			return ExecuteInputs == 1 && BoolInputs == 1 && ThenOutputs == 1 && ElseOutputs == 1;
		};

		auto Rollback = [&](const TCHAR* FailurePhase, const FString& FailureCode, bool bPersistedStateUncertain) -> TSharedPtr<FJsonValue>
		{
			TSharedPtr<FJsonObject> RollbackEvidence = Attempt(true, false);
			const bool bForceRollbackFailure = FaultCheckpoint == TEXT("ROLLBACK_FAILURE");
			bool bMutationRestored = false;
			if (!bForceRollbackFailure)
			{
				if (StageBOperation == EStageBOperation::AddNode)
				{
					UEdGraphNode* ExistingAdded = ResolveNode(Graph, CreatedNodeGuid);
					if (ExistingAdded)
					{
						Graph->Modify();
						ExistingAdded->Modify();
						Graph->RemoveNode(ExistingAdded);
						Graph->NotifyGraphChanged();
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					}
					bMutationRestored = ResolveNode(Graph, CreatedNodeGuid) == nullptr;
				}
				else if (StageBOperation == EStageBOperation::ConnectPins)
				{
					if (FromPin->LinkedTo.Contains(ToPin)) Schema->BreakSinglePinLink(FromPin, ToPin);
					bMutationRestored = !FromPin->LinkedTo.Contains(ToPin) && !ToPin->LinkedTo.Contains(FromPin);
				}
				else
				{
					const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
					bMutationRestored = Response.Response == CONNECT_RESPONSE_MAKE
						&& Schema->TryCreateConnection(FromPin, ToPin)
						&& FromPin->LinkedTo.Contains(ToPin) && ToPin->LinkedTo.Contains(FromPin);
				}
			}
			TSharedPtr<FJsonObject> RollbackCompile;
			const bool bCompileRestored = bMutationRestored && CompileWithoutSave(Blueprint, RollbackCompile);
			bool bRollbackComplete = false;
			const TSharedPtr<FJsonObject> RollbackTopology = SemanticTopology(Graph, GraphType, bRollbackComplete);
			const bool bSemanticRestored = bCompileRestored && bRollbackComplete
				&& CanonicalJsonObject(RollbackTopology) == CanonicalJsonObject(BaselineTopology);
			if (bSemanticRestored && !bPersistedStateUncertain) Package->SetDirtyFlag(false);
			const bool bCleanRestored = !Package->IsDirty();
			const bool bRestored = bSemanticRestored && bCleanRestored && !bPersistedStateUncertain;
			RollbackEvidence->SetBoolField(TEXT("succeeded"), bRestored);
			RollbackEvidence->SetBoolField(TEXT("semantic_restoration_verified"), bSemanticRestored);
			RollbackEvidence->SetBoolField(TEXT("clean_state_restored"), bCleanRestored);
			RollbackEvidence->SetBoolField(TEXT("persisted_state_uncertain"), bPersistedStateUncertain);
			if (bForceRollbackFailure)
			{
				RollbackEvidence->SetBoolField(TEXT("test_failure_injected"), true);
				RollbackEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
			}
			RollbackEvidence->SetObjectField(TEXT("compile"), RollbackCompile.IsValid() ? RollbackCompile : Attempt(false, false));
			Evidence->SetObjectField(TEXT("rollback"), RollbackEvidence);
			DirtyState->SetBoolField(TEXT("final"), Package->IsDirty());
			if (bRestored)
				SetFailure(Receipt, FailurePhase, FailureCode, TEXT("RESTORED"), TEXT("Mutation failed and the explicit before-state was restored and verified"));
			else
				SetFailure(Receipt, TEXT("ROLLBACK"), TEXT("RESTORATION_UNPROVEN"), TEXT("QUARANTINED"), TEXT("Mutation failed and exact restoration could not be proven"));
			return Complete(Receipt, FaultCheckpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT"));
		};

		Blueprint->Modify();
		Graph->Modify();
		TSharedPtr<FJsonObject> MutationEvidence = Attempt(true, false);
		Evidence->SetObjectField(TEXT("mutation"), MutationEvidence);
		bool bMutated = false;
		if (StageBOperation == EStageBOperation::AddNode)
		{
			UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
			if (Branch)
			{
				Graph->AddNode(Branch, false, false);
				Branch->NodeGuid = ParsedCreatedGuid;
				Branch->NodePosX = static_cast<int32>(PositionX);
				Branch->NodePosY = static_cast<int32>(PositionY);
				Branch->AllocateDefaultPins();
				Branch->PostPlacedNewNode();
				Graph->NotifyGraphChanged();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				AddedNode = Branch;
				bMutated = ResolveNode(Graph, CreatedNodeGuid) == Branch;
			}
			MutationEvidence->SetNumberField(TEXT("changed_nodes"), bMutated ? 1 : 0);
			MutationEvidence->SetStringField(TEXT("created_node_guid"), CreatedNodeGuid);
		}
		else if (StageBOperation == EStageBOperation::ConnectPins)
		{
			bMutated = Schema->TryCreateConnection(FromPin, ToPin)
				&& FromPin->LinkedTo.Contains(ToPin) && ToPin->LinkedTo.Contains(FromPin);
			MutationEvidence->SetNumberField(TEXT("changed_connections"), bMutated ? 1 : 0);
		}
		else
		{
			Schema->BreakSinglePinLink(FromPin, ToPin);
			bMutated = !FromPin->LinkedTo.Contains(ToPin) && !ToPin->LinkedTo.Contains(FromPin);
			MutationEvidence->SetNumberField(TEXT("changed_connections"), bMutated ? 1 : 0);
		}
		MutationEvidence->SetBoolField(TEXT("succeeded"), bMutated);
		if (!bMutated) return Rollback(TEXT("MUTATION"), TEXT("STAGE_B_GRAPH_MUTATION_FAILED"), false);
		if (FaultCheckpoint == TEXT("AFTER_MUTATION") || FaultCheckpoint == TEXT("ROLLBACK_FAILURE")
			|| FaultCheckpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT"))
		{
			MutationEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			MutationEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
			return Rollback(TEXT("MUTATION"), TEXT("FORCED_FAILURE_AFTER_MUTATION"), false);
		}

		TSharedPtr<FJsonObject> CompileEvidence;
		const bool bForcedCompileFailure = FaultCheckpoint == TEXT("BEFORE_COMPILE");
		const bool bCompiled = bForcedCompileFailure ? false : CompileWithoutSave(Blueprint, CompileEvidence);
		if (bForcedCompileFailure)
		{
			CompileEvidence = Attempt(true, false);
			CompileEvidence->SetNumberField(TEXT("errors"), 1);
			CompileEvidence->SetNumberField(TEXT("warnings"), 0);
			CompileEvidence->SetBoolField(TEXT("save_requested"), false);
			CompileEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			CompileEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		}
		Evidence->SetObjectField(TEXT("compile"), CompileEvidence);
		if (!bCompiled) return Rollback(TEXT("COMPILE"), TEXT("BLUEPRINT_COMPILE_FAILED"), false);
		if (FaultCheckpoint == TEXT("AFTER_COMPILE_BEFORE_VERIFY"))
		{
			CompileEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			CompileEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
			return Rollback(TEXT("VERIFY"), TEXT("FORCED_AFTER_COMPILE_BEFORE_VERIFY"), false);
		}

		bool bPostComplete = false;
		const TSharedPtr<FJsonObject> PostTopology = SemanticTopology(Graph, GraphType, bPostComplete);
		bool bPostV2Complete = false;
		const TSharedPtr<FJsonObject> PostTopologyV2 = SemanticTopologyV2(Graph, GraphType, bPostV2Complete);
		const FString PostFingerprint = bPostComplete ? Sha256(CanonicalJsonObject(PostTopology)) : FString();
		Evidence->SetStringField(TEXT("post_semantic_fingerprint"), PostFingerprint);
		Evidence->SetStringField(TEXT("post_semantic_fingerprint_v2"),
			bPostV2Complete ? Sha256(CanonicalJsonObject(PostTopologyV2)) : FString());
		const bool bForcedVerifyFailure = FaultCheckpoint == TEXT("VERIFICATION_FAILURE");
		const bool bVerified = !bForcedVerifyFailure && bPostComplete && bPostV2Complete
			&& (StageBOperation == EStageBOperation::AddNode
				? VerifyAddedNode(PostTopology)
				: CanonicalJsonObject(PostTopology) == CanonicalJsonObject(ExpectedPost));
		TSharedPtr<FJsonObject> VerificationEvidence = Attempt(true, bVerified);
		VerificationEvidence->SetBoolField(TEXT("exact_expected_delta"), bVerified);
		VerificationEvidence->SetBoolField(TEXT("unchanged_invariants"), bVerified);
		if (StageBOperation == EStageBOperation::AddNode) VerificationEvidence->SetNumberField(TEXT("changed_nodes"), bVerified ? 1 : 0);
		else VerificationEvidence->SetNumberField(TEXT("changed_connections"), bVerified ? 1 : 0);
		if (bForcedVerifyFailure)
		{
			VerificationEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			VerificationEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		}
		Evidence->SetObjectField(TEXT("verification"), VerificationEvidence);
		if (!bVerified) return Rollback(TEXT("VERIFY"), TEXT("EXACT_SEMANTIC_DELTA_FAILED"), false);
		if (FaultCheckpoint == TEXT("AFTER_VERIFY_BEFORE_SAVE"))
		{
			VerificationEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			VerificationEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
			return Rollback(TEXT("SAVE"), TEXT("FORCED_AFTER_VERIFY_BEFORE_SAVE"), false);
		}

		TSharedPtr<FJsonObject> SaveEvidence = Attempt(true, false);
		Evidence->SetObjectField(TEXT("save"), SaveEvidence);
		const bool bForcedSaveFailure = FaultCheckpoint == TEXT("SAVE_FAILURE");
		const bool bSaved = !bForcedSaveFailure && UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
		SaveEvidence->SetBoolField(TEXT("succeeded"), bSaved);
		if (bForcedSaveFailure)
		{
			SaveEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			SaveEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		}
		if (!bSaved) return Rollback(TEXT("SAVE"), TEXT("BLUEPRINT_SAVE_FAILED"), true);

		bool bPersistedComplete = false;
		const TSharedPtr<FJsonObject> PersistedTopology = SemanticTopology(Graph, GraphType, bPersistedComplete);
		bool bPersistedV2Complete = false;
		const TSharedPtr<FJsonObject> PersistedTopologyV2 = SemanticTopologyV2(Graph, GraphType, bPersistedV2Complete);
		Evidence->SetStringField(TEXT("persisted_semantic_fingerprint_v2"),
			bPersistedV2Complete ? Sha256(CanonicalJsonObject(PersistedTopologyV2)) : FString());
		const bool bPersisted = bPersistedComplete && bPersistedV2Complete && !Package->IsDirty()
			&& (StageBOperation == EStageBOperation::AddNode
				? VerifyAddedNode(PersistedTopology)
				: CanonicalJsonObject(PersistedTopology) == CanonicalJsonObject(ExpectedPost))
			&& CanonicalJsonObject(PersistedTopologyV2) == CanonicalJsonObject(PostTopologyV2);
		SaveEvidence->SetBoolField(TEXT("persisted_verified"), bPersisted);
		SaveEvidence->SetBoolField(TEXT("package_clean"), !Package->IsDirty());
		DirtyState->SetBoolField(TEXT("final"), Package->IsDirty());
		Evidence->SetObjectField(TEXT("rollback"), Attempt(false, false));
		TSharedPtr<FJsonObject> ObservedResult = MakeShared<FJsonObject>();
		ObservedResult->SetStringField(TEXT("operation"), ExpectedSpecKind);
		ObservedResult->SetStringField(TEXT("semantic_fingerprint"), PostFingerprint);
		ObservedResult->SetBoolField(TEXT("verified"), bPersisted);
		Evidence->SetObjectField(TEXT("observed_result"), ObservedResult);
		if (!bPersisted)
		{
			SetFailure(Receipt, TEXT("SAVE"), TEXT("PERSISTED_STATE_UNPROVEN"), TEXT("QUARANTINED"),
				TEXT("Save returned but persisted clean state could not be proven"));
			return Complete(Receipt);
		}
		Receipt->SetStringField(TEXT("state"), TEXT("SUCCESS"));
		return Complete(Receipt, FaultCheckpoint == TEXT("AFTER_SAVE_BEFORE_FINAL_RECEIPT"));
	}
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
		return Complete(Receipt);
	}
	FString FaultCheckpoint;
	if (!ReadTestFaultCheckpoint(TestHooks, PackagePath, FaultCheckpoint))
	{
		SetFailure(Receipt, TEXT("SPEC"), TEXT("TEST_HOOKS_NOT_AUTHORIZED"), TEXT("FAILED_PRE_MUTATION"),
			TEXT("Fault injection requires the versioned disposable ue_mcp test-project gate"));
		return Complete(Receipt);
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
		return Complete(Receipt);
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
		return Complete(Receipt);
	}

	const TSharedPtr<FJsonObject> BaselineTopology = ObjectField(Payload, TEXT("baseline_semantic_topology"));
	FString BaselineSemanticHash;
	if (!BaselineTopology.IsValid()
		|| !Payload->TryGetStringField(TEXT("baseline_semantic_fingerprint"), BaselineSemanticHash)
		|| !ValidHash(BaselineSemanticHash)
		|| Sha256(CanonicalJsonObject(BaselineTopology)) != BaselineSemanticHash)
	{
		SetFailure(Receipt, TEXT("BASELINE"), TEXT("INVALID_SEMANTIC_BASELINE"), TEXT("FAILED_PRE_MUTATION"), TEXT("Canonical semantic baseline integrity failed"));
		return Complete(Receipt);
	}
	TSharedPtr<FJsonObject> Evidence = ObjectField(Receipt, TEXT("evidence"));
	Evidence->SetNumberField(TEXT("dispatch_count"), 1);
	if (!FaultCheckpoint.IsEmpty()) Evidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
	TSharedPtr<FJsonObject> Received = CloneObject(Receipt);
	Received->SetStringField(TEXT("state"), TEXT("UNKNOWN"));
	SetFailure(Received, TEXT("TRANSPORT_UNKNOWN"), TEXT("EXECUTION_IN_PROGRESS_OR_INTERRUPTED"), TEXT("UNKNOWN"),
		TEXT("The bridge durably received the transaction but no terminal outcome is yet recorded"));
	if (!PersistDurableExecution(TransactionId, CorrelationId, PlanHash, Fencing, Received))
	{
		SetFailure(Receipt, TEXT("INTERNAL_CONTRACT"), TEXT("DURABLE_RECEIPT_NOT_ESTABLISHED"), TEXT("QUARANTINED"),
			TEXT("Mutation was denied because durable correlated evidence could not be established"));
		return Complete(Receipt);
	}
	if (FaultCheckpoint == TEXT("BEFORE_PREFLIGHT"))
	{
		TSharedPtr<FJsonObject> Preflight = Attempt(false, false);
		Preflight->SetBoolField(TEXT("test_failure_injected"), true);
		Preflight->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		Evidence->SetObjectField(TEXT("preflight"), Preflight);
		SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("FORCED_BEFORE_PREFLIGHT"), TEXT("FAILED_PRE_MUTATION"),
			TEXT("Test-only failure injected before live preflight"));
		return Complete(Receipt);
	}

	UBlueprint* Blueprint = LoadBlueprint(PackagePath);
	UEdGraph* Graph = Blueprint ? ResolveTargetGraph(Blueprint, SelectorKind, SelectorName) : nullptr;
	UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
	const bool bDirtyBefore = Package && Package->IsDirty();
	bool bPreComplete = false;
	const FString GraphType = BaselineTopology->GetObjectField(TEXT("graph"))->GetStringField(TEXT("graph_type"));
	TSharedPtr<FJsonObject> PreTopology = Graph ? SemanticTopology(Graph, GraphType, bPreComplete) : nullptr;
	bool bPreV2Complete = false;
	const TSharedPtr<FJsonObject> PreTopologyV2 = Graph ? SemanticTopologyV2(Graph, GraphType, bPreV2Complete) : nullptr;
	const bool bDirtyAfterPreflight = Package && Package->IsDirty();
	UEdGraphNode* Node = Graph ? ResolveNode(Graph, NodeGuid) : nullptr;
	UEdGraphPin* Pin = Node ? ResolvePin(Node, PinId, PinName) : nullptr;
	const FString ExpectedDefault = bExpected ? TEXT("true") : TEXT("false");
	const FString DesiredDefault = bDesired ? TEXT("true") : TEXT("false");
	const bool bPreflight = Blueprint && Graph && Package && Node && Pin && bPreComplete && bPreV2Complete
		&& !bDirtyBefore && !bDirtyAfterPreflight
		&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
		&& Pin->PinType.ContainerType == EPinContainerType::None
		&& !Pin->bDefaultValueIsReadOnly && Pin->LinkedTo.IsEmpty()
		&& Pin->DefaultValue == ExpectedDefault
		&& CanonicalJsonObject(PreTopology) == CanonicalJsonObject(BaselineTopology);
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
	Evidence->SetStringField(TEXT("semantic_topology_v2_version"), TopologyV2Version);
	Evidence->SetStringField(TEXT("pre_semantic_fingerprint_v2"),
		bPreV2Complete ? Sha256(CanonicalJsonObject(PreTopologyV2)) : FString());
	if (!bPreflight)
	{
		SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("STALE_PLAN_OR_TARGET_MISMATCH"), TEXT("FAILED_PRE_MUTATION"), TEXT("Live Unreal preflight did not exactly match the BuildPlan"));
		return Complete(Receipt);
	}
	if (FaultCheckpoint == TEXT("AFTER_PREFLIGHT_BEFORE_MUTATION"))
	{
		Preflight->SetBoolField(TEXT("test_failure_injected"), true);
		Preflight->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		SetFailure(Receipt, TEXT("LIVE_PREFLIGHT"), TEXT("FORCED_AFTER_PREFLIGHT"), TEXT("FAILED_PRE_MUTATION"),
			TEXT("Test-only failure injected after live preflight and before mutation"));
		return Complete(Receipt);
	}

	const FString OriginalDefault = Pin->DefaultValue;
	TSharedPtr<FJsonObject> ExpectedPost = CloneObject(BaselineTopology);
	TSharedPtr<FJsonObject> ExpectedPin = FindSemanticPin(ExpectedPost, NodeGuid, PinId);
	if (!ExpectedPost.IsValid() || !ExpectedPin.IsValid())
	{
		SetFailure(Receipt, TEXT("INTERNAL_CONTRACT"), TEXT("EXPECTED_DELTA_APPLICATION_FAILED"), TEXT("FAILED_PRE_MUTATION"), TEXT("Expected semantic delta could not be applied"));
		return Complete(Receipt);
	}
	ExpectedPin->SetBoolField(TEXT("default_value"), bDesired);

	auto Rollback = [&](const TCHAR* FailurePhase, const FString& FailureCode, bool bPersistedStateUncertain) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> RollbackEvidence = Attempt(true, false);
		const UEdGraphSchema* Schema = Graph->GetSchema();
		const bool bForceRollbackFailure = FaultCheckpoint == TEXT("ROLLBACK_FAILURE");
		if (Schema && !bForceRollbackFailure) Schema->TrySetDefaultValue(*Pin, OriginalDefault);
		TSharedPtr<FJsonObject> RollbackCompile;
		const bool bCompileRestored = !bForceRollbackFailure && Schema && Pin->DefaultValue == OriginalDefault
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
		if (bForceRollbackFailure)
		{
			RollbackEvidence->SetBoolField(TEXT("test_failure_injected"), true);
			RollbackEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		}
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
		return Complete(Receipt, FaultCheckpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT"));
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
	if (FaultCheckpoint == TEXT("AFTER_MUTATION")
		|| FaultCheckpoint == TEXT("ROLLBACK_FAILURE")
		|| FaultCheckpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT"))
	{
		Mutation->SetBoolField(TEXT("test_failure_injected"), true);
		Mutation->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		return Rollback(TEXT("MUTATION"), TEXT("FORCED_FAILURE_AFTER_MUTATION"), false);
	}

	TSharedPtr<FJsonObject> CompileEvidence;
	const bool bForcedCompileFailure = FaultCheckpoint == TEXT("BEFORE_COMPILE");
	const bool bCompiled = bForcedCompileFailure ? false : CompileWithoutSave(Blueprint, CompileEvidence);
	if (bForcedCompileFailure)
	{
		CompileEvidence = Attempt(true, false);
		CompileEvidence->SetNumberField(TEXT("errors"), 1);
		CompileEvidence->SetNumberField(TEXT("warnings"), 0);
		CompileEvidence->SetBoolField(TEXT("save_requested"), false);
		CompileEvidence->SetBoolField(TEXT("test_failure_injected"), true);
		CompileEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
	}
	Evidence->SetObjectField(TEXT("compile"), CompileEvidence);
	if (!bCompiled) return Rollback(TEXT("COMPILE"), TEXT("BLUEPRINT_COMPILE_FAILED"), false);
	if (FaultCheckpoint == TEXT("AFTER_COMPILE_BEFORE_VERIFY"))
	{
		CompileEvidence->SetBoolField(TEXT("test_failure_injected"), true);
		CompileEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		return Rollback(TEXT("VERIFY"), TEXT("FORCED_AFTER_COMPILE_BEFORE_VERIFY"), false);
	}

	bool bPostComplete = false;
	const TSharedPtr<FJsonObject> PostTopology = SemanticTopology(Graph, GraphType, bPostComplete);
	bool bPostV2Complete = false;
	const TSharedPtr<FJsonObject> PostTopologyV2 = SemanticTopologyV2(Graph, GraphType, bPostV2Complete);
	const FString PostFingerprint = bPostComplete ? Sha256(CanonicalJsonObject(PostTopology)) : FString();
	Evidence->SetStringField(TEXT("post_semantic_fingerprint"), PostFingerprint);
	Evidence->SetStringField(TEXT("post_semantic_fingerprint_v2"),
		bPostV2Complete ? Sha256(CanonicalJsonObject(PostTopologyV2)) : FString());
	const bool bForcedVerifyFailure = FaultCheckpoint == TEXT("VERIFICATION_FAILURE");
	const bool bVerified = !bForcedVerifyFailure && bPostComplete && bPostV2Complete
		&& CanonicalJsonObject(PostTopology) == CanonicalJsonObject(ExpectedPost);
	TSharedPtr<FJsonObject> Verification = Attempt(true, bVerified);
	Verification->SetBoolField(TEXT("exact_expected_delta"), bVerified);
	Verification->SetBoolField(TEXT("unchanged_invariants"), bVerified);
	Verification->SetNumberField(TEXT("changed_pin_defaults"), bVerified ? 1 : 0);
	if (bForcedVerifyFailure)
	{
		Verification->SetBoolField(TEXT("test_failure_injected"), true);
		Verification->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
	}
	Evidence->SetObjectField(TEXT("verification"), Verification);
	if (!bVerified) return Rollback(TEXT("VERIFY"), TEXT("EXACT_SEMANTIC_DELTA_FAILED"), false);
	if (FaultCheckpoint == TEXT("AFTER_VERIFY_BEFORE_SAVE"))
	{
		Verification->SetBoolField(TEXT("test_failure_injected"), true);
		Verification->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
		return Rollback(TEXT("SAVE"), TEXT("FORCED_AFTER_VERIFY_BEFORE_SAVE"), false);
	}

	TSharedPtr<FJsonObject> SaveEvidence = Attempt(true, false);
	Evidence->SetObjectField(TEXT("save"), SaveEvidence);
	const bool bForcedSaveFailure = FaultCheckpoint == TEXT("SAVE_FAILURE");
	const bool bSaved = !bForcedSaveFailure && UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false);
	SaveEvidence->SetBoolField(TEXT("succeeded"), bSaved);
	if (bForcedSaveFailure)
	{
		SaveEvidence->SetBoolField(TEXT("test_failure_injected"), true);
		SaveEvidence->SetStringField(TEXT("fault_checkpoint"), FaultCheckpoint);
	}
	if (!bSaved) return Rollback(TEXT("SAVE"), TEXT("BLUEPRINT_SAVE_FAILED"), true);

	bool bPersistedComplete = false;
	const TSharedPtr<FJsonObject> PersistedTopology = SemanticTopology(Graph, GraphType, bPersistedComplete);
	bool bPersistedV2Complete = false;
	const TSharedPtr<FJsonObject> PersistedTopologyV2 = SemanticTopologyV2(Graph, GraphType, bPersistedV2Complete);
	Evidence->SetStringField(TEXT("persisted_semantic_fingerprint_v2"),
		bPersistedV2Complete ? Sha256(CanonicalJsonObject(PersistedTopologyV2)) : FString());
	const bool bPersisted = bPersistedComplete && bPersistedV2Complete && !Package->IsDirty()
		&& CanonicalJsonObject(PersistedTopology) == CanonicalJsonObject(ExpectedPost)
		&& CanonicalJsonObject(PersistedTopologyV2) == CanonicalJsonObject(PostTopologyV2);
	SaveEvidence->SetBoolField(TEXT("persisted_verified"), bPersisted);
	SaveEvidence->SetBoolField(TEXT("package_clean"), !Package->IsDirty());
	DirtyState->SetBoolField(TEXT("final"), Package->IsDirty());
	Evidence->SetObjectField(TEXT("rollback"), Attempt(false, false));
	if (!bPersisted)
	{
		SetFailure(Receipt, TEXT("SAVE"), TEXT("PERSISTED_STATE_UNPROVEN"), TEXT("QUARANTINED"), TEXT("Save returned but persisted clean state could not be proven"));
		return Complete(Receipt);
	}
	Receipt->SetStringField(TEXT("state"), TEXT("SUCCESS"));
	return Complete(Receipt, FaultCheckpoint == TEXT("AFTER_SAVE_BEFORE_FINAL_RECEIPT"));
}
