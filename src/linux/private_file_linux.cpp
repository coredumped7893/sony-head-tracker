// private_file_linux.cpp
// Private (0600 file, 0700 directory) atomic file persistence for Linux.
#include "sony_head_tracker/linux_private_file.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sony::linux_platform {

namespace {

constexpr mode_t kPrivateDirectoryMode = 0700;
constexpr mode_t kPrivateFileMode = 0600;
constexpr std::size_t kMaximumFileBytes = 1024 * 1024;

bool ensurePrivateDirectory(const std::filesystem::path& directory) {
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    // Ancestor symlinks are permitted; only the application directory itself
    // and the final file are security boundaries.
    struct stat status{};
    if (::lstat(directory.c_str(), &status) != 0 || S_ISLNK(status.st_mode) ||
        !S_ISDIR(status.st_mode)) {
        return false;
    }
    return ::chmod(directory.c_str(), kPrivateDirectoryMode) == 0;
}

bool finalPathIsSafe(int directoryFd, const std::string& filename) {
    struct stat status{};
    if (::fstatat(directoryFd, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        return S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode);
    }
    return errno == ENOENT;
}

std::string randomSuffix() {
    std::array<std::uint8_t, 16> bytes{};
    arc4random_buf(bytes.data(), bytes.size());
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

bool writeAll(int file, std::string_view content) {
    std::size_t offset{};
    while (offset < content.size()) {
        const auto remaining = content.size() - offset;
        const auto written = ::write(file, content.data() + offset, remaining);
        if (written > 0 && static_cast<std::size_t>(written) <= remaining) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

} // namespace

bool writePrivateFileAtomically(const std::filesystem::path& path, std::string_view content) {
    const auto directory = path.parent_path();
    const auto filename = path.filename().string();
    if (filename.empty() || filename == "." || filename == ".." ||
        !ensurePrivateDirectory(directory)) {
        return false;
    }

    const int directoryFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) return false;
    const auto closeDirectory = [&] { ::close(directoryFd); };
    if (!finalPathIsSafe(directoryFd, filename)) {
        closeDirectory();
        return false;
    }

    int temporaryFd = -1;
    std::string temporaryName;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temporaryName = "." + filename + ".tmp." + randomSuffix();
        temporaryFd = ::openat(directoryFd, temporaryName.c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                               kPrivateFileMode);
        if (temporaryFd >= 0) break;
        if (errno != EEXIST) {
            closeDirectory();
            return false;
        }
    }
    if (temporaryFd < 0) {
        closeDirectory();
        return false;
    }

    const auto cleanup = [&] {
        if (temporaryFd >= 0) ::close(temporaryFd);
        ::unlinkat(directoryFd, temporaryName.c_str(), 0);
        closeDirectory();
    };
    if (!writeAll(temporaryFd, content) || ::fsync(temporaryFd) != 0 ||
        ::fchmod(temporaryFd, kPrivateFileMode) != 0) {
        cleanup();
        return false;
    }
    if (::close(temporaryFd) != 0) {
        temporaryFd = -1;
        cleanup();
        return false;
    }
    temporaryFd = -1;

    if (::renameat(directoryFd, temporaryName.c_str(), directoryFd, filename.c_str()) != 0) {
        cleanup();
        return false;
    }
    if (::fsync(directoryFd) != 0) {
        closeDirectory();
        return false;
    }
    closeDirectory();
    return true;
}

std::string readPrivateFile(const std::filesystem::path& path) {
    const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) return {};
    struct stat status{};
    if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > kMaximumFileBytes) {
        ::close(file);
        return {};
    }
    std::string content(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset{};
    while (offset < content.size()) {
        const auto count = ::read(file, content.data() + offset, content.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        ::close(file);
        return {};
    }
    ::close(file);
    return content;
}

} // namespace sony::linux_platform
