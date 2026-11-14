#pragma once

#include <QFile>
#include <QString>

namespace NativeBinary {

// Return the x86 word size only for a complete, little-endian ELF header.
inline int architecture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray header = file.read(64);
    if (header.size() < 52 || header.left(4) != QByteArray("\x7f" "ELF", 4) ||
        header[5] != 1 || header[6] != 1)
        return 0;
    const auto machine = static_cast<unsigned char>(header[18]) |
                         (static_cast<unsigned char>(header[19]) << 8);
    if (header[4] == 1 && machine == 3)
        return 32;
    if (header.size() == 64 && header[4] == 2 && machine == 62)
        return 64;
    return 0;
}

inline bool compatible(const QString& binary, const QString& target) {
    const int targetArchitecture = architecture(target);
    return targetArchitecture != 0 && architecture(binary) == targetArchitecture;
}

} // namespace NativeBinary
