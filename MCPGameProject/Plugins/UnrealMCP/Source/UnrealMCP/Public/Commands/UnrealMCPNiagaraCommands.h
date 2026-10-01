#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Niagara system authoring through the same stack view models the Niagara editor uses.
 * Commands: niagara_add_emitter, niagara_describe, niagara_edit.
 * Modules and inputs are addressed by the names the stack shows ("Add Velocity", "Velocity Speed"); a dynamic input's own inputs by a slash path ("Lifetime/Minimum"). Emitter "System" addresses the system stack.
 */
class UNREALMCP_API FUnrealMCPNiagaraCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/** {system:path, template:emitter path, name}. Adds a copy of the template emitter to the system, renames it, compiles and saves. */
	TSharedPtr<FJsonObject> AddEmitter(const TSharedPtr<FJsonObject>& Params);

	/** {system:path, emitter:name}. Lists each emitter's stack groups, modules and inputs with their value mode and value, and its renderer objects. */
	TSharedPtr<FJsonObject> Describe(const TSharedPtr<FJsonObject>& Params);

	/**
	 * {system:path, ops:[...]}. Applies each op in order, then compiles and saves. Ops:
	 * {op:set, emitter, module, input, value} local value: number, bool, [x,y(,z(,w))], enum name, or an asset path for an object input.
	 * {op:dynamic, emitter, module, input, script:path} replaces the input with a dynamic input script.
	 * {op:link, emitter, module, input, parameter:"Particles.X"} reads the input from a parameter.
	 * {op:reset, emitter, module, input}; {op:edit_condition, emitter, module, input, enabled}.
	 * {op:enable_module, emitter, module, enabled}; {op:add_module, emitter, group, script:path, index}; {op:remove_module, emitter, module}.
	 * {op:rename_emitter, emitter, name}; {op:enable_emitter, emitter, enabled}; {op:remove_emitter, emitter}.
	 * {op:add_renderer, emitter, class:path} reports the new renderer's object path for set_properties; {op:remove_renderer, emitter, index}.
	 * A module name may carry "@Group" ("Add Velocity@Particle Spawn") when two groups hold one module name.
	 */
	TSharedPtr<FJsonObject> Edit(const TSharedPtr<FJsonObject>& Params);
};
