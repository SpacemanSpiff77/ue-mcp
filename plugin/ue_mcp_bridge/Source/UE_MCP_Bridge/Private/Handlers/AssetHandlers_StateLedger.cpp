#include "AssetHandlers.h"
#include "BlueprintHandlers.h"
#include "HandlerUtils.h"
#include "EditorAssetLibrary.h"
#include "IPlatformCrypto.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
	FString SerializeJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid()) return FString();
		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		if (!FJsonSerializer::Serialize(Object->ToSharedRef(), Writer)) return FString();
		return Output;
	}

	FString Sha256String(const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		const TUniquePtr<FEncryptionContext> Context = IPlatformCrypto::Get().CreateContext();
		if (!Context.IsValid()) return FString();
		TArray<uint8> Hash;
		if (!Context->CalcSHA256(Bytes, Hash)) return FString();
		return BytesToHex(Hash.GetData(), Hash.Num()).ToLower();
	}

	void StripObservationFields(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid()) return;
		for (const TCHAR* Field : { TEXT("dirtyBefore"), TEXT("dirtyAfter"), TEXT("dirty"), TEXT("dirtyStateChanged"),
			TEXT("mutationGuardPassed"), TEXT("mutationOperationsPerformed"), TEXT("compileRequested"), TEXT("saveRequested"),
			TEXT("reconstructRequested"), TEXT("serializedPayloadBytes") })
		{
			Object->RemoveField(Field);
		}
	}
}

