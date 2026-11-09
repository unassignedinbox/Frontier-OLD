# Project Designer — Frontier’s project-owned alternative to Blueprint Editor

**Name chosen:** **Project Designer**. It is the umbrella name for project-defined C++ classes, their authoring UI, and user-authored workflows. “Workflow Editor” and “Configuration Editor” remain useful names for two capabilities inside the Project Designer, not competing product names.

## Product direction

A user should be able to add a C++ class or project type and give that type its own:

- **Inspector** — the fields, controls, validation, and live readouts used to inspect/configure an instance.
- **Icon and presentation** — a project-owned icon, display name, category, accent, and Outliner treatment.
- **Editor** — a dedicated in-app authoring experience, normally dockable and bound to the selected instance.
- **Configuration codec** — versioned, project-owned persistence for authored settings.
- **Workflow capabilities** — typed events, actions, or data ports that can be used by the project’s workflow graphs.

The intent is to give project authors more control over their own tools without requiring every domain editor to be hard-coded into Frontier’s central Inspector. Existing native inspectors and editors remain valid entry points and can be wrapped or registered as project-owned tools incrementally.

## Two complementary authoring modes

### Workflow Editor

A visual, typed graph for composing events and actions—an alternative to Blueprint-style gameplay graphs, not a replacement for writing C++. Candidate use cases include trigger-to-action chains, test sequences, effect sequencing, and explicit system composition. Ports should carry typed data and validation should report missing inputs, incompatible types, cycles where disallowed, and unavailable project capabilities before execution.

Workflows must call registered project/engine operations rather than execute arbitrary serialized code. They should compose independent systems through explicit inputs (for example, a selected source, a wind sample, or a particle trigger); they must not merge or silently alter the Fluid, Particle, ForceField, or physics solvers.

### Configuration Editor

A native domain-specific editor for the settings and authoring process of a class or component. It can include a viewport, timeline, previews, bake controls, and specialized inspector sections. It should be a dockable in-app window/panel, preserve the class’s ordinary Inspector entry point, and use the class’s configuration codec.

Examples:

- The **Fracture Editor** is the first shape of this workflow: it is opened from both a Fracture record and a Geometry record, and now previews deterministic convex Voronoi cells. Its current preview is not runtime fracture physics.
- The planned **Vehicle Configuration Editor** is a Configuration Editor for vehicle components, controls, response curves, toggles, and test flow; it should reuse the existing vehicle and tyre records rather than fork their persisted settings.

## Project class registration model

The implementation should establish an explicit, stable registration contract. A project class descriptor should be able to provide:

1. A stable project-scoped type identifier and schema version.
2. A C++ construction/factory function and configuration codec.
3. Inspector, icon, and dedicated editor factories (each optional where appropriate).
4. Outliner grouping, display name, icon key, and presentation metadata.
5. Workflow event/action/typed-port descriptors, when that class participates in workflows.
6. Capability/availability diagnostics so a missing module or invalid configuration is visible rather than silently ignored.

Registrations should be discoverable by the Project Designer’s class catalog and usable by the existing construct/outliner/inspector paths. Project-owned descriptors and saved data need versioning and validation. Do not assume arbitrary C++ can safely be compiled or loaded from untrusted project files: executable code remains a trusted project extension/build concern, while project manifests describe stable registrations and data contracts.

## Intended user flow

1. Add or select a project C++ class in the Project Designer.
2. Register its stable identifier, factory, configuration schema, icon, Inspector factory, and optional dockable Configuration Editor.
3. Define workflow actions and typed ports for the class, or leave it out of workflows.
4. Preview the class in the project’s Outliner/Inspector, open its dedicated editor, and connect its registered actions in a Workflow Editor graph.
5. Validate and save the project-owned configuration/workflow data with explicit schema versions.

## Compatibility and boundaries

- Preserve existing native Inspector buttons and record selection; a registered editor adds an entry point rather than silently replacing one.
- Keep the accepted Fluid Editor unchanged and modal as already directed.
- Keep each solver independently constructible. Workflows may connect them through explicit inputs and triggers, not by merging their algorithms.
- Keep icons and type metadata project-scoped where possible, with existing Frontier icons and inspectors available as defaults.
- Distinguish a geometry/configuration preview from a validated runtime solver or bake in labels and documentation.

## Delivery sequence

1. Inventory `WorldRegister`, record factories, `RecordInspector`, `IconPresentation`, `ProjectSpaceCodec`, and dockspace registration; map the current hard-coded seams.
2. Define and validate the versioned project-class descriptor and registration API.
3. Register one existing native type without changing its Inspector behavior, then add a project-owned icon and dockable editor factory.
4. Add the Configuration Editor catalog and lifecycle/codec tests; migrate Fracture and the future Vehicle Configuration Editor incrementally.
5. Add the Workflow Editor’s typed graph model, validation, persistence, and deterministic execution tests before adding more node types.
6. Document extension trust boundaries, reload behavior, schema migration, and the project author workflow.

**Status:** requested and documented; this is a planned product direction, not implemented functionality. The current task continues Batch 4 and must not claim that Project Designer or Workflow Editor APIs already exist.
