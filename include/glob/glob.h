
#pragma once
#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef GLOB_USE_GHC_FILESYSTEM
#include <ghc/filesystem.hpp>
#else
#include <filesystem>
#endif

namespace glob {

#ifdef GLOB_USE_GHC_FILESYSTEM
namespace fs = ghc::filesystem;
#else
namespace fs = std::filesystem;
#endif

/// \param pathname string containing a path specification
/// \return vector of paths that match the pathname
///
/// Pathnames can be absolute (/usr/src/Foo/Makefile) or relative (../../Tools/*/*.gif)
/// Pathnames can contain shell-style wildcards
/// Broken symlinks are included in the results (as in the shell)
std::vector<fs::path> glob(const std::string &pathname);

/// \param pathnames string containing a path specification
/// \return vector of paths that match the pathname
///
/// Globs recursively.
/// The pattern “**” will match any files and zero or more directories, subdirectories and
/// symbolic links to directories.
std::vector<fs::path> rglob(const std::string &pathname);

/// 取消异常: 遍历过程中检测到取消标记置位时抛出。
/// 用于将递归遍历 (rglob / **) 中断, 避免超时/取消后后台任务继续占用线程。
class cancelled_error : public std::runtime_error {
public:
  explicit cancelled_error(const std::string &what) : std::runtime_error(what) {}
};

/// 支持取消的版本: 遍历期间定期检查 `cancel_flag` (每个目录条目/递归层级),
/// 置位时抛出 `cancelled_error`。用于将长时间的全目录递归遍历提前中断。
std::vector<fs::path> glob(const std::string &pathname, const std::atomic<bool> &cancel_flag);

/// 支持取消的版本, 语义同 `rglob`, 详见 `glob` 取消版说明。
std::vector<fs::path> rglob(const std::string &pathname, const std::atomic<bool> &cancel_flag);

/// Runs `glob` against each pathname in `pathnames` and accumulates the results
std::vector<fs::path> glob(const std::vector<std::string> &pathnames);

/// Runs `rglob` against each pathname in `pathnames` and accumulates the results
std::vector<fs::path> rglob(const std::vector<std::string> &pathnames);

/// Initializer list overload for convenience
std::vector<fs::path> glob(const std::initializer_list<std::string> &pathnames);

/// Initializer list overload for convenience
std::vector<fs::path> rglob(const std::initializer_list<std::string> &pathnames);

} // namespace glob
