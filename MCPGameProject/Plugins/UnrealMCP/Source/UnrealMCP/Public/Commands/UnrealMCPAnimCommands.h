#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Animation asset maintenance that Python cannot reach.
 * Commands: replace_skeleton, replace_notify_classes, describe_notifies.
 */
class UNREALMCP_API FUnrealMCPAnimCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/** {assets:[path], skeleton:path, convert_spaces:false}. Rebinds each animation asset to the skeleton and saves it. */
	TSharedPtr<FJsonObject> ReplaceSkeleton(const TSharedPtr<FJsonObject>& Params);

	/** {asset:path, class_map:{old_class_path:new_class_path}, text_replace:[[from,to]]}. Swaps each matching notify for a new object of the mapped class, copying same-named properties through their text form with the replacements applied, then saves. */
	TSharedPtr<FJsonObject> ReplaceNotifyClasses(const TSharedPtr<FJsonObject>& Params);

	/** {asset:path}. Lists every notify with its class, track, time, duration and property text. */
	TSharedPtr<FJsonObject> DescribeNotifies(const TSharedPtr<FJsonObject>& Params);
};
