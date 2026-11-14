#include "native_binary.h"
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

static void require(bool condition) {
    if (!condition) {
        std::fputs("ELF architecture validation failed\n", stderr);
        std::exit(1);
    }
}

int main() {
    QTemporaryDir directory;
    require(directory.isValid());
    const auto writeHeader = [&](const QString& name, int wordSize, int machine, int length) {
        QByteArray data(length, '\0');
        if (length >= 20) {
            data.replace(0, 4, QByteArray("\x7f" "ELF", 4));
            data[4] = wordSize == 64 ? 2 : 1;
            data[5] = 1;
            data[6] = 1;
            data[18] = machine;
        }
        const QString path = directory.filePath(name);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly));
        require(file.write(data) == data.size());
        return path;
    };
    const auto x64 = writeHeader("x64", 64, 62, 64);
    const auto x86 = writeHeader("x86", 32, 3, 52);
    require(NativeBinary::compatible(x64, x64));
    require(NativeBinary::compatible(x86, x86));
    require(!NativeBinary::compatible(x64, x86));
    require(!NativeBinary::compatible(x86, x64));
    require(NativeBinary::architecture(writeHeader("arm", 64, 183, 64)) == 0);
    require(NativeBinary::architecture(writeHeader("truncated", 64, 62, 52)) == 0);
    require(!NativeBinary::compatible(directory.filePath("missing"), x64));
    std::puts("Native binary architecture checks passed");
}
