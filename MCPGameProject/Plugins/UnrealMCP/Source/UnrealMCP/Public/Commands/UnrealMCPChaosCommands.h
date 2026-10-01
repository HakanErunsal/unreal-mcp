#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Chaos destruction authoring.
 * Commands: fracture_static_mesh.
 */
class UNREALMCP_API FUnrealMCPChaosCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * {mesh:path, out:path, seed, grout, collision_spacing, noise:{amplitude, frequency, octaves, point_spacing}, stages:[{planes:[{point:[x,y,z], normal:[x,y,z]}], within:[[min],[max]], noise:bool}], flatten:bool, internal_uv:{u:[ax,ay,az,c], v:[bx,by,bz,d]}}.
	 * Builds a Geometry Collection asset from a static mesh, cutting it with each stage's planes in mesh space. A stage cuts every leaf piece, or only the leaves whose bounds centre lies inside `within`, so later stages can break earlier strips at staggered places. `flatten` puts every leaf directly under the root. `internal_uv` re-projects the cut faces' UVs as u = a.p + c and v = b.p + d, with the axis a face looks down replaced by the plane normal so no face collapses to a line. Saves the asset, replacing the collection of one already at `out`.
	 */
	TSharedPtr<FJsonObject> FractureStaticMesh(const TSharedPtr<FJsonObject>& Params);
};
