#pragma once

#include <cstdint>
#include <cstddef>

namespace CloudHooks
{
    // Store originals before publishing hooks to Steam.
    void SetOriginals(void* origSlot5, void* origSlot7, void* origSlot8);

    // Called by the vtable installer before publishing IsCloudEnabledForApp.
    void SetOriginalIsCloudEnabled(void* orig);

    // Resolve protobuf helper functions from the loaded steamclient.so.
    // Must be called after steamclient is loaded. Returns true on success.
    bool ResolveProtobufHelpers(void* steamclientBase, size_t steamclientSize);

    // Install the GamesPlayed observer (playtime tracking) using the resolved
    // protobuf serializer. False means a failed patch could not be rolled back.
    bool InstallGamesPlayedObserver(uintptr_t steamclientBase, size_t steamclientSize);

    // Publish interception only after startup has completed.
    void Enable();

    // Stop interception and drain hooks. Init rollback must not apply queued
    // playtime writes from the initialization thread.
    bool BeginShutdown(bool drainPendingPlaytime = true);
}
