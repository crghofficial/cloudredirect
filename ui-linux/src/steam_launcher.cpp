#include "steam_launcher.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <cstdio>
#include <unistd.h>

namespace
{
const QByteArray beginMarker = "# BEGIN CloudRedirect preload\n";
const QByteArray endMarker = "# END CloudRedirect preload\n";
const QByteArray originalPrefix = "# CloudRedirect original: ";

struct Launcher
{
    QString path;
    QByteArray current;
    QByteArray original;
    QByteArray configured;
    bool managed = false;
};

QByteArray preloadBlock(const QString &library, const QByteArray &legacy)
{
    QByteArray quoted = QFile::encodeName(library);
    quoted.replace("'", "'\\''");
    QByteArray block = beginMarker;
    if (!legacy.isEmpty())
        block += originalPrefix + legacy + '\n';
    block += "_cloudredirect_so='" + quoted + "'\n";
    block += "if [ -r \"$_cloudredirect_so\" ]; then\n"
             "    case \":${LD_PRELOAD// /:}:\" in\n"
             "        *\":$_cloudredirect_so:\"*) ;;\n"
             "        *) export LD_PRELOAD=\"$_cloudredirect_so${LD_PRELOAD:+:$LD_PRELOAD}\" ;;\n"
             "    esac\n"
             "fi\n"
             "unset _cloudredirect_so\n";
    return block + endMarker;
}

Launcher readLauncher(const QString &path, const QString &library)
{
    Launcher launcher;
    if (path.isEmpty()) return launcher;
    const QFileInfo requested(path);
    if (requested.isSymLink()) return launcher;
    launcher.path = requested.canonicalFilePath();
    QFile file(launcher.path);
    if (launcher.path.isEmpty() || !QFileInfo(file).isFile() || QFileInfo(file).ownerId() != geteuid() ||
        !file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024)
        return launcher;
    launcher.current = file.readAll();

    const qsizetype begin = launcher.current.indexOf("# BEGIN CloudRedirect preload");
    launcher.managed = begin >= 0 || launcher.current.contains("# END CloudRedirect preload");
    if (library.contains(QRegularExpression(QStringLiteral("[:\\s]"))) ||
        launcher.current.contains('\0') || !launcher.current.startsWith("#!/usr/bin/env bash\n"))
        return launcher;

    launcher.original = launcher.current;
    if (launcher.managed) {
        const qsizetype end = launcher.current.indexOf(endMarker, begin);
        if (begin < 0 || end < 0 || (begin != 0 && launcher.current[begin - 1] != '\n'))
            return launcher;
        const qsizetype content = begin + beginMarker.size();
        QByteArray legacy;
        if (launcher.current.mid(content).startsWith(originalPrefix)) {
            const qsizetype start = content + originalPrefix.size();
            legacy = launcher.current.mid(start, launcher.current.indexOf('\n', start) - start);
        }
        const qsizetype size = end + endMarker.size() - begin;
        if (launcher.current.mid(begin, size) != preloadBlock(library, legacy))
            return launcher;
        launcher.original.replace(begin, size, legacy.isEmpty() ? QByteArray() : legacy + '\n');
    }

    auto lines = launcher.original.split('\n');
    if (!lines.contains("    STEAM_CLIENT=\"$HOME/.steam/steam/client.sh\"") ||
        (!lines.contains("    INJECT_SLS=\"LD_AUDIT=$HOME/.local/share/SLSsteam/SLSsteam.so\"") &&
         !lines.contains("    INJECT_SLS=\"LD_AUDIT=$HOME/.local/share/SLSsteam/library-inject.so:$HOME/.local/share/SLSsteam/SLSsteam.so\"")))
        return launcher;

    constexpr auto legacyDefinition = "INJECT_CR=\"LD_PRELOAD=$HOME/.local/share/CloudRedirect/cloud_redirect.so\"";
    constexpr auto legacyExport = "echo \"Loaded CloudRedirect\" & export $INJECT_CR &> /dev/null";
    constexpr auto auditExport = "echo \"Loaded SLSsteam\" & export $INJECT_SLS &> /dev/null";
    qsizetype anchor = -1, legacyLine = -1;
    int definitions = 0;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line == "source $STEAM_CLIENT \"$@\" &> /dev/null" ||
            line == "source \"$STEAM_CLIENT\" \"$@\" &> /dev/null") {
            if (anchor >= 0) return launcher;
            anchor = i;
        } else if (line == legacyDefinition) {
            ++definitions;
        } else if (line == legacyExport && legacyLine < 0) {
            legacyLine = i;
        } else if (line.contains("LD_PRELOAD") || line.contains("INJECT_CR") ||
                   line.contains("cloud_redirect.so")) {
            return launcher;
        }
    }
    const qsizetype audit = legacyLine >= 0 ? anchor - 2 : anchor - 1;
    if (audit < 0 || lines[audit].trimmed() != auditExport ||
        (legacyLine >= 0 ? definitions != 1 || legacyLine != anchor - 1 : definitions != 0))
        return launcher;

    if (legacyLine >= 0)
        lines[legacyLine] = preloadBlock(library, lines[legacyLine]).chopped(1);
    else
        lines.insert(anchor, preloadBlock(library, {}).chopped(1));
    launcher.configured = lines.join('\n');
    if (launcher.managed && launcher.configured != launcher.current)
        launcher.configured.clear();
    return launcher;
}

bool replaceLauncher(const Launcher &launcher, const QByteArray &contents)
{
    if (contents == launcher.current)
        return true;

    // Headcrab marks steam.sh read-only. Replace it atomically and retain its
    // mode, without making the original writable or changing symlinks.
    QFile file(launcher.path);
    QTemporaryFile replacement(launcher.path + ".cloudredirect-XXXXXX");
    if (!file.open(QIODevice::ReadOnly) || !replacement.open() ||
        replacement.write(contents) != contents.size() || !replacement.flush() ||
        !replacement.setPermissions(file.permissions()) ||
        file.readAll() != launcher.current)
        return false;
    replacement.close();
    return ::rename(QFile::encodeName(replacement.fileName()).constData(),
                    QFile::encodeName(launcher.path).constData()) == 0;
}
}

SteamLauncher::State SteamLauncher::inspect(const QString &path, const QString &library)
{
    const auto launcher = readLauncher(path, library);
    if (launcher.configured.isEmpty()) return State::Unsupported;
    return launcher.managed ? State::Configured : State::NeedsRepair;
}

bool SteamLauncher::repair(const QString &path, const QString &library)
{
    const auto launcher = readLauncher(path, library);
    return !launcher.configured.isEmpty() && replaceLauncher(launcher, launcher.configured);
}

bool SteamLauncher::remove(const QString &path, const QString &library)
{
    const auto launcher = readLauncher(path, library);
    return !launcher.managed ||
        (!launcher.configured.isEmpty() && replaceLauncher(launcher, launcher.original));
}
