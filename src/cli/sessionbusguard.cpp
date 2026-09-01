#include "sessionbusguard.h"

#include <QList>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tryx::cli {

namespace {

constexpr char kUnixPathPrefix[] = "unix:path=";
constexpr char kGuidPrefix[] = "guid=";
constexpr qsizetype kMaximumAddressBytes = 4096;

class ScopedFileDescriptor final {
public:
    explicit ScopedFileDescriptor(int descriptor = -1)
        : descriptor_(descriptor) {}

    ~ScopedFileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor &) = delete;
    ScopedFileDescriptor &operator=(
        const ScopedFileDescriptor &) = delete;

    int get() const {
        return descriptor_;
    }

    void reset(int descriptor) {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

    int release() {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

private:
    int descriptor_ = -1;
};

int hexadecimalValue(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool isUnescapedAddressValueByte(char character) {
    return (character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        character == '_' || character == '-' ||
        character == '/' || character == '.' ||
        character == '\\' || character == '*';
}

bool decodeAddressValue(
    const QByteArray &encoded,
    QByteArray *decoded) {
    if (!decoded || encoded.isEmpty()) {
        return false;
    }

    decoded->clear();
    decoded->reserve(encoded.size());
    for (qsizetype index = 0; index < encoded.size(); ++index) {
        unsigned char value = 0;
        const char character = encoded.at(index);
        if (character == '%') {
            if (index + 2 >= encoded.size()) {
                return false;
            }
            const int high = hexadecimalValue(encoded.at(index + 1));
            const int low = hexadecimalValue(encoded.at(index + 2));
            if (high < 0 || low < 0) {
                return false;
            }
            value = static_cast<unsigned char>((high << 4) | low);
            index += 2;
        } else {
            if (!isUnescapedAddressValueByte(character)) {
                return false;
            }
            value = static_cast<unsigned char>(character);
        }

        if (value == 0 || value <= 0x1f || value == 0x7f) {
            return false;
        }
        decoded->append(static_cast<char>(value));
    }
    return !decoded->isEmpty();
}

bool guidIsValid(const QByteArray &field) {
    if (!field.startsWith(kGuidPrefix)) {
        return false;
    }
    const QByteArray guid = field.mid(
        static_cast<qsizetype>(sizeof(kGuidPrefix) - 1));
    if (guid.size() != 32) {
        return false;
    }
    for (const char character : guid) {
        if (hexadecimalValue(character) < 0) {
            return false;
        }
    }
    return true;
}

bool directoryIsTrusted(
    const struct stat &metadata,
    uid_t effectiveUserId) {
    if (!S_ISDIR(metadata.st_mode) ||
        (metadata.st_uid != 0 &&
         metadata.st_uid != effectiveUserId)) {
        return false;
    }

    const bool writableByOthers =
        (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    if (!writableByOthers) {
        return true;
    }
    return metadata.st_uid == 0 &&
        (metadata.st_mode & S_ISVTX) != 0;
}

bool socketPathIsTrusted(const QByteArray &path) {
    const uid_t realUserId = ::getuid();
    const uid_t effectiveUserId = ::geteuid();
    if (effectiveUserId == 0 || realUserId != effectiveUserId ||
        !path.startsWith('/')) {
        return false;
    }

    const QList<QByteArray> components = path.split('/');
    if (components.size() < 2 || !components.first().isEmpty()) {
        return false;
    }
    for (qsizetype index = 1; index < components.size(); ++index) {
        const QByteArray &component = components.at(index);
        if (component.isEmpty() || component == "." ||
            component == "..") {
            return false;
        }
    }

    ScopedFileDescriptor current(
        ::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0) {
        return false;
    }
    struct stat rootMetadata {};
    if (::fstat(current.get(), &rootMetadata) != 0 ||
        !directoryIsTrusted(rootMetadata, effectiveUserId)) {
        return false;
    }

    for (qsizetype index = 1; index < components.size(); ++index) {
        const bool isFinal = index + 1 == components.size();
        const int flags = O_PATH | O_NOFOLLOW | O_CLOEXEC |
            (isFinal ? 0 : O_DIRECTORY);
        ScopedFileDescriptor next(::openat(
            current.get(), components.at(index).constData(), flags));
        if (next.get() < 0) {
            return false;
        }

        struct stat metadata {};
        if (::fstat(next.get(), &metadata) != 0) {
            return false;
        }
        if (isFinal) {
            return S_ISSOCK(metadata.st_mode) &&
                metadata.st_uid == effectiveUserId;
        }
        if (!directoryIsTrusted(metadata, effectiveUserId)) {
            return false;
        }
        current.reset(next.release());
    }
    return false;
}

}  // namespace

bool localSessionBusAddressIsSafe(const QByteArray &address) {
    if (address.isEmpty() ||
        address.size() > kMaximumAddressBytes ||
        address.contains(';')) {
        return false;
    }

    const QList<QByteArray> fields = address.split(',');
    if (fields.isEmpty() || fields.size() > 2 ||
        !fields.first().startsWith(kUnixPathPrefix) ||
        (fields.size() == 2 && !guidIsValid(fields.at(1)))) {
        return false;
    }

    const QByteArray encodedPath = fields.first().mid(
        static_cast<qsizetype>(sizeof(kUnixPathPrefix) - 1));
    QByteArray decodedPath;
    if (!decodeAddressValue(encodedPath, &decodedPath)) {
        return false;
    }
    return socketPathIsTrusted(decodedPath);
}

}  // namespace tryx::cli
