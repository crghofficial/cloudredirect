#include "deployer.h"
#include "utils.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (false)

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.flush();
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int main(int argc, char **argv)
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString data = dir.filePath(".local/share");
    qputenv("HOME", dir.path().toUtf8());
    qputenv("HOST_HOME", dir.path().toUtf8());
    qputenv("XDG_DATA_HOME", data.toUtf8());
    qputenv("XDG_CONFIG_HOME", dir.filePath(".config").toUtf8());
    const QString so = dir.filePath("bundled.so");
    const QString cli = dir.filePath("bundled-cli");
    qputenv("CR_BUNDLED_SO", so.toUtf8());
    qputenv("CR_BUNDLED_CLI", cli.toUtf8());
    QCoreApplication app(argc, argv);
    CHECK(crDataDir() == data + "/CloudRedirect");
    CHECK(crConfigDir() == dir.filePath(".config/CloudRedirect"));
    CHECK(QDir().mkpath(data + "/SLSsteam"));
    CHECK(QDir().mkpath(data + "/Steam"));
    const QString launcher = data + "/Steam/steam.sh";
    const QString clientDir = dir.filePath(".steam/steam");
    CHECK(QDir().mkpath(clientDir));
    const QByteArray wrapper =
        "#!/usr/bin/env bash\n"
        "    STEAM_CLIENT=\"$HOME/.steam/steam/client.sh\"\n"
        "    INJECT_SLS=\"LD_AUDIT=$HOME/.local/share/SLSsteam/SLSsteam.so\"\n"
        "GameLauncher(){\n"
        "    echo \"Loaded SLSsteam\" & export $INJECT_SLS &> /dev/null\n"
        "    source $STEAM_CLIENT \"$@\" &> /dev/null\n"
        "}\n"
        "GameLauncher \"$@\"\n";
    const auto launcherMode = QFileDevice::ReadOwner | QFileDevice::ExeOwner;
    CHECK(writeFile(launcher, wrapper));
    CHECK(QFile::setPermissions(launcher, launcherMode));
    const QString result = dir.filePath("result");
    CHECK(writeFile(clientDir + "/client.sh",
        "printf '%s\\n' \"${LD_PRELOAD-}\" \"${LD_AUDIT-}\" \"$@\" > \"$HOME/result\"\n"));
    const auto launch = [&](const QString &preload, const QStringList &args) {
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.remove("LD_PRELOAD");
        env.remove("LD_AUDIT");
        process.setProcessEnvironment(env);
        QStringList argv{"-c", "export LD_PRELOAD=\"$1\"; script=\"$2\"; shift 2; source \"$script\" \"$@\"", "bash", preload, launcher};
        process.start("/bin/bash", argv + args);
        return process.waitForFinished(5000) && process.exitCode() == 0;
    };
    const QByteArray audit = (data + "/SLSsteam/SLSsteam.so").toUtf8();
    CHECK(launch("first.so:second.so", {"-gamepadui"}));
    CHECK(readFile(result) == "first.so:second.so\n" + audit + "\n-gamepadui\n");

    QByteArray library(200000, '\0');
    library.replace(0, 7, QByteArray::fromHex("7f454c46020101"));
    library[18] = 62; // EM_X86_64
    library.append("2.6.6+abcdef0");
    const QByteArray command = library.left(64);
    CHECK(writeFile(data + "/SLSsteam/SLSsteam.so", command));
    CHECK(writeFile(cli, command));
    CHECK(writeFile(so, "short"));
    Deployer deployer;
    CHECK(deployer.bundledVersion().isEmpty());
    CHECK(!deployer.deploy());
    CHECK(writeFile(so, library));
    deployer.checkPrerequisites();
    CHECK(deployer.bundledVersion() == "2.6.6+abcdef0");
    CHECK(deployer.deploy());
    CHECK(deployer.deployedVersion() == deployer.bundledVersion());
    const QString installed = crDataDir() + "/cloud_redirect.so";
    const QString installedCli = crDataDir() + "/cloud_redirect_cli";
    CHECK(deployer.launcherConfigured());
    CHECK(QFileInfo(launcher).isExecutable() && !(QFile::permissions(launcher) & QFileDevice::WriteOwner));
    const QByteArray repaired = readFile(launcher);
    CHECK(repaired.count("# BEGIN CloudRedirect preload") == 1);
    CHECK(launch("first.so:second.so", {"-gamepadui", "arg with spaces"}));
    CHECK(readFile(result) == installed.toUtf8() + ":first.so:second.so\n" + audit + "\n-gamepadui\narg with spaces\n");
    CHECK(launch("first.so " + installed, {}));
    CHECK(readFile(result) == "first.so " + installed.toUtf8() + '\n' + audit + '\n');
    CHECK(deployer.update());
    CHECK(readFile(launcher) == repaired);
    CHECK(readFile(installed) == library);
    CHECK(readFile(installedCli) == command);
    CHECK(QFileInfo(installedCli).isExecutable());
    deployer.checkPrerequisites();
    CHECK(deployer.alreadyDeployed() && !deployer.updateAvailable());

    CHECK(QFile::remove(installedCli));
    deployer.checkPrerequisites();
    CHECK(deployer.updateAvailable());
    CHECK(deployer.update());
    CHECK(readFile(installedCli) == command);
    QByteArray legacyCli = command;
    legacyCli[4] = 1;
    legacyCli[18] = 3; // EM_386
    CHECK(writeFile(installedCli, legacyCli));
    deployer.checkPrerequisites();
    CHECK(deployer.updateAvailable());
    CHECK(deployer.update());
    CHECK(QFile::setPermissions(installedCli, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    deployer.checkPrerequisites();
    CHECK(deployer.updateAvailable());
    CHECK(deployer.update());
    deployer.checkPrerequisites();
    CHECK(!deployer.updateAvailable() && QFileInfo(installedCli).isExecutable());

    CHECK(QFile::setPermissions(launcher, launcherMode | QFileDevice::WriteOwner));
    CHECK(writeFile(launcher, wrapper));
    const QString steamDir = QFileInfo(launcher).absolutePath();
    const auto dirMode = QFile::permissions(steamDir);
    CHECK(QFile::setPermissions(steamDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    CHECK(!deployer.repairLauncher());
    CHECK(deployer.launcherRepairAvailable() && readFile(launcher) == wrapper);
    CHECK(QFile::setPermissions(steamDir, dirMode));

    // Headcrab can replace steam.sh. Refresh offers repair without recopying the library.
    CHECK(QFile::setPermissions(launcher, launcherMode | QFileDevice::WriteOwner));
    CHECK(writeFile(launcher, wrapper));
    CHECK(QFile::setPermissions(launcher, launcherMode));
    deployer.checkPrerequisites();
    CHECK(deployer.launcherRepairAvailable() && !deployer.launcherConfigured());
    CHECK(deployer.repairLauncher());
    CHECK(readFile(launcher) == repaired);
    CHECK(deployer.undeploy());
    CHECK(readFile(launcher) == wrapper && !QFile::exists(installed) && !QFile::exists(installedCli));
    CHECK(deployer.deploy());
    CHECK(readFile(launcher) == repaired);

    // Refuse to overwrite a locally edited managed block, and retain the files.
    CHECK(QFile::setPermissions(launcher, launcherMode | QFileDevice::WriteOwner));
    QByteArray edited = repaired;
    edited.replace("unset _cloudredirect_so", "unset _other_and_cloudredirect");
    CHECK(writeFile(launcher, edited));
    CHECK(!deployer.undeploy());
    CHECK(readFile(launcher) == edited && QFile::exists(installed));
    CHECK(writeFile(launcher, repaired));
    CHECK(deployer.undeploy());

    auto legacy = wrapper;
    legacy.replace("GameLauncher(){", "    INJECT_CR=\"LD_PRELOAD=$HOME/.local/share/CloudRedirect/cloud_redirect.so\"\nGameLauncher(){");
    legacy.replace("    source ", "    echo \"Loaded CloudRedirect\" & export $INJECT_CR &> /dev/null\n    source ");
    CHECK(writeFile(launcher, legacy));
    CHECK(deployer.deploy());
    CHECK(launch("first.so:second.so", {"-silent"}));
    CHECK(readFile(result) == installed.toUtf8() + ":first.so:second.so\n" + audit + "\n-silent\n");
    CHECK(deployer.undeploy());
    CHECK(readFile(launcher) == legacy);

    auto unknown = wrapper;
    unknown.replace("    source ", "    export LD_PRELOAD=custom.so\n    source ");
    CHECK(writeFile(launcher, unknown));
    CHECK(deployer.deploy());
    CHECK(!deployer.launcherConfigured() && !deployer.launcherRepairAvailable());
    CHECK(readFile(launcher) == unknown);
    CHECK(deployer.undeploy());
    CHECK(writeFile(launcher, wrapper));
    CHECK(deployer.deploy());

    CHECK(QFile::remove(so));
    CHECK(!deployer.update());
    CHECK(readFile(installed) == library && readFile(installedCli) == command);
    CHECK(writeFile(so, library));
    CHECK(QFile::remove(installedCli) && QDir().mkdir(installedCli));
    CHECK(!deployer.update());
    CHECK(readFile(installed) == library && QFileInfo(installedCli).isDir());
    return 0;
}
