
#pragma once
#include <atomic>
#include <functional>
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

// ── 遍历策略 (排除过滤 + 结果数量上限) ────────────────────────────────────

/// 遍历策略: 在**遍历过程中**生效的条目排除与匹配结果数量上限。
///
/// 为什么必须在遍历中生效 (而不是在遍历结果上做后置过滤/后置计数):
/// - 后置过滤时, 被排除的大目录 (如 build/third_party) 仍会被完整遍历 —— 排除
///   失去意义; 遍历中命中目录即整棵子树剪枝, 才能省掉这部分遍历开销;
/// - 后置计数时, 超限的几十万条路径已经读进内存 —— 因此在结果产生处即判定并
///   抛出 [walk_limit_exceeded], 立刻停止遍历。
struct WalkPolicy {
    /// 条目过滤: 返回 false 表示排除该条目。
    /// - `entryPath`: 与遍历产出的路径写法一致 (dirname 为绝对路径时即绝对路径);
    /// - `isDir`: 该条目是否为目录 (目录被排除时整棵子树不再遍历, 即剪枝);
    /// - 调用方按模式自行判定: 覆盖整棵子树的模式 (如 `**/build/**`) 对被排除的
    ///   目录返回 false 即完成剪枝; 只匹配目录自身的模式若希望仍遍历其内容, 可对
    ///   该目录返回 true, 再由调用方自己过滤结果;
    /// - 为空表示不过滤。
    std::function<bool(const fs::path &entryPath, bool isDir)> keepPath{};
    /// 匹配结果数量上限 (0 = 不限): 超过即停止遍历 (见 [stopOnLimit] 决定是抛异常
    /// 还是返回已匹配结果)。空目录/不存在等中间层遍历不计入, 只统计最终匹配结果。
    size_t maxResults = 0;
    /// 累计已匹配结果数 (由遍历内部累加; 同一个 policy 可用于多次遍历, 从而对
    /// **多个 pattern 的总量**生效, 调用方也可在异常后读取它以报告实际数量)。
    size_t resultCount = 0;
    /// 超过 [maxResults] 时的处理方式:
    /// - false (默认): 抛 [walk_limit_exceeded], 本次遍历结果不返回, 由调用方报错;
    /// - true: 不抛异常, 遍历立即停止并**返回已匹配到的结果** (调用方按
    ///   [limitReached] 判断结果是否被截断)。适合"尽量给出一部分结果 + 提示"
    ///   的调用方, 避免整个调用因规模过大而无任何返回值。
    bool stopOnLimit = false;
    /// 是否已因超过 [maxResults] 而停止 ([stopOnLimit] 模式下由遍历内部置位):
    /// - 调用方据此判断"结果被截断, 还有更多匹配未收集";
    /// - 同一 policy 再次遍历时立即返回空结果, 保证多 pattern 的总量不超上限。
    bool limitReached = false;
};

/// 遍历匹配结果数超过 [WalkPolicy::maxResults] 时抛出
/// ([WalkPolicy::stopOnLimit] 为 true 时不抛异常, 改为停止遍历并返回已匹配结果)
class walk_limit_exceeded : public std::runtime_error {
public:
    explicit walk_limit_exceeded(const std::string &what) : std::runtime_error(what) {}
};

/// 带遍历策略的版本: 语义同 `glob`, 遍历过程中应用 `policy` (排除剪枝 + 数量上限)
std::vector<fs::path> glob(const std::string &pathname, bool case_sensitive,
                           const std::atomic<bool> &cancel_flag, WalkPolicy &policy);

/// 带遍历策略的递归版本, 语义同 `rglob`
std::vector<fs::path> rglob(const std::string &pathname, bool case_sensitive,
                            const std::atomic<bool> &cancel_flag, WalkPolicy &policy);

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

/// 判断排除模式是否"覆盖整棵子树": 除首段外还出现 `**` 段。
/// 用于 [WalkPolicy::keepPath] 中决定目录是否可整体剪枝:
/// - `**/build/**`、`build/**` → true (命中目录即整棵子树不再遍历);
/// - `**/build/*`、`build/*`、`build` → false (只匹配单层或目录自身,
///   排除目录时仍需继续向下遍历, 否则会误删未被模式覆盖的后代)。
bool is_subtree_exclude_pattern(std::string_view pattern) noexcept;

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