TSharedPtr<FJsonValue> FAssetHandlers::ReadStateLedgerFingerprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Error = RequireStringAlt(Params, TEXT("assetPath"), TEXT("path"), AssetPath)) return Error;
	FString AssetType;
	if (auto Error = RequireString(Params, TEXT("assetType"), AssetType)) return Error;

	UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!Asset) return MCPError(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	UPackage* Package = Asset->GetOutermost();
	const bool bDirtyBefore = Package && Package->IsDirty();
	FString Projection;
	int32 GraphCount = 0;
	int32 FieldCount = 0;
	int32 EnumeratorCount = 0;

	if (AssetType == TEXT("Blueprint"))
	{
		TSharedPtr<FJsonObject> TopologyParams = MakeShared<FJsonObject>();
		TopologyParams->SetStringField(TEXT("assetPath"), AssetPath);
		TopologyParams->SetNumberField(TEXT("maxAuthoredGraphs"), 256);
		TopologyParams->SetNumberField(TEXT("maxNodesPerGraph"), 4096);
		TopologyParams->SetNumberField(TEXT("maxPinsPerGraph"), 32768);
		TopologyParams->SetNumberField(TEXT("maxConnectionsPerGraph"), 65536);
		TopologyParams->SetNumberField(TEXT("maxTotalNodes"), 16384);
		TopologyParams->SetNumberField(TEXT("maxTotalPins"), 131072);
		TopologyParams->SetNumberField(TEXT("maxTotalConnections"), 262144);
		TopologyParams->SetNumberField(TEXT("maxSerializedBytes"), 8388608);
		const TSharedPtr<FJsonValue> Topology = FBlueprintHandlers::ReadBlueprintTopology(TopologyParams);
		const TSharedPtr<FJsonObject>* TopologyObject = nullptr;
		if (!Topology.IsValid() || !Topology->TryGetObject(TopologyObject) || !TopologyObject || !(*TopologyObject)->GetBoolField(TEXT("success")))
			return MCPError(TEXT("Qualified Blueprint topology traversal failed during fingerprinting"));
		if ((*TopologyObject)->GetBoolField(TEXT("dataOmitted"))
			|| (*TopologyObject)->GetBoolField(TEXT("truncated"))
			|| (!(*TopologyObject)->HasField(TEXT("snapshotHash")) && !(*TopologyObject)->GetBoolField(TEXT("complete"))))
			return MCPError(TEXT("Qualified Blueprint topology traversal was incomplete during fingerprinting"));
		FString TopologyHash;
		if ((*TopologyObject)->HasField(TEXT("snapshotHash")))
		{
			TopologyHash = (*TopologyObject)->GetStringField(TEXT("snapshotHash"));
			const FString Handle = (*TopologyObject)->GetStringField(TEXT("captureHandle"));
			TSharedPtr<FJsonObject> ReleaseParams = MakeShared<FJsonObject>();
			ReleaseParams->SetStringField(TEXT("captureHandle"), Handle);
			const TSharedPtr<FJsonValue> Release = FBlueprintHandlers::ReleaseBlueprintTopologyCapture(ReleaseParams);
			const TSharedPtr<FJsonObject>* ReleaseObject = nullptr;
			if (!Release.IsValid() || !Release->TryGetObject(ReleaseObject) || !ReleaseObject || !(*ReleaseObject)->GetBoolField(TEXT("released")))
				return MCPError(TEXT("Blueprint fingerprint capture release failed"));
		}
		else
		{
			StripObservationFields(*TopologyObject);
			TopologyHash = Sha256String(SerializeJsonValue(Topology));
		}
		if (TopologyHash.Len() != 64) return MCPError(TEXT("Qualified Blueprint topology hash was invalid during fingerprinting"));
		GraphCount = static_cast<int32>((*TopologyObject)->GetNumberField(TEXT("totalGraphCount")));
		TSharedPtr<FJsonObject> StructureParams = MakeShared<FJsonObject>();
		StructureParams->SetStringField(TEXT("path"), AssetPath);
		const TSharedPtr<FJsonValue> Structure = FBlueprintHandlers::ReadBlueprint(StructureParams);
		const TSharedPtr<FJsonObject>* StructureObject = nullptr;
		if (!Structure.IsValid() || !Structure->TryGetObject(StructureObject) || !StructureObject || !(*StructureObject)->GetBoolField(TEXT("success")))
			return MCPError(TEXT("Blueprint structure traversal failed during fingerprinting"));
		if ((*StructureObject)->GetStringField(TEXT("contractVersion")) != TEXT("spacehead.blueprint-structure@1.0")
			|| !(*StructureObject)->GetBoolField(TEXT("complete")) || (*StructureObject)->GetBoolField(TEXT("dirtyStateChanged"))
			|| (*StructureObject)->GetBoolField(TEXT("mutationOperationsPerformed")))
			return MCPError(TEXT("Blueprint structure traversal was incomplete or unsafe during fingerprinting"));
		StripObservationFields(*StructureObject);
		Projection = TopologyHash + TEXT("\n") + SerializeJsonValue(Structure);
	}
	else if (AssetType == TEXT("UserDefinedStruct"))
	{
		const TSharedPtr<FJsonValue> Definition = ListStructFields(Params);
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Definition.IsValid() || !Definition->TryGetObject(Object) || !Object || !(*Object)->GetBoolField(TEXT("success"))) return Definition;
		if ((*Object)->GetStringField(TEXT("contractVersion")) != TEXT("spacehead.struct-definition@1.0")
			|| !(*Object)->GetBoolField(TEXT("complete")) || (*Object)->GetBoolField(TEXT("dirtyStateChanged"))
			|| (*Object)->GetBoolField(TEXT("mutationOperationsPerformed"))) return MCPError(TEXT("Struct definition traversal was incomplete or unsafe"));
		FieldCount = static_cast<int32>((*Object)->GetNumberField(TEXT("count")));
		StripObservationFields(*Object);
		Projection = SerializeJsonValue(Definition);
	}
	else if (AssetType == TEXT("UserDefinedEnum"))
	{
		const TSharedPtr<FJsonValue> Definition = ListEnumValues(Params);
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Definition.IsValid() || !Definition->TryGetObject(Object) || !Object || !(*Object)->GetBoolField(TEXT("success"))) return Definition;
		if ((*Object)->GetStringField(TEXT("contractVersion")) != TEXT("spacehead.enum-definition@1.0")
			|| !(*Object)->GetBoolField(TEXT("complete")) || (*Object)->GetBoolField(TEXT("dirtyStateChanged"))
			|| (*Object)->GetBoolField(TEXT("mutationOperationsPerformed"))) return MCPError(TEXT("Enum definition traversal was incomplete or unsafe"));
		EnumeratorCount = static_cast<int32>((*Object)->GetNumberField(TEXT("count")));
		StripObservationFields(*Object);
		Projection = SerializeJsonValue(Definition);
	}
	else return MCPError(TEXT("assetType must be Blueprint, UserDefinedStruct, or UserDefinedEnum"));

	const FString Fingerprint = Sha256String(Projection);
	if (Fingerprint.IsEmpty()) return MCPError(TEXT("Failed to compute State Ledger SHA-256 fingerprint"));
	const bool bDirtyAfter = Package && Package->IsDirty();
	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetStringField(TEXT("contractVersion"), TEXT("spacehead.state-ledger-fingerprint@1.0"));
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("objectPath"), Asset->GetPathName());
	Result->SetStringField(TEXT("assetType"), AssetType);
	Result->SetStringField(TEXT("algorithm"), TEXT("sha256"));
	Result->SetStringField(TEXT("fingerprint"), Fingerprint);
	Result->SetNumberField(TEXT("graphCount"), GraphCount);
	Result->SetNumberField(TEXT("fieldCount"), FieldCount);
	Result->SetNumberField(TEXT("enumeratorCount"), EnumeratorCount);
	Result->SetBoolField(TEXT("dirty"), bDirtyAfter);
	Result->SetBoolField(TEXT("dirtyStateChanged"), bDirtyBefore != bDirtyAfter);
	Result->SetBoolField(TEXT("mutationOperationsPerformed"), false);
	Result->SetBoolField(TEXT("compileRequested"), false);
	Result->SetBoolField(TEXT("saveRequested"), false);
	Result->SetBoolField(TEXT("reconstructRequested"), false);
	Result->SetBoolField(TEXT("layoutIncluded"), AssetType == TEXT("Blueprint"));
	if (bDirtyBefore != bDirtyAfter) return MCPError(TEXT("Read-only fingerprint traversal changed package dirty state"));
	return MCPResult(Result);
}
