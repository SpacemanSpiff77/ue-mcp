#include "BlueprintHandlers.h"
#include "BlueprintTopologySerializer.h"
#include "HandlerUtils.h"

#include "Blueprint/BlueprintExtension.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "IPlatformCrypto.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace
{
	constexpr int32 DefaultMaxAuthoredGraphs = 64;
	constexpr int32 DefaultMaxNodesPerGraph = 512;
	constexpr int32 DefaultMaxPinsPerGraph = 4096;
	constexpr int32 DefaultMaxConnectionsPerGraph = 8192;
	constexpr int32 DefaultMaxTotalNodes = 4096;
	constexpr int32 DefaultMaxTotalPins = 32768;
	constexpr int32 DefaultMaxTotalConnections = 65536;
	constexpr int32 DefaultMaxSerializedBytes = 3670016; // 3.5 MiB

	constexpr int32 HardMaxAuthoredGraphs = 256;
	constexpr int32 HardMaxNodesPerGraph = 4096;
	constexpr int32 HardMaxPinsPerGraph = 32768;
	constexpr int32 HardMaxConnectionsPerGraph = 65536;
	constexpr int32 HardMaxTotalNodes = 16384;
	constexpr int32 HardMaxTotalPins = 131072;
	constexpr int32 HardMaxTotalConnections = 262144;
	constexpr int32 HardMaxSerializedBytes = 8388608;
	constexpr int32 MultipartRawChunkBytes = 2 * 1024 * 1024;
	constexpr int32 MultipartCaptureTtlSeconds = 300;
	constexpr int32 MultipartMaxActiveCaptures = 4;
	constexpr int64 MultipartMaxRetainedBytes = 64ll * 1024ll * 1024ll;

	struct FFullTopologyCapture
	{
		FString Handle;
		FString AssetPath;
		FString ObjectPath;
		FString InventoryHash;
		FString InventoryToken;
		FString SnapshotHash;
		FDateTime CreatedAtUtc;
		FDateTime ExpiresAtUtc;
		TArray<uint8> FrozenBytes;
	};

	TMap<FString, FFullTopologyCapture> FullTopologyCaptures;
	int64 FullTopologyRetainedBytes = 0;

	struct FFullTopologyLimits
	{
		int32 MaxAuthoredGraphs = DefaultMaxAuthoredGraphs;
		int32 MaxNodesPerGraph = DefaultMaxNodesPerGraph;
		int32 MaxPinsPerGraph = DefaultMaxPinsPerGraph;
		int32 MaxConnectionsPerGraph = DefaultMaxConnectionsPerGraph;
		int32 MaxTotalNodes = DefaultMaxTotalNodes;
		int32 MaxTotalPins = DefaultMaxTotalPins;
		int32 MaxTotalConnections = DefaultMaxTotalConnections;
		int32 MaxSerializedBytes = DefaultMaxSerializedBytes;
	};

	struct FGraphInventoryRecord
	{
		UEdGraph* Graph = nullptr;
		TSet<FString> MembershipSet;
		TSet<UEdGraph*> ParentGraphs;
		FString Identity;
		FString NormalizedType;
		FString OwnershipKind;
		FString ParentIdentity;
		FString Provenance;
		FString CapturePolicy;
		FString ExclusionReason;
		FString UnsupportedReason;
		int32 NestingDepth = 0;
		int32 NodeCount = 0;
		int32 PinCount = 0;
		int32 ConnectionCount = 0;
		bool bAuthored = false;
		bool bExecutable = false;
		bool bSignatureOnly = false;
		bool bGenerated = false;
		bool bTransient = false;
		bool bSupported = false;
		bool bUnclassified = false;
	};

	FString GuidString(const FGuid& Guid)
	{
		return Guid.IsValid() ? Guid.ToString(EGuidFormats::Digits) : FString();
	}

	FString StableGraphIdentity(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (!Blueprint || !Graph) return FString();
		const FString Guid = GuidString(Graph->GraphGuid);
		return Blueprint->GetPathName()
			+ TEXT("::graph::")
			+ (Guid.IsEmpty() ? TEXT("no-guid") : Guid)
			+ TEXT("::")
			+ Graph->GetPathName();
	}

	bool IsLocallyOwned(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		return Blueprint && Graph && Graph->IsIn(Blueprint);
	}

	bool HasMembership(const FGraphInventoryRecord& Record, const FString& Membership)
	{
		return Record.MembershipSet.Contains(Membership);
	}

	bool HasMembershipPrefix(const FGraphInventoryRecord& Record, const FString& Prefix)
	{
		for (const FString& Membership : Record.MembershipSet)
		{
			if (Membership.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	TArray<FString> SortedMemberships(const FGraphInventoryRecord& Record)
	{
		TArray<FString> Memberships = Record.MembershipSet.Array();
		Memberships.Sort();
		return Memberships;
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	FString GraphTypeName(const UEdGraph* Graph)
	{
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		if (!Schema) return TEXT("unknown");
		switch (Schema->GetGraphType(Graph))
		{
		case GT_Function: return TEXT("function");
		case GT_Ubergraph: return TEXT("ubergraph");
		case GT_Macro: return TEXT("macro");
		case GT_Animation: return TEXT("animation");
		case GT_StateMachine: return TEXT("state-machine");
		default: return TEXT("unknown");
		}
	}

	FString Sha1String(const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		return FSHA1::HashBuffer(Utf8.Get(), Utf8.Length()).ToString().ToLower();
	}

	int32 SerializedByteCount(const TSharedPtr<FJsonObject>& Json)
	{
		if (!Json.IsValid()) return 0;
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		if (!FJsonSerializer::Serialize(Json.ToSharedRef(), Writer))
		{
			return HardMaxSerializedBytes + 1;
		}
		FTCHARToUTF8 Utf8(*Text);
		return Utf8.Length();
	}

	bool SerializeUtf8(const TSharedPtr<FJsonObject>& Json, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		if (!Json.IsValid()) return false;
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		if (!FJsonSerializer::Serialize(Json.ToSharedRef(), Writer)) return false;
		FTCHARToUTF8 Utf8(*Text);
		OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return true;
	}

	FString Sha256Bytes(const TArray<uint8>& Bytes)
	{
		TUniquePtr<FEncryptionContext> EncryptionContext = IPlatformCrypto::Get().CreateContext();
		if (!EncryptionContext.IsValid())
		{
			return FString();
		}

		TArray<uint8> HashBytes;
		if (!EncryptionContext->CalcSHA256(Bytes, HashBytes))
		{
			return FString();
		}

		return BytesToHex(HashBytes.GetData(), HashBytes.Num()).ToLower();
	}

	void CleanupExpiredFullTopologyCaptures()
	{
		const FDateTime Now = FDateTime::UtcNow();
		TArray<FString> ExpiredHandles;
		for (const TPair<FString, FFullTopologyCapture>& Pair : FullTopologyCaptures)
		{
			if (Pair.Value.ExpiresAtUtc <= Now) ExpiredHandles.Add(Pair.Key);
		}
		for (const FString& Handle : ExpiredHandles)
		{
			if (const FFullTopologyCapture* Capture = FullTopologyCaptures.Find(Handle))
			{
				FullTopologyRetainedBytes -= Capture->FrozenBytes.Num();
			}
			FullTopologyCaptures.Remove(Handle);
		}
	}

	FString NewCaptureHandle()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	TSharedPtr<FJsonObject> CaptureManifest(const FFullTopologyCapture& Capture)
	{
		TSharedPtr<FJsonObject> Manifest = MCPSuccess();
		Manifest->SetStringField(TEXT("contractVersion"), TEXT("spacehead.full-blueprint-topology-multipart@1.0"));
		Manifest->SetStringField(TEXT("logicalContractVersion"), TEXT("spacehead.full-blueprint-topology@1.0"));
		Manifest->SetStringField(TEXT("delivery"), TEXT("multipart"));
		Manifest->SetStringField(TEXT("status"), TEXT("capture-ready"));
		Manifest->SetStringField(TEXT("captureHandle"), Capture.Handle);
		Manifest->SetStringField(TEXT("assetPath"), Capture.AssetPath);
		Manifest->SetStringField(TEXT("objectPath"), Capture.ObjectPath);
		Manifest->SetNumberField(TEXT("totalBytes"), Capture.FrozenBytes.Num());
		Manifest->SetNumberField(TEXT("chunkCount"),
			FMath::DivideAndRoundUp(Capture.FrozenBytes.Num(), MultipartRawChunkBytes));
		Manifest->SetNumberField(TEXT("rawChunkSizeBytes"), MultipartRawChunkBytes);
		Manifest->SetStringField(TEXT("encoding"), TEXT("base64"));
		Manifest->SetStringField(TEXT("snapshotHash"), Capture.SnapshotHash);
		Manifest->SetStringField(TEXT("snapshotHashAlgorithm"), TEXT("sha256"));
		Manifest->SetStringField(TEXT("inventoryHash"), Capture.InventoryHash);
		Manifest->SetStringField(TEXT("inventoryToken"), Capture.InventoryToken);
		Manifest->SetStringField(TEXT("createdAtUtc"), Capture.CreatedAtUtc.ToIso8601());
		Manifest->SetStringField(TEXT("expiresAtUtc"), Capture.ExpiresAtUtc.ToIso8601());
		Manifest->SetNumberField(TEXT("ttlSeconds"), MultipartCaptureTtlSeconds);
		Manifest->SetBoolField(TEXT("unrealAccessedForChunks"), false);
		Manifest->SetBoolField(TEXT("mutationOperationsPerformed"), false);
		return Manifest;
	}

	void CountGraph(FGraphInventoryRecord& Record)
	{
		for (UEdGraphNode* Node : Record.Graph->Nodes)
		{
			if (!Node) continue;
			++Record.NodeCount;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				++Record.PinCount;
				if (Pin->Direction == EGPD_Output)
				{
					Record.ConnectionCount += Pin->LinkedTo.Num();
				}
			}
		}
	}

	TSharedPtr<FJsonObject> LimitsJson(const FFullTopologyLimits& Limits)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("maxAuthoredGraphs"), Limits.MaxAuthoredGraphs);
		Json->SetNumberField(TEXT("maxNodesPerGraph"), Limits.MaxNodesPerGraph);
		Json->SetNumberField(TEXT("maxPinsPerGraph"), Limits.MaxPinsPerGraph);
		Json->SetNumberField(TEXT("maxConnectionsPerGraph"), Limits.MaxConnectionsPerGraph);
		Json->SetNumberField(TEXT("maxTotalNodes"), Limits.MaxTotalNodes);
		Json->SetNumberField(TEXT("maxTotalPins"), Limits.MaxTotalPins);
		Json->SetNumberField(TEXT("maxTotalConnections"), Limits.MaxTotalConnections);
		Json->SetNumberField(TEXT("maxSerializedBytes"), Limits.MaxSerializedBytes);
		Json->SetNumberField(TEXT("hardMaxAuthoredGraphs"), HardMaxAuthoredGraphs);
		Json->SetNumberField(TEXT("hardMaxNodesPerGraph"), HardMaxNodesPerGraph);
		Json->SetNumberField(TEXT("hardMaxPinsPerGraph"), HardMaxPinsPerGraph);
		Json->SetNumberField(TEXT("hardMaxConnectionsPerGraph"), HardMaxConnectionsPerGraph);
		Json->SetNumberField(TEXT("hardMaxTotalNodes"), HardMaxTotalNodes);
		Json->SetNumberField(TEXT("hardMaxTotalPins"), HardMaxTotalPins);
		Json->SetNumberField(TEXT("hardMaxTotalConnections"), HardMaxTotalConnections);
		Json->SetNumberField(TEXT("hardMaxSerializedBytes"), HardMaxSerializedBytes);
		return Json;
	}

	void FinishDirtyState(TSharedPtr<FJsonObject>& Result, UPackage* Package, bool bDirtyBefore)
	{
		const bool bDirtyAfter = Package && Package->IsDirty();
		Result->SetBoolField(TEXT("dirtyAfter"), bDirtyAfter);
		Result->SetBoolField(TEXT("dirtyStateChanged"), bDirtyBefore != bDirtyAfter);
		Result->SetBoolField(TEXT("mutationGuardPassed"), bDirtyBefore == bDirtyAfter);
	}
}

TSharedPtr<FJsonValue> FBlueprintHandlers::ReadBlueprintTopology(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Error = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Error;

	FFullTopologyLimits Limits;
	Limits.MaxAuthoredGraphs = FMath::Clamp(
		OptionalInt(Params, TEXT("maxAuthoredGraphs"), DefaultMaxAuthoredGraphs), 1, HardMaxAuthoredGraphs);
	Limits.MaxNodesPerGraph = FMath::Clamp(
		OptionalInt(Params, TEXT("maxNodesPerGraph"), DefaultMaxNodesPerGraph), 1, HardMaxNodesPerGraph);
	Limits.MaxPinsPerGraph = FMath::Clamp(
		OptionalInt(Params, TEXT("maxPinsPerGraph"), DefaultMaxPinsPerGraph), 1, HardMaxPinsPerGraph);
	Limits.MaxConnectionsPerGraph = FMath::Clamp(
		OptionalInt(Params, TEXT("maxConnectionsPerGraph"), DefaultMaxConnectionsPerGraph), 1, HardMaxConnectionsPerGraph);
	Limits.MaxTotalNodes = FMath::Clamp(
		OptionalInt(Params, TEXT("maxTotalNodes"), DefaultMaxTotalNodes), 1, HardMaxTotalNodes);
	Limits.MaxTotalPins = FMath::Clamp(
		OptionalInt(Params, TEXT("maxTotalPins"), DefaultMaxTotalPins), 1, HardMaxTotalPins);
	Limits.MaxTotalConnections = FMath::Clamp(
		OptionalInt(Params, TEXT("maxTotalConnections"), DefaultMaxTotalConnections), 1, HardMaxTotalConnections);
	Limits.MaxSerializedBytes = FMath::Clamp(
		OptionalInt(Params, TEXT("maxSerializedBytes"), DefaultMaxSerializedBytes), 262144, HardMaxSerializedBytes);

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MCPError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UPackage* Package = Blueprint->GetOutermost();
	const bool bDirtyBefore = Package && Package->IsDirty();
	const FString BlueprintObjectPath = Blueprint->GetPathName();

	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetStringField(TEXT("contractVersion"), TEXT("spacehead.full-blueprint-topology@1.0"));
	Result->SetStringField(TEXT("status"), TEXT("inventory"));
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("objectPath"), BlueprintObjectPath);
	Result->SetStringField(TEXT("assetClass"), Blueprint->GetClass()->GetPathName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
	Result->SetStringField(TEXT("scanScope"), TEXT("full-blueprint"));
	Result->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
	Result->SetBoolField(TEXT("compileRequested"), false);
	Result->SetBoolField(TEXT("saveRequested"), false);
	Result->SetBoolField(TEXT("reconstructRequested"), false);
	Result->SetBoolField(TEXT("mutationOperationsPerformed"), false);
	Result->SetBoolField(TEXT("allOrNothing"), true);
	Result->SetObjectField(TEXT("limits"), LimitsJson(Limits));

	TSharedPtr<FJsonObject> Provider = MakeShared<FJsonObject>();
	Provider->SetStringField(TEXT("name"), TEXT("UE_MCP_Bridge"));
	Provider->SetStringField(TEXT("version"), TEXT("0.3.0"));
	Provider->SetStringField(TEXT("tool"), TEXT("blueprint.read_blueprint_topology"));
	Provider->SetStringField(TEXT("nativeMethod"), TEXT("read_blueprint_topology"));
	Provider->SetStringField(TEXT("toolVersion"), TEXT("1.0.0"));
	Provider->SetStringField(TEXT("serializer"), TEXT("qualified-exact-topology"));
	Result->SetObjectField(TEXT("provider"), Provider);

	const TArray<FString> CollectionsInspected = {
		TEXT("FunctionGraphs"),
		TEXT("UbergraphPages"),
		TEXT("MacroGraphs"),
		TEXT("DelegateSignatureGraphs"),
		TEXT("ImplementedInterfaces.Graphs"),
		TEXT("BlueprintExtensions.GetAllGraphs"),
		TEXT("RecursiveSubGraphs"),
		TEXT("UserConstructionScript"),
		TEXT("IntermediateGeneratedGraphs"),
		TEXT("EventGraphs"),
		TEXT("GetAllGraphs"),
		TEXT("OwnedUEdGraphAudit")
	};
	Result->SetArrayField(TEXT("collectionsInspected"), StringValues(CollectionsInspected));

	TMap<UEdGraph*, FGraphInventoryRecord> Records;
	TSet<UEdGraph*> ForeignGraphReferences;
	int32 NullGraphReferenceCount = 0;
	int32 DuplicateMembershipCount = 0;

	auto AddGraph = [&](UEdGraph* Graph, const FString& Membership, int32 Depth, UEdGraph* Parent)
	{
		if (!Graph)
		{
			++NullGraphReferenceCount;
			return;
		}
		if (!IsLocallyOwned(Blueprint, Graph))
		{
			ForeignGraphReferences.Add(Graph);
			return;
		}
		FGraphInventoryRecord& Record = Records.FindOrAdd(Graph);
		Record.Graph = Graph;
		if (Record.MembershipSet.Contains(Membership))
		{
			++DuplicateMembershipCount;
		}
		Record.MembershipSet.Add(Membership);
		if (Parent)
		{
			Record.ParentGraphs.Add(Parent);
			if (Record.NestingDepth == 0 || Depth < Record.NestingDepth)
			{
				Record.NestingDepth = Depth;
			}
		}
	};

	for (UEdGraph* Graph : Blueprint->FunctionGraphs) AddGraph(Graph, TEXT("FunctionGraphs"), 0, nullptr);
	for (UEdGraph* Graph : Blueprint->UbergraphPages) AddGraph(Graph, TEXT("UbergraphPages"), 0, nullptr);
	for (UEdGraph* Graph : Blueprint->MacroGraphs) AddGraph(Graph, TEXT("MacroGraphs"), 0, nullptr);
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) AddGraph(Graph, TEXT("DelegateSignatureGraphs"), 0, nullptr);
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		const FString InterfacePath = Interface.Interface ? Interface.Interface->GetPathName() : TEXT("unknown-interface");
		for (UEdGraph* Graph : Interface.Graphs)
		{
			AddGraph(Graph, TEXT("ImplementedInterfaces.Graphs:") + InterfacePath, 0, nullptr);
		}
	}
	for (const UBlueprintExtension* Extension : Blueprint->GetExtensions())
	{
		if (!Extension) continue;
		TArray<UEdGraph*> ExtensionGraphs;
		Extension->GetAllGraphs(ExtensionGraphs);
		const FString ExtensionIdentity = Extension->GetClass()->GetPathName() + TEXT(":") + Extension->GetPathName();
		for (UEdGraph* Graph : ExtensionGraphs)
		{
			AddGraph(Graph, TEXT("BlueprintExtensions.GetAllGraphs:") + ExtensionIdentity, 0, nullptr);
		}
	}
	if (UEdGraph* ConstructionScript = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint))
	{
		AddGraph(ConstructionScript, TEXT("UserConstructionScript"), 0, nullptr);
	}
	for (UEdGraph* Graph : Blueprint->IntermediateGeneratedGraphs)
	{
		AddGraph(Graph, TEXT("IntermediateGeneratedGraphs"), 0, nullptr);
	}
	for (UEdGraph* Graph : Blueprint->EventGraphs)
	{
		AddGraph(Graph, TEXT("EventGraphs"), 0, nullptr);
	}

	TArray<UEdGraph*> GetAllGraphs;
	Blueprint->GetAllGraphs(GetAllGraphs);
	for (UEdGraph* Graph : GetAllGraphs)
	{
		AddGraph(Graph, TEXT("GetAllGraphs"), 0, nullptr);
	}

	TArray<UObject*> OwnedObjects;
	GetObjectsWithOuter(Blueprint, OwnedObjects, EGetObjectsFlags::IncludeNestedObjects);
	int32 OwnershipAuditGraphCount = 0;
	for (UObject* Object : OwnedObjects)
	{
		if (UEdGraph* Graph = Cast<UEdGraph>(Object))
		{
			++OwnershipAuditGraphCount;
			AddGraph(Graph, TEXT("OwnedUEdGraphAudit"), 0, nullptr);
		}
	}

	TSet<UEdGraph*> ExpandedGraphs;
	TFunction<void(UEdGraph*, int32)> AddSubGraphs = [&](UEdGraph* Parent, int32 ParentDepth)
	{
		if (!Parent || ExpandedGraphs.Contains(Parent)) return;
		ExpandedGraphs.Add(Parent);
		for (UEdGraph* Child : Parent->SubGraphs)
		{
			AddGraph(Child, TEXT("RecursiveSubGraphs:") + Parent->GetPathName(), ParentDepth + 1, Parent);
			AddSubGraphs(Child, ParentDepth + 1);
		}
	};
	TArray<UEdGraph*> InventorySeeds;
	Records.GetKeys(InventorySeeds);
	for (UEdGraph* Graph : InventorySeeds)
	{
		AddSubGraphs(Graph, Records.FindChecked(Graph).NestingDepth);
	}

	TArray<FGraphInventoryRecord*> SortedRecords;
	SortedRecords.Reserve(Records.Num());
	for (TPair<UEdGraph*, FGraphInventoryRecord>& Pair : Records)
	{
		FGraphInventoryRecord& Record = Pair.Value;
		Record.Identity = StableGraphIdentity(Blueprint, Record.Graph);

		if (Record.ParentGraphs.IsEmpty())
		{
			for (UObject* Outer = Record.Graph->GetOuter(); Outer && Outer != Blueprint; Outer = Outer->GetOuter())
			{
				if (UEdGraph* ParentGraph = Cast<UEdGraph>(Outer))
				{
					Record.ParentGraphs.Add(ParentGraph);
					break;
				}
			}
		}
		if (!Record.ParentGraphs.IsEmpty())
		{
			TArray<FString> ParentIdentities;
			for (UEdGraph* Parent : Record.ParentGraphs)
			{
				ParentIdentities.Add(StableGraphIdentity(Blueprint, Parent));
			}
			ParentIdentities.Sort();
			Record.ParentIdentity = ParentIdentities[0];
			if (Record.NestingDepth == 0) Record.NestingDepth = 1;
		}

		Record.OwnershipKind = TEXT("blueprint-owned-object");
		for (UObject* Outer = Record.Graph->GetOuter(); Outer && Outer != Blueprint; Outer = Outer->GetOuter())
		{
			if (Outer->IsA<UBlueprintExtension>())
			{
				Record.OwnershipKind = TEXT("blueprint-extension");
				break;
			}
		}
		if (!Record.ParentIdentity.IsEmpty())
		{
			Record.OwnershipKind = TEXT("nested-blueprint-graph");
		}
		else if (Record.Graph->GetOuter() == Blueprint)
		{
			Record.OwnershipKind = TEXT("blueprint");
		}

		Record.bGenerated = HasMembership(Record, TEXT("IntermediateGeneratedGraphs"));
		Record.bTransient = HasMembership(Record, TEXT("EventGraphs"))
			|| Record.Graph->HasAnyFlags(RF_Transient)
			|| Record.Graph->GetOutermost() == GetTransientPackage();
		Record.bSignatureOnly = HasMembership(Record, TEXT("DelegateSignatureGraphs"));
		const bool bConstructionScript = HasMembership(Record, TEXT("UserConstructionScript"))
			|| UEdGraphSchema_K2::IsConstructionScript(Record.Graph);
		const bool bInterface = HasMembershipPrefix(Record, TEXT("ImplementedInterfaces.Graphs:"));
		const bool bMacro = HasMembership(Record, TEXT("MacroGraphs"));
		const bool bUbergraph = HasMembership(Record, TEXT("UbergraphPages"));
		const bool bFunction = HasMembership(Record, TEXT("FunctionGraphs"));
		const bool bNested = HasMembershipPrefix(Record, TEXT("RecursiveSubGraphs:"));
		const bool bExtension = HasMembershipPrefix(Record, TEXT("BlueprintExtensions.GetAllGraphs:"));
		const bool bKnownAuthored = bConstructionScript || bInterface || bMacro || bUbergraph
			|| bFunction || bNested || bExtension || Record.bSignatureOnly;
		// The qualified serializer is intentionally limited to the standard K2
		// schema. AnimGraph and other schema subclasses may inherit K2 mechanics
		// but require their own topology and completeness rules.
		const bool bK2Schema = Record.Graph->GetSchema()
			&& Record.Graph->GetSchema()->GetClass() == UEdGraphSchema_K2::StaticClass();

		if (Record.bGenerated)
		{
			Record.NormalizedType = TEXT("generated");
			Record.CapturePolicy = TEXT("exclude-generated");
			Record.ExclusionReason = TEXT("compiler intermediate generated graph");
		}
		else if (Record.bTransient)
		{
			Record.NormalizedType = TEXT("transient");
			Record.CapturePolicy = TEXT("exclude-transient");
			Record.ExclusionReason = TEXT("transient compiler graph");
		}
		else if (Record.bSignatureOnly)
		{
			Record.NormalizedType = TEXT("delegate-signature");
			Record.CapturePolicy = TEXT("inventory-signature-only");
			Record.ExclusionReason = TEXT("signature-only graph has no executable topology requirement");
			Record.bAuthored = true;
		}
		else if (!bKnownAuthored)
		{
			Record.NormalizedType = TEXT("unclassified-owned");
			Record.CapturePolicy = TEXT("unclassified-owned");
			Record.UnsupportedReason = TEXT("owned UEdGraph is absent from every known UE 5.8 Blueprint graph collection");
			Record.bUnclassified = true;
		}
		else
		{
			if (bConstructionScript) Record.NormalizedType = TEXT("construction-script");
			else if (bInterface) Record.NormalizedType = TEXT("interface-implementation");
			else if (bMacro) Record.NormalizedType = TEXT("macro");
			else if (bUbergraph) Record.NormalizedType = TEXT("event-graph");
			else if (bFunction) Record.NormalizedType = TEXT("function");
			else if (bNested) Record.NormalizedType = TEXT("nested-authored");
			else Record.NormalizedType = TEXT("extension-authored");

			Record.bAuthored = true;
			if (bK2Schema)
			{
				Record.bSupported = true;
				Record.bExecutable = true;
				Record.CapturePolicy = TEXT("capture-topology");
			}
			else
			{
				Record.CapturePolicy = TEXT("unsupported-authored");
				Record.UnsupportedReason = Record.Graph->GetSchema()
					? TEXT("authored graph schema is not supported by the qualified K2 topology serializer")
					: TEXT("authored graph has no schema");
			}
		}

		CountGraph(Record);
		const TArray<FString> Memberships = SortedMemberships(Record);
		Record.Provenance = FString::Join(Memberships, TEXT("|"));
		SortedRecords.Add(&Record);
	}
	SortedRecords.Sort([](const FGraphInventoryRecord& A, const FGraphInventoryRecord& B)
	{
		return A.Identity < B.Identity;
	});

	int32 AuthoredGraphCount = 0;
	int32 CapturedGraphCount = 0;
	int32 SignatureOnlyGraphCount = 0;
	int32 ExcludedGraphCount = 0;
	int32 UnsupportedGraphCount = 0;
	int32 UnclassifiedGraphCount = 0;
	int32 TotalNodeCount = 0;
	int32 TotalPinCount = 0;
	int32 TotalConnectionCount = 0;
	int32 GetAllOnlyCount = 0;
	int32 OwnershipAuditOnlyCount = 0;
	TArray<TSharedPtr<FJsonValue>> InventoryJson;
	FString CanonicalInventory;

	for (const FGraphInventoryRecord* Record : SortedRecords)
	{
		const TArray<FString> Memberships = SortedMemberships(*Record);
		if (Record->bAuthored) ++AuthoredGraphCount;
		if (Record->CapturePolicy == TEXT("capture-topology"))
		{
			++CapturedGraphCount;
			TotalNodeCount += Record->NodeCount;
			TotalPinCount += Record->PinCount;
			TotalConnectionCount += Record->ConnectionCount;
		}
		if (Record->bSignatureOnly) ++SignatureOnlyGraphCount;
		if (Record->bGenerated || Record->bTransient) ++ExcludedGraphCount;
		if (Record->CapturePolicy == TEXT("unsupported-authored")) ++UnsupportedGraphCount;
		if (Record->bUnclassified) ++UnclassifiedGraphCount;
		if (HasMembership(*Record, TEXT("GetAllGraphs"))
			&& !HasMembership(*Record, TEXT("OwnedUEdGraphAudit"))) ++GetAllOnlyCount;
		if (!HasMembership(*Record, TEXT("GetAllGraphs"))
			&& HasMembership(*Record, TEXT("OwnedUEdGraphAudit"))) ++OwnershipAuditOnlyCount;

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		const FString GraphGuid = GuidString(Record->Graph->GraphGuid);
		Item->SetStringField(TEXT("graphIdentity"), Record->Identity);
		Item->SetStringField(TEXT("name"), Record->Graph->GetName());
		Item->SetStringField(TEXT("objectPath"), Record->Graph->GetPathName());
		Item->SetStringField(TEXT("graphGuid"), GraphGuid);
		Item->SetBoolField(TEXT("graphGuidAvailable"), !GraphGuid.IsEmpty());
		Item->SetStringField(TEXT("graphClass"), Record->Graph->GetClass()->GetPathName());
		Item->SetStringField(TEXT("schemaClass"),
			Record->Graph->GetSchema() ? Record->Graph->GetSchema()->GetClass()->GetPathName() : FString());
		Item->SetStringField(TEXT("nativeGraphType"), GraphTypeName(Record->Graph));
		Item->SetStringField(TEXT("normalizedGraphType"), Record->NormalizedType);
		Item->SetArrayField(TEXT("collectionMemberships"), StringValues(Memberships));
		Item->SetStringField(TEXT("ownershipKind"), Record->OwnershipKind);
		Item->SetStringField(TEXT("parentGraphIdentity"), Record->ParentIdentity);
		Item->SetNumberField(TEXT("nestingDepth"), Record->NestingDepth);
		Item->SetBoolField(TEXT("authored"), Record->bAuthored);
		Item->SetBoolField(TEXT("executable"), Record->bExecutable);
		Item->SetBoolField(TEXT("signatureOnly"), Record->bSignatureOnly);
		Item->SetBoolField(TEXT("generated"), Record->bGenerated);
		Item->SetBoolField(TEXT("transient"), Record->bTransient);
		Item->SetStringField(TEXT("capturePolicy"), Record->CapturePolicy);
		Item->SetBoolField(TEXT("supported"), Record->bSupported);
		Item->SetStringField(TEXT("exclusionReason"), Record->ExclusionReason);
		Item->SetStringField(TEXT("unsupportedReason"), Record->UnsupportedReason);
		Item->SetNumberField(TEXT("nodeCount"), Record->NodeCount);
		Item->SetNumberField(TEXT("pinCount"), Record->PinCount);
		Item->SetNumberField(TEXT("connectionCount"), Record->ConnectionCount);
		InventoryJson.Add(MakeShared<FJsonValueObject>(Item));

		CanonicalInventory += Record->Identity + TEXT("\n")
			+ Record->NormalizedType + TEXT("\n")
			+ Record->OwnershipKind + TEXT("\n")
			+ Record->ParentIdentity + TEXT("\n")
			+ FString::FromInt(Record->NestingDepth) + TEXT("\n")
			+ Record->CapturePolicy + TEXT("\n")
			+ FString::Join(Memberships, TEXT(",")) + TEXT("\n")
			+ FString::FromInt(Record->NodeCount) + TEXT(":")
			+ FString::FromInt(Record->PinCount) + TEXT(":")
			+ FString::FromInt(Record->ConnectionCount) + TEXT("\n");
	}

	const FString InventoryHash = Sha1String(CanonicalInventory);
	Result->SetArrayField(TEXT("graphInventory"), InventoryJson);
	Result->SetStringField(TEXT("inventoryHash"), InventoryHash);
	Result->SetStringField(TEXT("inventoryToken"), TEXT("sha1:") + InventoryHash);
	Result->SetNumberField(TEXT("authoredGraphCount"), AuthoredGraphCount);
	Result->SetNumberField(TEXT("capturedGraphCount"), CapturedGraphCount);
	Result->SetNumberField(TEXT("signatureOnlyGraphCount"), SignatureOnlyGraphCount);
	Result->SetNumberField(TEXT("excludedGraphCount"), ExcludedGraphCount);
	Result->SetNumberField(TEXT("unsupportedGraphCount"), UnsupportedGraphCount);
	Result->SetNumberField(TEXT("unclassifiedGraphCount"), UnclassifiedGraphCount);
	Result->SetNumberField(TEXT("totalGraphCount"), SortedRecords.Num());
	Result->SetNumberField(TEXT("totalNodeCount"), TotalNodeCount);
	Result->SetNumberField(TEXT("totalPinCount"), TotalPinCount);
	Result->SetNumberField(TEXT("totalConnectionCount"), TotalConnectionCount);

	TSharedPtr<FJsonObject> InventorySource = MakeShared<FJsonObject>();
	InventorySource->SetStringField(TEXT("strategy"), TEXT("pointer-deduplicated union with explicit membership evidence"));
	InventorySource->SetNumberField(TEXT("getAllGraphsReferenceCount"), GetAllGraphs.Num());
	InventorySource->SetNumberField(TEXT("ownershipAuditGraphCount"), OwnershipAuditGraphCount);
	InventorySource->SetNumberField(TEXT("unionGraphCount"), SortedRecords.Num());
	InventorySource->SetNumberField(TEXT("getAllOnlyCount"), GetAllOnlyCount);
	InventorySource->SetNumberField(TEXT("ownershipAuditOnlyCount"), OwnershipAuditOnlyCount);
	InventorySource->SetNumberField(TEXT("duplicateMembershipCount"), DuplicateMembershipCount);
	InventorySource->SetNumberField(TEXT("foreignGraphReferenceCount"), ForeignGraphReferences.Num());
	InventorySource->SetNumberField(TEXT("inheritedGraphExclusionCount"), ForeignGraphReferences.Num());
	InventorySource->SetNumberField(TEXT("nullGraphReferenceCount"), NullGraphReferenceCount);
	InventorySource->SetBoolField(TEXT("pointerDeduplicated"), true);
	InventorySource->SetBoolField(TEXT("unpaginated"), true);
	InventorySource->SetBoolField(TEXT("untruncated"), true);

	bool bGetAllGraphsReconciled = true;
	for (UEdGraph* Graph : GetAllGraphs)
	{
		if (IsLocallyOwned(Blueprint, Graph) && !Records.Contains(Graph))
		{
			bGetAllGraphsReconciled = false;
			break;
		}
	}
	bool bOwnershipAuditReconciled = true;
	for (UObject* Object : OwnedObjects)
	{
		if (UEdGraph* Graph = Cast<UEdGraph>(Object))
		{
			if (!Records.Contains(Graph))
			{
				bOwnershipAuditReconciled = false;
				break;
			}
		}
	}
	bool bClassificationComplete = true;
	for (const FGraphInventoryRecord* Record : SortedRecords)
	{
		if (Record->Identity.IsEmpty() || Record->NormalizedType.IsEmpty() || Record->CapturePolicy.IsEmpty())
		{
			bClassificationComplete = false;
			break;
		}
	}
	InventorySource->SetBoolField(TEXT("getAllGraphsReconciled"), bGetAllGraphsReconciled);
	InventorySource->SetBoolField(TEXT("ownershipAuditReconciled"), bOwnershipAuditReconciled);
	InventorySource->SetBoolField(TEXT("classificationComplete"), bClassificationComplete);
	Result->SetObjectField(TEXT("inventorySource"), InventorySource);

	const bool bGraphInventoryComplete = CollectionsInspected.Num() == 12
		&& SortedRecords.Num() == Records.Num()
		&& bGetAllGraphsReconciled
		&& bOwnershipAuditReconciled
		&& bClassificationComplete;
	Result->SetBoolField(TEXT("graphInventoryComplete"), bGraphInventoryComplete);

	TArray<FString> BoundViolations;
	if (AuthoredGraphCount > Limits.MaxAuthoredGraphs)
	{
		BoundViolations.Add(FString::Printf(TEXT("authored graphs %d exceed %d"),
			AuthoredGraphCount, Limits.MaxAuthoredGraphs));
	}
	for (const FGraphInventoryRecord* Record : SortedRecords)
	{
		if (Record->CapturePolicy != TEXT("capture-topology")) continue;
		if (Record->NodeCount > Limits.MaxNodesPerGraph)
			BoundViolations.Add(Record->Identity + TEXT(": node bound exceeded"));
		if (Record->PinCount > Limits.MaxPinsPerGraph)
			BoundViolations.Add(Record->Identity + TEXT(": pin bound exceeded"));
		if (Record->ConnectionCount > Limits.MaxConnectionsPerGraph)
			BoundViolations.Add(Record->Identity + TEXT(": connection bound exceeded"));
	}
	if (TotalNodeCount > Limits.MaxTotalNodes)
		BoundViolations.Add(TEXT("whole-Blueprint node bound exceeded"));
	if (TotalPinCount > Limits.MaxTotalPins)
		BoundViolations.Add(TEXT("whole-Blueprint pin bound exceeded"));
	if (TotalConnectionCount > Limits.MaxTotalConnections)
		BoundViolations.Add(TEXT("whole-Blueprint connection bound exceeded"));
	BoundViolations.Sort();

	TArray<TSharedPtr<FJsonValue>> GraphsJson;
	int32 UnresolvedEndpointCount = 0;
	int32 DuplicatePinIdentityCount = 0;
	int32 SerializedNodeCount = 0;
	int32 SerializedPinCount = 0;
	int32 SerializedConnectionCount = 0;
	if (BoundViolations.IsEmpty())
	{
		for (const FGraphInventoryRecord* Record : SortedRecords)
		{
			if (Record->CapturePolicy != TEXT("capture-topology")) continue;
			UE_MCP_BlueprintTopology::FGraphSerializationOptions Options;
			Options.GraphIdentity = Record->Identity;
			Options.NormalizedGraphType = Record->NormalizedType;
			Options.CollectionMemberships = SortedMemberships(*Record);
			Options.OwnershipKind = Record->OwnershipKind;
			Options.ParentGraphIdentity = Record->ParentIdentity;
			Options.NestingDepth = Record->NestingDepth;
			Options.GraphProvenance = Record->Provenance;
			Options.bIncludeFullBlueprintMetadata = true;
			Options.bRejectDuplicateNodeScopedPinIdentity = true;
			const UE_MCP_BlueprintTopology::FSerializedGraphTopology Serialized =
				UE_MCP_BlueprintTopology::SerializeGraph(Record->Graph, Options);
			GraphsJson.Add(MakeShared<FJsonValueObject>(Serialized.Graph));
			UnresolvedEndpointCount += Serialized.UnresolvedEndpointCount;
			DuplicatePinIdentityCount += Serialized.DuplicatePinIdentityCount;
			SerializedNodeCount += Serialized.NodeCount;
			SerializedPinCount += Serialized.PinCount;
			SerializedConnectionCount += Serialized.ConnectionCount;
		}
	}

	Result->SetArrayField(TEXT("graphs"), GraphsJson);
	Result->SetNumberField(TEXT("unresolvedEndpointCount"), UnresolvedEndpointCount);
	Result->SetNumberField(TEXT("duplicatePinIdentityCount"), DuplicatePinIdentityCount);
	Result->SetNumberField(TEXT("serializedNodeCount"), SerializedNodeCount);
	Result->SetNumberField(TEXT("serializedPinCount"), SerializedPinCount);
	Result->SetNumberField(TEXT("serializedConnectionCount"), SerializedConnectionCount);
	const bool bNonByteBoundViolation = !BoundViolations.IsEmpty();
	if (bNonByteBoundViolation) Result->SetArrayField(TEXT("graphs"), {});
	Result->SetBoolField(TEXT("truncated"), bNonByteBoundViolation);
	Result->SetBoolField(TEXT("dataOmitted"), bNonByteBoundViolation);
	Result->SetNumberField(TEXT("serializedPayloadBytes"), 0);

	TSharedPtr<FJsonObject> Omission = MakeShared<FJsonObject>();
	Omission->SetBoolField(TEXT("omitted"), bNonByteBoundViolation);
	Omission->SetBoolField(TEXT("allOrNothing"), true);
	Omission->SetStringField(TEXT("reason"), bNonByteBoundViolation
		? TEXT("one or more non-byte full-Blueprint topology bounds were exceeded; no graph topology was returned")
		: FString());
	Omission->SetArrayField(TEXT("violations"), StringValues(BoundViolations));
	Omission->SetNumberField(TEXT("omittedGraphCount"), bNonByteBoundViolation ? CapturedGraphCount : 0);
	Omission->SetNumberField(TEXT("omittedNodeCount"), bNonByteBoundViolation ? TotalNodeCount : 0);
	Omission->SetNumberField(TEXT("omittedPinCount"), bNonByteBoundViolation ? TotalPinCount : 0);
	Omission->SetNumberField(TEXT("omittedConnectionCount"), bNonByteBoundViolation ? TotalConnectionCount : 0);
	Result->SetObjectField(TEXT("omission"), Omission);

	FinishDirtyState(Result, Package, bDirtyBefore);
	const bool bCountReconciled = SerializedNodeCount == TotalNodeCount
		&& SerializedPinCount == TotalPinCount
		&& SerializedConnectionCount + UnresolvedEndpointCount == TotalConnectionCount;
	const bool bCapturedSetReconciled = !bNonByteBoundViolation && GraphsJson.Num() == CapturedGraphCount;
	const bool bTopologyComplete = bGraphInventoryComplete
		&& bCapturedSetReconciled
		&& bCountReconciled
		&& UnsupportedGraphCount == 0
		&& UnclassifiedGraphCount == 0
		&& UnresolvedEndpointCount == 0
		&& DuplicatePinIdentityCount == 0
		&& !bNonByteBoundViolation
		&& !Result->GetBoolField(TEXT("dirtyStateChanged"));
	Result->SetBoolField(TEXT("topologyComplete"), bTopologyComplete);
	Result->SetBoolField(TEXT("complete"), bTopologyComplete);
	Result->SetBoolField(TEXT("countsReconciled"), bCountReconciled);
	Result->SetBoolField(TEXT("capturedSetReconciled"), bCapturedSetReconciled);
	Result->SetStringField(TEXT("status"), bNonByteBoundViolation
		? TEXT("omitted") : (bTopologyComplete ? TEXT("exact") : TEXT("partial")));

	if (bNonByteBoundViolation)
	{
		return MCPResult(Result);
	}

	// Stabilize the self-reported byte count, then freeze the exact canonical
	// logical result. Multipart delivery changes only transport; the bytes decode
	// to the same spacehead.full-blueprint-topology@1.0 payload as inline delivery.
	TArray<uint8> FrozenBytes;
	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		if (!SerializeUtf8(Result, FrozenBytes)) return MCPError(TEXT("Failed to serialize Full Blueprint topology"));
		const int32 PriorSize = static_cast<int32>(Result->GetNumberField(TEXT("serializedPayloadBytes")));
		if (PriorSize == FrozenBytes.Num()) break;
		Result->SetNumberField(TEXT("serializedPayloadBytes"), FrozenBytes.Num());
	}
	if (!SerializeUtf8(Result, FrozenBytes)) return MCPError(TEXT("Failed to freeze Full Blueprint topology"));
	Result->SetNumberField(TEXT("serializedPayloadBytes"), FrozenBytes.Num());
	TArray<uint8> VerifiedFrozenBytes;
	if (!SerializeUtf8(Result, VerifiedFrozenBytes)) return MCPError(TEXT("Failed to verify Full Blueprint topology size"));
	if (VerifiedFrozenBytes.Num() != FrozenBytes.Num())
	{
		FrozenBytes = MoveTemp(VerifiedFrozenBytes);
		Result->SetNumberField(TEXT("serializedPayloadBytes"), FrozenBytes.Num());
		if (!SerializeUtf8(Result, FrozenBytes)) return MCPError(TEXT("Failed to finalize Full Blueprint topology size"));
	}

	if (FrozenBytes.Num() <= Limits.MaxSerializedBytes)
	{
		return MCPResult(Result);
	}

	CleanupExpiredFullTopologyCaptures();
	if (FrozenBytes.Num() > MultipartMaxRetainedBytes)
	{
		return MCPError(FString::Printf(TEXT("Full Blueprint capture-capacity-exceeded: %d bytes exceed retained-byte quota %lld"),
			FrozenBytes.Num(), MultipartMaxRetainedBytes));
	}
	if (FullTopologyCaptures.Num() >= MultipartMaxActiveCaptures
		|| FullTopologyRetainedBytes + FrozenBytes.Num() > MultipartMaxRetainedBytes)
	{
		return MCPError(TEXT("Full Blueprint capture-capacity-exceeded: active capture quota is full"));
	}

	FFullTopologyCapture Capture;
	Capture.Handle = NewCaptureHandle();
	Capture.AssetPath = AssetPath;
	Capture.ObjectPath = BlueprintObjectPath;
	Capture.InventoryHash = InventoryHash;
	Capture.InventoryToken = TEXT("sha1:") + InventoryHash;
	Capture.SnapshotHash = Sha256Bytes(FrozenBytes);
	if (Capture.SnapshotHash.IsEmpty()) return MCPError(TEXT("Failed to hash frozen Full Blueprint topology"));
	Capture.CreatedAtUtc = FDateTime::UtcNow();
	Capture.ExpiresAtUtc = Capture.CreatedAtUtc + FTimespan::FromSeconds(MultipartCaptureTtlSeconds);
	Capture.FrozenBytes = MoveTemp(FrozenBytes);
	FullTopologyRetainedBytes += Capture.FrozenBytes.Num();
	const FString CaptureHandle = Capture.Handle;
	FullTopologyCaptures.Add(CaptureHandle, MoveTemp(Capture));
	return MCPResult(CaptureManifest(FullTopologyCaptures.FindChecked(CaptureHandle)));
}

