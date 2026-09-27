#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Declarative authoring for Blueprint graphs, Blueprint variables, Blackboards and Behavior Trees.
 * Commands: add_variables, build_graph, build_blackboard, build_behavior_tree.
 */
class UNREALMCP_API FUnrealMCPGraphBuilder
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TSharedPtr<FJsonObject> AddVariables(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> BuildGraph(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> BuildBlackboard(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> BuildBehaviorTree(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> DescribeGraph(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> SetProperties(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> BuildWidget(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> RecolorComments(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> ListComments(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> DescribeWidget(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> SetComment(const TSharedPtr<FJsonObject>& Params);
};
