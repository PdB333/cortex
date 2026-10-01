# Qt Q_INVOKABLE -> Dear ImGui parity audit

This is the method-level release gate for the frozen Qt application layer.
It is generated from the Qt controller headers on the ImGui migration branch
and records the replacement for every public `Q_INVOKABLE` workflow.

Outcomes:

- **Direct**: a direct Dear ImGui/application-model workflow exists.
- **Replacement**: the Qt convenience method is replaced by a toolkit-neutral/global ImGui mechanism.
- **Lifecycle**: the method is not a standalone user workflow; equivalent reset/cleanup runs as part of target/session lifecycle.

Audit result: **148 mapped / 0 unmapped**  
Breakdown: **135 Direct / 6 Replacement / 7 Lifecycle**.

| Controller | Q_INVOKABLE | Outcome | Dear ImGui / native coverage |
| --- | --- | --- | --- |
| AiActivityController | `clear()` | Direct | `AiActivityModel::ClearHistory` / Bottom Panel AI tab |
| AppController | `refreshTargets()` | Direct | Process picker / `AppState::RefreshTargets` |
| AppController | `selectTarget()` | Direct | Process picker attach flow |
| AppController | `detachTarget()` | Direct | Header detach / session manager |
| AppController | `detachTargetAt()` | Direct | `SessionsWorkspace` per-target Detach |
| AppController | `detachAllTargets()` | Direct | `SessionsWorkspace` Detach all |
| AppController | `selectSection()` | Replacement | `WorkspaceRegistry::Select` |
| AppController | `openAddress()` | Replacement | `UiContext::NavigateTo` targeted workspace routing |
| AppController | `resolveAddressExpression()` | Replacement | global Go To resolver (Ctrl+G) |
| AppController | `copyText()` | Replacement | ImGui clipboard actions / shared address context menu |
| AppController | `capabilitySummary()` | Direct | Overview + Sessions capability summaries |
| AppController | `readMemory()` | Direct | `MemoryBrowserWorkspace` |
| AppController | `writeMemoryHex()` | Direct | gated byte write in `MemoryBrowserWorkspace` |
| AppController | `refreshModules()` | Direct | `ModulesWorkspace` |
| AppController | `startScan()` | Direct | Scanner in `MemoryWorkspace` |
| AppController | `cancelScan()` | Direct | Scanner Cancel |
| AppController | `clearScanResults()` | Direct | Scanner New scan / result reset |
| DebuggerController | `refreshThreads()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `selectThread()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `enableRuntime()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `refreshRuntime()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `addBreakpoint()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `removeBreakpoint()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `pauseCurrent()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `continueCurrent()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `stepCurrent()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `stepOverCurrent()` | Direct | `DebuggerWorkspace` + `DebuggerModel` |
| DebuggerController | `clear()` | Lifecycle | Debugger model resets on target/session changes |
| DisassemblyController | `disassemble()` | Direct | `DisassemblyWorkspace` decode |
| DisassemblyController | `goBack()` | Replacement | global address history (Alt+Left) |
| DisassemblyController | `goForward()` | Replacement | global address history (Alt+Right) |
| DisassemblyController | `analyzeCfg()` | Direct | Disassembler CFG action (`analysis_cfg`) |
| DisassemblyController | `analyzeXrefs()` | Direct | Disassembler Xrefs action (`analysis_xrefs`) |
| DisassemblyController | `analyzeStructure()` | Direct | Disassembler Structured CFG action (`analysis_structure`) |
| DisassemblyController | `clearAnalysis()` | Direct | Disassembler Clear analysis |
| DisassemblyController | `clear()` | Lifecycle | Disassembly state resets on target/session changes |
| FeatureController | `refreshApiLog()` | Direct | `EventsWorkspace` / Bottom Panel Events + Console |
| FeatureController | `refreshRuntimeEvents()` | Direct | `EventsWorkspace` / Bottom Panel Events + Console |
| FeatureController | `refreshProject()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `setProjectAddress()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `deleteProjectAddress()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `setProjectPointerPath()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `deleteProjectPointerPath()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `resolveProjectPointerPath()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `addProjectNote()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `deleteProjectNote()` | Direct | `ProjectWorkspace` / persistent `AddressesWorkspace` |
| FeatureController | `refreshActions()` | Direct | `ActionsWorkspace` / `ActionsModel` |
| FeatureController | `rollbackAllActions()` | Direct | `ActionsWorkspace` / `ActionsModel` |
| FeatureController | `rollbackTo()` | Direct | `ActionsWorkspace` / `ActionsModel` |
| FeatureController | `clearActions()` | Direct | `ActionsWorkspace` / `ActionsModel` |
| FeatureController | `setNetworkCapture()` | Direct | `NetworkWorkspace` / `NetworkModel` |
| FeatureController | `refreshNetwork()` | Direct | `NetworkWorkspace` / `NetworkModel` |
| FeatureController | `sendKeyTap()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `sendText()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `startInputRecording()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `stopInputRecording()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `startInputSequence()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `replayRecordedInput()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `refreshInputSequence()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `cancelInputSequence()` | Direct | `InputWorkspace` / `InputModel` |
| FeatureController | `captureScreenshot()` | Direct | `ScreenshotWorkspace` / `ScreenshotModel` |
| FeatureController | `refreshScripts()` | Direct | `ScriptsWorkspace` / `ScriptsModel` |
| FeatureController | `loadScript()` | Direct | `ScriptsWorkspace` / `ScriptsModel` |
| FeatureController | `saveScript()` | Direct | `ScriptsWorkspace` / `ScriptsModel` |
| FeatureController | `runScriptBuffer()` | Direct | `ScriptsWorkspace` / `ScriptsModel` |
| FeatureController | `runSavedScript()` | Direct | `ScriptsWorkspace` / `ScriptsModel` |
| FeatureController | `deleteScript()` | Direct | `ScriptsWorkspace` / `ScriptsModel` |
| FeatureController | `clearScriptSelection()` | Direct | `ScriptsWorkspace` / `ScriptsModel` |
| FeatureController | `refreshWatches()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `addFreeze()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `deleteFreeze()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `addWatch()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `deleteWatch()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `refreshInstrumentationState()` | Direct | `InstrumentationWorkspace` / `InstrumentationModel` |
| FeatureController | `refreshInstrumentationEvents()` | Direct | `InstrumentationWorkspace` / `InstrumentationModel` |
| FeatureController | `setAllocationWatch()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `addPageAccessWatch()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `deletePageAccessWatch()` | Direct | `WatchesWorkspace` + `AddressesWorkspace` / `WatchesModel` |
| FeatureController | `resolveSymbol()` | Direct | `SymbolsWorkspace` / `SymbolsModel` |
| FeatureController | `lookupSymbol()` | Direct | `SymbolsWorkspace` / `SymbolsModel` |
| FeatureController | `clearSymbolResult()` | Direct | `SymbolsWorkspace` / `SymbolsModel` |
| FeatureController | `refreshStructures()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `selectStructure()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `clearStructureSelection()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `defineStructure()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `deleteStructure()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `readStructure()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `writeStructure()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `inferStructure()` | Direct | `StructuresWorkspace` / `StructuresModel` |
| FeatureController | `refreshDiagnostics()` | Direct | `DiagnosticsWorkspace` / Bottom Panel Diagnostics |
| FeatureController | `refreshPatches()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `applyPatchBytes()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `applyPatchNop()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `applyPatchAssembly()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `applyPatchDetour()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `applyPatchTrampoline()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `allocatePatchCave()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `revertPatch()` | Direct | `PatchesWorkspace` / `PatchesModel` |
| FeatureController | `refreshSnapshots()` | Direct | `SnapshotsWorkspace` / `SnapshotsModel` |
| FeatureController | `createSnapshot()` | Direct | `SnapshotsWorkspace` / `SnapshotsModel` |
| FeatureController | `diffSnapshots()` | Direct | `SnapshotsWorkspace` / `SnapshotsModel` |
| FeatureController | `rewindSnapshot()` | Direct | `SnapshotsWorkspace` / `SnapshotsModel` |
| FeatureController | `deleteSnapshot()` | Direct | `SnapshotsWorkspace` / `SnapshotsModel` |
| FeatureController | `lastSnapshotChange()` | Direct | `SnapshotsWorkspace` / `SnapshotsModel` |
| FeatureController | `refreshPointerMaps()` | Direct | `PointerMapsWorkspace` / `PointerMapsModel` |
| FeatureController | `capturePointerMap()` | Direct | `PointerMapsWorkspace` / `PointerMapsModel` |
| FeatureController | `intersectPointerMaps()` | Direct | `PointerMapsWorkspace` / `PointerMapsModel` |
| FeatureController | `deletePointerMap()` | Direct | `PointerMapsWorkspace` / `PointerMapsModel` |
| FeatureController | `refreshTraces()` | Direct | `TraceWorkspace` |
| FeatureController | `startTrace()` | Direct | `TraceWorkspace` |
| FeatureController | `stopTrace()` | Direct | `TraceWorkspace` |
| FeatureController | `deleteTrace()` | Direct | `TraceWorkspace` |
| FeatureController | `selectTrace()` | Direct | `TraceWorkspace` |
| FeatureController | `loadTraceEvents()` | Direct | `TraceWorkspace` |
| FeatureController | `exportSession()` | Direct | RE workspace Sessions tab (`ReModel::ExportSession`) |
| FeatureController | `reset()` | Lifecycle | application models reset on target/session changes |
| PayloadController | `ensureReady()` | Direct | Runtime enable/connect flows across Runtime and runtime-backed workspaces |
| PayloadController | `tryConnectExisting()` | Direct | Runtime enable/connect flows across Runtime and runtime-backed workspaces |
| PayloadController | `reset()` | Lifecycle | `PayloadClient::Reset` on attach/detach/target activation |
| PromptController | `answer()` | Direct | modal human Prompt surface / `PromptModel` |
| PromptController | `refresh()` | Direct | modal human Prompt surface / `PromptModel` |
| PromptController | `reset()` | Lifecycle | `PromptModel::Reset` on attach/detach |
| ReController | `reset()` | Lifecycle | `ReModel::Reset` on target/session changes |
| ReController | `refresh()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `refreshSessions()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `refreshCheckpoints()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `selectTrack()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `trackObject()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `deleteTrack()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `findLastWriter()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `detectSubobjects()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `traceTransition()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `runTest()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `createCheckpoint()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `rollbackCheckpoint()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `deleteCheckpoint()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `saveFact()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `saveBreakpointTemplates()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `applyBreakpointTemplates()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `exportSession()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `diffSessions()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `ghidraExport()` | Direct | `ReWorkspace` / `ReModel` |
| ReController | `ghidraImport()` | Direct | `ReWorkspace` / `ReModel` |
| RuntimeController | `refreshTools()` | Direct | Runtime workspace Refresh catalog |
| RuntimeController | `callToolJson()` | Direct | Runtime workspace direct tool call |
| RuntimeController | `clearResult()` | Direct | Runtime tool selection/result reset |
| RuntimeController | `reset()` | Lifecycle | Runtime workspace/model state resets on target/session changes |
| SettingsController | `resetDefaults()` | Direct | `SettingsWorkspace` / `SettingsStore` |

## Gate

No Qt `Q_INVOKABLE` remains without an explicit migration outcome.
Any future controller invokable added to the frozen reference must be added to
this table before Qt/QML removal.
