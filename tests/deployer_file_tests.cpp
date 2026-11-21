#include "deployer.h"
#include "utils.h"

#include <QCoreApplication>
#include <QFileInfo>
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
    CHECK(writeFile(data + "/Steam/steam.sh", "LD_AUDIT=SLSsteam.so"));

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

    CHECK(QFile::remove(so));
    CHECK(!deployer.update());
    CHECK(readFile(installed) == library && readFile(installedCli) == command);
    CHECK(writeFile(so, library));
    CHECK(QFile::remove(installedCli) && QDir().mkdir(installedCli));
    CHECK(!deployer.update());
    CHECK(readFile(installed) == library && QFileInfo(installedCli).isDir());
    return 0;
}
