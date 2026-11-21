#pragma once

#include <QByteArray>
#include <QString>

namespace SteamLauncher
{
    enum class State { Unsupported, NeedsRepair, Configured };

    // Edit only a recognized native Headcrab wrapper.
    State inspect(const QString &path, const QString &library);
    bool repair(const QString &path, const QString &library);
    bool remove(const QString &path, const QString &library);
}
