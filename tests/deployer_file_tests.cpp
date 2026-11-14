#include "../ui-linux/src/deployer.cpp"
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

static void require(bool condition) {
    if (!condition) {
        std::fputs("Native file deployment failed\n", stderr);
        std::exit(1);
    }
}

static QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

int main() {
    QTemporaryDir directory;
    require(directory.isValid());
    const QString source = directory.filePath("source");
    const QString destination = directory.filePath("installed");
    QFile file(source);
    require(file.open(QIODevice::WriteOnly));
    const QByteArray payload(200000, 'x');
    require(file.write(payload) == payload.size());
    file.close();
    require(copyNativeBinary(source, destination));
    require(readFile(destination) == payload);
    require(QFile::permissions(destination).testFlag(QFileDevice::ExeOwner));
    require(!copyNativeBinary(directory.filePath("missing"), destination));
    require(readFile(destination) == payload);
    require(!copyNativeBinary(source, directory.filePath("missing/destination")));
    require(getSoVersion(directory.filePath("missing")).isEmpty());
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    require(file.write("short") == 5);
    file.close();
    require(getSoVersion(source).isEmpty());
    std::puts("Native replacement and missing-source checks passed");
}
