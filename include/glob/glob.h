
#pragma once
#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>
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

// ── 模式工具函数 (供上层 filesystem 工具使用) ────────────────────────────────

/// 判断 glob 模式是否包含递归段 `**`。
/// 与 shell globstar / `find -r` 对齐: 只有显式写出 `**` 时才递归遍历子目录,
/// 否则 `*.txt` 这类模式只匹配当前目录 (不递归)。
bool has_recursive_segment(std::string_view pattern) noexcept;

/// 提取 glob 模式中第一个通配符 (`*` `?` `[`) 之前的固定目录前缀。
/// 用于计算 max_depth 的基准目录, 以及 include_hidden 自实现遍历的起点。
/// 例如 `/a/b/*.txt` -> `/a/b/`; `*.txt` -> `.`; `/a/b/c.txt` -> `/a/b`。
fs::path static_prefix(std::string_view pattern);

/// 计算相对路径的目录深度 (段数)。`.` 与空段不计, `..` 计 1。
int path_depth(const fs::path &rel) noexcept;

/// 将 glob 通配模式转换为等价正则表达式字符串 (用于 exclude 过滤等)。
/// 语义从宽: `*` 与 `**` 均匹配任意字符 (含路径分隔符 `/`), `?` 匹配单个字符,
/// `[...]` 字符类原样传递 (`[!...]` 转为 `[^...]`), 其余正则特殊字符转义。
std::string to_regex(std::string_view pattern);

/// 将模式中顶层 (不在字符类 `[]` 内、且未被 `\` 转义) 的 ASCII 字母折叠为
/// `[xX]` 字符类, 实现大小写不敏感匹配 (glob 库本身大小写敏感)。
std::string case_fold_pattern(std::string_view pattern);

/// 大小写不敏感版本: 匹配前对 pattern 做 case-fold (见 case_fold_pattern)。
/// 与取消版可组合使用。
std::vector<fs::path> glob(const std::string &pathname, bool case_sensitive);
std::vector<fs::path> rglob(const std::string &pathname, bool case_sensitive);
std::vector<fs::path> glob(const std::string &pathname, bool case_sensitive,
                           const std::atomic<bool> &cancel_flag);
std::vector<fs::path> rglob(const std::string &pathname, bool case_sensitive,
                            const std::atomic<bool> &cancel_flag);

} // namespace glob
