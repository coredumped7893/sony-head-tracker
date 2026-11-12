// linux_private_file.hpp
// Private, same-directory atomic file replacement used for Linux persistence.
// The namespace is not named `linux`: that is a predefined macro in GNU modes.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sony::linux_platform {

// Creates or replaces a regular final file without following a symlink. The
// completed file is mode 0600 and the containing directory is mode 0700.
bool writePrivateFileAtomically(const std::filesystem::path& path, std::string_view content);

// Reads only a regular, non-symlink final file. An unreadable or unsafe file
// yields an empty string.
std::string readPrivateFile(const std::filesystem::path& path);

} // namespace sony::linux_platform
