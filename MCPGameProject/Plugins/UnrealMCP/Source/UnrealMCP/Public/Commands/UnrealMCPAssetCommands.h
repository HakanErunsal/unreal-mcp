#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Generic asset and graph surgery for any asset type.
 * Commands: replace_object_references, create_subobject, export_properties, edgraph_describe, edgraph_add_node, edgraph_connect, edgraph_remove_node, blueprint_retarget, fixup_redirectors, list_objects, rename_object.
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

	/** {blueprint:path, map:{old_object_path:new_object_path}, text_replace:[[from,to]]}. Points every reference inside the Blueprint at its mapped object (a mapped Blueprint also maps its generated class), rewrites pin defaults, variable defaults, node comments and class defaults through the text replacements, then refreshes every node, compiles and saves. */
	TSharedPtr<FJsonObject> BlueprintRetarget(const TSharedPtr<FJsonObject>& Params);

	/** {path:folder}. Points every referencer of each redirector under the folder at the redirector's target, saves them, and deletes each redirector nothing references any more, without the engine's delete prompt. */
	TSharedPtr<FJsonObject> FixupRedirectors(const TSharedPtr<FJsonObject>& Params);

	/** {asset:path, contains:text}. Lists every object in the asset's package with its class and outer, optionally only those whose name contains the text. */
	TSharedPtr<FJsonObject> ListObjects(const TSharedPtr<FJsonObject>& Params);

	/** {object:path, new_name:name}. Renames an object inside its package without leaving a redirector, then saves the package. */
	TSharedPtr<FJsonObject> RenameObject(const TSharedPtr<FJsonObject>& Params);
};
