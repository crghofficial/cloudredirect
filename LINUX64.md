# Linux x86-64 Steam client support

Steam build 1792618285 uses a 64-bit client. The injected library and CLI must both target x86-64. The legacy i386 build remains available with `LINUX_32BIT=ON`.

## Build for Steam Runtime

Run `scripts/build-linux64.sh` from the repository root. The script downloads checksum-pinned dependencies into `build-deps/` and builds against a glibc 2.31 sysroot. Required host tools are clang, CMake, Ninja, make, ar, tar, Perl and Python 3. Output files are `build-steamrt64/cloud_redirect.so` and `build-steamrt64/cloud_redirect_cli`.

The script links curl, OpenSSL and libstdc++ privately. `scripts/check-linux64-abi.py` checks the resulting ELF architecture, symbol-version requirements and CLI export before deployment. The tested library requires GLIBC 2.30.

## Flatpak packaging

Run `scripts/stage-flatpak.sh` after the isolated build. It validates both ELF files and their symbol versions before copying them into the existing Flatpak manifest inputs. Then run `flatpak-builder --force-clean flatpak/build-dir flatpak/org.cloudredirect.CloudRedirect.yml` with the KDE 6.10 SDK installed.

The UI checks the library and CLI against the installed SLSsteam architecture during deployment and updates. Deployment status describes the installed files. Hook initialization is reported by Steam at startup. Native file replacements use atomic per-file commits.

## Runtime changes

The x86-64 path resolves protobuf helpers and KeyValues functions through unique instruction signatures. It uses pointer-width vtables, the SysV bool/size_t calling conventions and the current CAppInfoCache tree layout. Anonymous BSS mappings are included and clipped to the ELF image boundary. Missing or ambiguous required signatures stop initialization before hook installation.

The preload survives Steam's pressure-vessel bootstrap. RemoteStorage method discovery accepts an existing SLSsteam entry detour while retaining its original passthrough behavior.

Legacy i386 GamesPlayed/LivePlaytime inline observers are skipped on x86-64. Automatic playtime capture needs a separate port. Save synchronization runs independently.

## Verification

The isolated build runs CTest for the pending-operation journal, resolver rejection cases, corrupt tree pointers and concurrent protobuf calls. Native builds with Qt Core also test architecture validation and file replacement failures.

The Linux workflow builds x86-64 and i386 libraries and runs their tests on each pull request. It also builds the Qt UI and produces Steam-compatible native artifacts. These jobs do not require a Steam account or cloud credentials.

For an installed Steam client, run the additional read-only probes with its absolute `steamclient.so` path and the Steam library directory in `LD_LIBRARY_PATH`.

```sh
build-steamrt64/linux_kv64_resolver_tests /path/to/steamrt64/steamclient.so
LD_LIBRARY_PATH=/path/to/steamrt64 build-steamrt64/linux_vtable_probe /path/to/steamrt64/steamclient.so
```

`tests/linux64_steam_protobuf_smoke.cpp` also exercises a real cloud protobuf request in a separate process. Its fixture address is guarded by the complete tested ELF SHA-256 and RTTI identity.

Live validation on Steam 1792618285 with SLSsteam confirmed five completed game downloads, a completed upload, Google Drive blob transfers and manifest publication. Fresh in-game saves, conflict recovery and other Steam builds remain untested. The i386 library builds and its CLI loads, but an old Steam session has not been exercised. A complete Flatpak bundle installation has not been tested. Future Steam builds with missing or ambiguous signatures are rejected and need validation before use.
