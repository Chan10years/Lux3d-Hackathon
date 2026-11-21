# Source-only publication · 2026-11-19

## Scope and provenance

This branch starts from the remote main commit `773feee3c0e576498cb5a33d58257ffce1551fc8`. It does not import the Unreal development branch history containing game assets. Previously committed prototype files on main are preserved; all new game files are source/text.

Unreal code is exported from the current development worktree based on commit `99cb658`, including its uncommitted combat, room and hero animation sync changes. This is a source snapshot, not a claim that the in-progress game was fully accepted. The code-only export changes shared configuration and documentation, removes an Android File Server config section containing a local token, removes three development-only MCP plugin entries, and removes a workstation path from a comment. Gameplay code is otherwise retained. The glTFRuntime asset-based test suite and its binary/model fixtures are omitted together; retained plugin runtime/editor source files are unchanged.

Room backend is exported from local backend commit `510d60c`, including five local revisions after main. Its `app/**/*.py` files match the deployed `Windows/Room/app` byte-for-byte at export time. The frozen generation-service build copy differs in `room_index/main.py` and is not used as the room-service source. Backend README workstation paths are replaced with placeholders.

## Assets required for the full experience

Neither project nor plugin Content is included. Integration references remain in source and Config:

- `/Game/SuoMaoGame/Lvl_Front`, `/Game/SuoMao/`: front-end maps, UI blueprints, fonts and audio.
- `/Game/ThirdPerson/`, `/Game/CityZZZ/`: gameplay maps, game mode and city HUD textures.
- Character, movement animation, attack animation and VFX packages referenced in PlayerCombatComponent and TrioCombat.
- `Content/WeaponSamples/`: optional local GLB weapon samples.
- `/glTFRuntime/`: the loader's default material assets. Source compilation does not replace these material assets.
- `/TrioCombat/`: data-table and death effect assets. Text attack/combatant CSV files are included as reference but do not replace every uasset.

Obtain authorized assets separately or supply replacements, preserve the expected paths or update source/config references, and build/assign the corresponding maps and blueprints before playing. Missing assets can cause load errors or missing functionality even when source compilation succeeds. The repository contains no reconstruction download links and does not grant rights to commercial assets.

## Services and startup

Room index runs independently from `backend/` on port 8901. For the room-index client path, pass `-BackendUrl=http://<host>:8901/v1` to Unreal; the client appends `/rooms`. The Unreal client also contains direct LAN connection paths. `-AutoLAN` expects an external local controller on port 18902 that is excluded from this source-only export; it is not enabled by the room index or the discovery utility alone. A health check for the Python room index is not an end-to-end Unreal multiplayer acceptance test.

`unreal/scripts/lan_discovery.py` and `unreal/scripts/forge_gateway.py` are published as networking utilities with local tests. The gateway's standalone launcher still expects an external portable generation-service installation and is not a bundled generation backend. Original game launchers, packaging scripts, generation-service implementation, frozen executables and paid-test probes are intentionally excluded because they depend on private runtime bundles, credentials or local workstation layout. For a configured game, launch through the UE editor or your separately assembled authorized build.

The original local project and Git histories are preserved. No full-content branch, internal handoff, raw evidence, credentials, databases or generated models are added by this export.

## Verification

Observed checks on the export checkout:

- `python -m pytest tests -q` in `backend/`: **55 passed**.
- `python -m unittest discover -s unreal/tests -p "test_*.py" -v`: **13 passed**, local gateway/discovery only, no cloud calls.
- `unreal/scripts/Test-Cpp.cmd`: **75 combat-rule checks passed**; city map projection and polygon clipping passed.
- Unreal 5.8 `Build.bat Try_GLB_Put_IntoEditor Win64 Development`: initial full source build **succeeded** (77 actions); final incremental build after physically removing upstream fixture-dependent tests also **succeeded** (3 link/metadata actions).
- No game or cloud generation end-to-end acceptance is claimed. No Cook/package or multiplayer playtest was run from this asset-free checkout.
- Staged-file audit: **148 files, approximately 1.6 MiB**, all UTF-8 text/source; no binary assets/runtime directories, unexpected file types, common real-key formats or credential literals found. All selected source files are present. Original development-source SHA-256 hashes remain unchanged. The vendor MIT license is preserved. These checks are scoped to new publication files; they are not a comprehensive security audit.

Existing upstream whitespace is retained in vendor source. A diff check with trailing-space and blank-EOF warnings disabled is used to check for other patch errors. UE compilation emits existing NetUpdateFrequency deprecation warnings in CombatMatchActor and GoldenHammerActor; these are not compile failures.


## Latest source sync · 2026-11-21

The default branch now includes the reviewed source snapshot. Synced the remaining `RoomCodeSubsystem.cpp` and `.h` changes from local development commit `e4a3dca`: reset ignored movement/look input when entering the match and restore game-only input after pawn possession. Other published gameplay source already matched that development commit (except the documented removal of a workstation path from a comment). Original development history, assets and private runtime launchers remain excluded.

Validation of this update: Unreal 5.8 Editor build **succeeded** (62 compile/link/metadata actions). New changes contain only the two source files and this text record, with no binary assets or credential-format matches. No multiplayer runtime acceptance is implied.
