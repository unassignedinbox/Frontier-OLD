# Frontier + Project Everest — Windows x64 Release preview

Keep this folder together after extraction. `EngineContent/` and `Projects/Everest/` are runtime data, not optional extras.

## Launch

- `Frontier.exe` is the Frontier editor. To open the Everest project from a terminal, run:

  ```text
  Frontier.exe Projects\Everest\Everest.projectspace
  ```

  You can also open Project Everest from Frontier's Project Browser.
- `Everest.exe` is the separate standalone Project Everest runtime. It starts in the existing **Studio Lobby** experience; it is not the Frontier editor. Its startup content is under `Projects/Everest/Content/` and `EngineContent/`.
- `SolidArcWorkspace.exe`, when present, is Frontier's companion for opening `.solidarc` models. `TextureWorkspace.exe` is not included in this two-target preview.

The package contains the engine shaders, fonts, icons and other `EngineContent/` assets, plus `Projects/Everest/Everest.projectspace` and the project's `Content/` tree. `Package.json` records the source revision and hashes of the packaged files.

## Security / provenance

These are unsigned development-preview executables; this workflow does not have a trusted publisher signing certificate. Chrome, SmartScreen or antivirus may still warn about them. Package hashes help detect file changes but do not establish publisher identity or Chrome reputation. Do not disable or bypass browser or antivirus protection.

The binaries were built by the linked GitHub Actions run. `Package.json` records that this preview was not Authenticode-signed, antivirus-scanned, or interactively runtime-verified on Windows.