TSharedPtr<FJsonValue> FBlueprintHandlers::ReadBlueprintTopologyChunk(const TSharedPtr<FJsonObject>& Params)
{
	FString CaptureHandle;
	if (auto Error = RequireString(Params, TEXT("captureHandle"), CaptureHandle)) return Error;
	double ChunkIndexNumber = -1.0;
	if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("chunkIndex"), ChunkIndexNumber)
		|| ChunkIndexNumber < 0.0 || ChunkIndexNumber != FMath::FloorToDouble(ChunkIndexNumber)
		|| ChunkIndexNumber > MAX_int32)
	{
		return MCPError(TEXT("chunkIndex must be a non-negative integer"));
	}

	CleanupExpiredFullTopologyCaptures();
	const FFullTopologyCapture* Capture = FullTopologyCaptures.Find(CaptureHandle);
	if (!Capture) return MCPError(TEXT("Full Blueprint capture handle was not found, expired, or released"));
	const int32 ChunkIndex = static_cast<int32>(ChunkIndexNumber);
	const int32 ChunkCount = FMath::DivideAndRoundUp(Capture->FrozenBytes.Num(), MultipartRawChunkBytes);
	if (ChunkIndex >= ChunkCount) return MCPError(TEXT("Full Blueprint capture chunk index is out of range"));

	const int32 Offset = ChunkIndex * MultipartRawChunkBytes;
	const int32 RawByteCount = FMath::Min(MultipartRawChunkBytes, Capture->FrozenBytes.Num() - Offset);
	TArray<uint8> ChunkBytes;
	ChunkBytes.Append(Capture->FrozenBytes.GetData() + Offset, RawByteCount);
	const FString ChunkHash = Sha256Bytes(ChunkBytes);
	if (ChunkHash.IsEmpty()) return MCPError(TEXT("Failed to hash Full Blueprint capture chunk"));

	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetStringField(TEXT("contractVersion"), TEXT("spacehead.full-blueprint-topology-chunk@1.0"));
	Result->SetStringField(TEXT("captureHandle"), Capture->Handle);
	Result->SetStringField(TEXT("assetPath"), Capture->AssetPath);
	Result->SetStringField(TEXT("objectPath"), Capture->ObjectPath);
	Result->SetNumberField(TEXT("chunkIndex"), ChunkIndex);
	Result->SetNumberField(TEXT("chunkCount"), ChunkCount);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("rawByteCount"), RawByteCount);
	Result->SetNumberField(TEXT("totalBytes"), Capture->FrozenBytes.Num());
	Result->SetStringField(TEXT("encoding"), TEXT("base64"));
	Result->SetStringField(TEXT("data"), FBase64::Encode(ChunkBytes));
	Result->SetStringField(TEXT("chunkHash"), ChunkHash);
	Result->SetStringField(TEXT("chunkHashAlgorithm"), TEXT("sha256"));
	Result->SetStringField(TEXT("snapshotHash"), Capture->SnapshotHash);
	Result->SetBoolField(TEXT("unrealAccessed"), false);
	Result->SetBoolField(TEXT("mutationOperationsPerformed"), false);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FBlueprintHandlers::ReleaseBlueprintTopologyCapture(const TSharedPtr<FJsonObject>& Params)
{
	FString CaptureHandle;
	if (auto Error = RequireString(Params, TEXT("captureHandle"), CaptureHandle)) return Error;
	CleanupExpiredFullTopologyCaptures();
	const FFullTopologyCapture* Capture = FullTopologyCaptures.Find(CaptureHandle);
	if (!Capture) return MCPError(TEXT("Full Blueprint capture handle was not found, expired, or released"));
	FullTopologyRetainedBytes -= Capture->FrozenBytes.Num();
	FullTopologyCaptures.Remove(CaptureHandle);

	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetStringField(TEXT("contractVersion"), TEXT("spacehead.full-blueprint-topology-release@1.0"));
	Result->SetStringField(TEXT("captureHandle"), CaptureHandle);
	Result->SetBoolField(TEXT("released"), true);
	Result->SetBoolField(TEXT("unrealAccessed"), false);
	Result->SetBoolField(TEXT("mutationOperationsPerformed"), false);
	return MCPResult(Result);
}
