#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Generic asset and graph surgery for any asset type.
 * Commands: replace_object_references, create_subobject, export_properties, edgraph_describe, edgraph_add_node, edgraph_connect, edgraph_remove_node.
 */
class UNREALMCP_API FUnrealMCPAssetCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/** {assets:[path], map:{old_object_path:new_object_path}}. Points every reference to an old object inside each asset at its new object, then saves. */
	TSharedPtr<FJsonObject> ReplaceObjectReferences(const TSharedPtr<FJsonObject>& Params);

	/** {object:path, property:dotted.path, class:path}. Creates an instanced subobject of the class inside the object and assigns it to the property, appending when the property is an array. */
	TSharedPtr<FJsonObject> CreateSubobject(const TSharedPtr<FJsonObject>& Params);

	/** {object:path, properties:[name]}. Exports the named properties, or every property, as text. */
	TSharedPtr<FJsonObject> ExportProperties(const TSharedPtr<FJsonObject>& Params);

	/** {asset:path} or {graph:path}. Lists every node of every graph with its class, position and pin links. */
	TSharedPtr<FJsonObject> EdGraphDescribe(const TSharedPtr<FJsonObject>& Params);

	/** {graph:path, class:path, x, y}. Places a node the way the graph editor does. */
	TSharedPtr<FJsonObject> EdGraphAddNode(const TSharedPtr<FJsonObject>& Params);

	/** {graph:path, from:node, from_pin:name, to:node, to_pin:name}. Connects through the graph's schema, which may place nodes of its own between the two. */
	TSharedPtr<FJsonObject> EdGraphConnect(const TSharedPtr<FJsonObject>& Params);

	/** {graph:path, node:name}. Breaks the node's links and removes it. */
	TSharedPtr<FJsonObject> EdGraphRemoveNode(const TSharedPtr<FJsonObject>& Params);
};
