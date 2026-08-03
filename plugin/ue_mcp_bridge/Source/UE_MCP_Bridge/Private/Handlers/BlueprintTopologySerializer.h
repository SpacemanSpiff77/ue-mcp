#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UEdGraph;

namespace UE_MCP_BlueprintTopology
{
	struct FGraphSerializationOptions
	{
		FString GraphIdentity;
		FString NormalizedGraphType;
		FString FunctionName;
		TArray<FString> CollectionMemberships;
		FString OwnershipKind;
		FString ParentGraphIdentity;
		int32 NestingDepth = 0;
		FString GraphProvenance;
		bool bIncludeFullBlueprintMetadata = false;
		bool bRejectDuplicateNodeScopedPinIdentity = false;
	};

	struct FSerializedGraphTopology
	{
		TSharedPtr<FJsonObject> Graph;
		int32 NodeCount = 0;
		int32 PinCount = 0;
		int32 ConnectionCount = 0;
		int32 UnresolvedEndpointCount = 0;
		int32 DuplicatePinIdentityCount = 0;

		bool IsComplete() const
		{
			return UnresolvedEndpointCount == 0 && DuplicatePinIdentityCount == 0;
		}
	};

	/**
	 * Qualified exact graph serializer shared by the selected-function and
	 * full-Blueprint providers. Pin identity is intentionally node-scoped:
	 * duplicate native pin GUIDs on different nodes are valid, while duplicate
	 * identities inside one node can be rejected by the full provider.
	 */
	FSerializedGraphTopology SerializeGraph(
		UEdGraph* Graph,
		const FGraphSerializationOptions& Options);
}
