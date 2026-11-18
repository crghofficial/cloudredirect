# Linux x86-64 Steam client support

For Steam's `steamrt64` client, the library and CLI must both target x86-64. The legacy i386 build remains available with `LINUX_32BIT=ON`.

## Build for Steam Runtime

Run `scripts/build-linux64.sh` from the repository root. The script downloads checksum-pinned dependencies into `build-deps/` and builds against a glibc 2.31 sysroot. Required host tools are clang, CMake, Ninja, make, ar, tar, Perl and Python 3. Output files are `build-steamrt64/cloud_redirect.so` and `build-steamrt64/cloud_redirect_cli`.

The script links curl, OpenSSL and libstdc++ privately. `scripts/check-linux64-abi.py` checks the resulting ELF architecture, symbol-version requirements and CLI export before deployment. The tested library requires GLIBC 2.30.

## Flatpak packaging

Run `scripts/stage-flatpak.sh` after the isolated build. It validates both ELF files and their symbol versions before copying them into the existing Flatpak manifest inputs. Then run `flatpak-builder --force-clean flatpak/build-dir flatpak/org.cloudredirect.CloudRedirect.yml` with the KDE 6.10 SDK installed.

The UI checks the library and CLI against the installed SLSsteam architecture during deployment and updates. Deployment status describes the installed files. Hook initialization is reported by Steam at startup. Native file replacements use atomic per-file commits.

## Runtime changes

The x86-64 path resolves protobuf helpers and KeyValues functions through unique instruction signatures. It uses pointer-width vtables, the SysV bool/size_t calling conventions and the current CAppInfoCache tree layout. Anonymous BSS mappings are included and clipped to the ELF image boundary. Missing or ambiguous required signatures stop initialization before hook installation.

The launcher keeps `LD_PRELOAD` for Steam's pressure-vessel handoff. A launcher that exits before mapping `steamclient.so` cancels its wait during shutdown. The x86-64 transport resolver requires a single vtable candidate with all three hook entries in executable mappings. RemoteStorage discovery accepts an existing SLSsteam entry detour.

On Linux, AutoCloud looks for a Proton prefix in the library containing the game's app manifest. This covers mounted external libraries whose paths appear in `libraryfolders.vdf`. The Steam client and host CLI need access to that mount.

Legacy i386 GamesPlayed/LivePlaytime inline observers are skipped on x86-64. Automatic playtime capture needs a separate port. Save synchronization runs independently.

## Verification

The isolated build runs CTest for the pending-operation journal, resolver rejection cases, corrupt tree pointers, concurrent protobuf calls, launcher exit and external Proton library paths. Native builds with Qt Core also test architecture validation and file replacement failures.

The Linux workflow builds x86-64 and i386 libraries and runs their tests on each pull request. It also builds the Qt UI and produces Steam-compatible native artifacts. These jobs do not require a Steam account or cloud credentials.

For an installed Steam client, run the additional read-only probes with its absolute `steamclient.so` path and the Steam library directory in `LD_LIBRARY_PATH`.

```sh
build-steamrt64/linux_kv64_resolver_tests /path/to/steamrt64/steamclient.so
LD_LIBRARY_PATH=/path/to/steamrt64 build-steamrt64/linux_vtable_probe /path/to/steamrt64/steamclient.so
LD_LIBRARY_PATH=/path/to/steamrt64 build-steamrt64/linux64_steam_protobuf_smoke /path/to/steamrt64/steamclient.so
```

These probes passed against Steam builds 1792618285 and 1794536281. The protobuf test creates and round-trips an actual cloud request in a separate process. It selects its test vtable address only after matching the ELF SHA-256 and also checks that vtable's RTTI before calling it. Production resolution uses the same signatures for both builds.

Earlier live validation on 1792618285 with SLSsteam confirmed five downloads and an upload. Live sync on 1794536281, SteamOS Gaming Mode and a legacy i386 Steam session still need testing. The Flatpak preview was installed and launched in isolation. Future clients may need ABI verification when Valve changes internal layouts.
