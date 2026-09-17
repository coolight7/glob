#include <glob/glob.h>

#include <cassert>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <optional>
#include <regex>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace glob {

namespace {

// ── UTF-8 <-> fs::path 无损转换 ─────────────────────────────────────────
// 项目内部约定路径字符串为 UTF-8。Windows (MSVC) 下 fs::path 的窄字符串
// 构造/导出按 ANSI 代码页 (如中文系统的 GBK/CP936) 解释和生成:
// - path::string() 遇到代码页无法表示的字符时直接抛出
//   std::system_error(ERROR_NO_UNICODE_TRANSLATION): "No mapping for the
//   Unicode character exists in the target multi-byte codepage"
//   (典型触发: 目录树中存在 GBK 外的文件名, 如 boost wave 测试数据
//   "utf8-test-ßµ™∃", 曾导致 rglob 整体失败);
// - 以窄字符串构造 path 会把 UTF-8 字节流误读为 GBK, 非 ASCII 模式永远
//   匹配不上目录条目。
// 此处 Windows 下使用 Win32 CP_UTF8 API 与 path.native() (wstring) 互转:
// 1) 兼容 C++17 (glob 库 target 标准为 C++17, 无需 C++20 char8_t 关键字);
// 2) 宽字符与 UTF-8 之间无损转换且永不抛异常;
// POSIX 下 path::native() 本身就是 UTF-8 字节串, 直通零开销、行为不变。
inline fs::path path_from_utf8(std::string_view utf8) {
#ifdef _WIN32
  if (utf8.empty()) {
    return fs::path{};
  }
  int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (wlen <= 0) {
    return fs::path{};
  }
  std::wstring wstr(static_cast<size_t>(wlen), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wstr.data(), wlen);
  return fs::path(std::move(wstr));
#else
  return fs::path(std::string(utf8));
#endif
}

inline std::string path_to_utf8(const fs::path &path) {
#ifdef _WIN32
  const std::wstring &wstr = path.native();
  if (wstr.empty()) {
    return std::string{};
  }
  int ulen = ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
  if (ulen <= 0) {
    return std::string{};
  }
  std::string u8(static_cast<size_t>(ulen), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), u8.data(), ulen, nullptr, nullptr);
  return u8;
#else
  return path.string();
#endif
}

/// 检查取消标记, 置位时抛出 cancelled_error (定义于 glob.h)。
/// 空指针表示不检查, 保持原有行为。
inline void check_cancel(const std::atomic<bool> *cancel_flag) {
  if (cancel_flag != nullptr && cancel_flag->load(std::memory_order_relaxed)) {
    throw cancelled_error("glob cancelled");
  }
}

static constexpr auto SPECIAL_CHARACTERS = std::string_view{"()[]{}?*+-|^$\\.&~# \t\n\r\v\f"};
static const auto ESCAPE_SET_OPER = std::regex(std::string{R"([&~|])"});
static const auto ESCAPE_REPL_STR = std::string{R"(\\\1)"};

bool string_replace(std::string &str, std::string_view from, std::string_view to) {
  std::size_t start_pos = str.find(from);
  if (start_pos == std::string::npos)
    return false;
  str.replace(start_pos, from.length(), to);
  return true;
}

std::string translate(std::string_view pattern) {
  std::size_t i = 0, n = pattern.size();
  std::string result_string;

  while (i < n) {
    auto c = pattern[i];
    i += 1;
    if (c == '*') {
      result_string += ".*";
    } else if (c == '?') {
      result_string += ".";
    } else if (c == '[') {
      auto j = i;
      if (j < n && pattern[j] == '!') {
        j += 1;
      }
      if (j < n && pattern[j] == ']') {
        j += 1;
      }
      while (j < n && pattern[j] != ']') {
        j += 1;
      }
      if (j >= n) {
        result_string += "\\[";
      } else {
        auto stuff = std::string(pattern.begin() + i, pattern.begin() + j);
        if (stuff.find("--") == std::string::npos) {
          string_replace(stuff, std::string_view{"\\"}, std::string_view{R"(\\)"});
        } else {
          std::vector<std::string> chunks;
          std::size_t k = 0;
          if (pattern[i] == '!') {
            k = i + 2;
          } else {
            k = i + 1;
          }

          while (true) {
            k = pattern.find("-", k, j);
            if (k == std::string_view::npos) {
              break;
            }
            chunks.push_back(std::string(pattern.begin() + i, pattern.begin() + k));
            i = k + 1;
            k = k + 3;
          }

          chunks.push_back(std::string(pattern.begin() + i, pattern.begin() + j));
          // Escape backslashes and hyphens for set difference (--).
          // Hyphens that create ranges shouldn't be escaped.
          bool first = true;
          for (auto &chunk : chunks) {
            string_replace(chunk, std::string_view{"\\"}, std::string_view{R"(\\)"});
            string_replace(chunk, std::string_view{"-"}, std::string_view{R"(\-)"});
            if (first) {
              stuff += chunk;
              first = false;
            } else {
              stuff += "-" + chunk;
            }
          }
        }

        // Escape set operations (&&, ~~ and ||).
        std::string result{};
        std::regex_replace(std::back_inserter(result), // result
                           stuff.begin(), stuff.end(), // string
                           ESCAPE_SET_OPER,            // pattern
                           ESCAPE_REPL_STR);           // repl
        stuff = result;
        i = j + 1;
        if (stuff[0] == '!') {
          stuff = "^" + std::string(stuff.begin() + 1, stuff.end());
        } else if (stuff[0] == '^' || stuff[0] == '[') {
          stuff = "\\\\" + stuff;
        }
        result_string = result_string + "[" + stuff + "]";
      }
    } else {
      // SPECIAL_CHARS
      // closing ')', '}' and ']'
      // '-' (a range in character set)
      // '&', '~', (extended character set operations)
      // '#' (comment) and WHITESPACE (ignored) in verbose mode
      static std::map<int, std::string> special_characters_map;
      if (special_characters_map.empty()) {
        for (auto &&sc : SPECIAL_CHARACTERS) {
          special_characters_map.emplace(static_cast<int>(sc), std::string{"\\"} + std::string(1, sc));
        }
      }

      if (SPECIAL_CHARACTERS.find(c) != std::string_view::npos) {
        result_string += special_characters_map[static_cast<int>(c)];
      } else {
        result_string += c;
      }
    }
  }
  return std::string{"(("} + result_string + std::string{R"()|[\r\n])$)"};
}

std::regex compile_pattern(std::string_view pattern) {
  return std::regex(translate(pattern), std::regex::ECMAScript);
}

bool fnmatch(std::string&& name, const std::regex& pattern) {
  return std::regex_match(std::move(name), pattern);
}

std::vector<fs::path> filter(const std::vector<fs::path> &names,
                             std::string_view pattern) {
  // std::cout << "Pattern: " << pattern << "\n";
  const auto pattern_re = compile_pattern(pattern);
  std::vector<fs::path> result;
  std::copy_if(std::make_move_iterator(names.begin()), std::make_move_iterator(names.end()),
               std::back_inserter(result),
               [&pattern_re](const fs::path& name) { return fnmatch(path_to_utf8(name), pattern_re); });
  return result;
}

#ifdef _WIN32
#include <cstdlib>

inline std::string get_env(const char* var) {
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, var) == 0 && buffer != nullptr) {
        std::string result(buffer);
        free(buffer);
        return result;
    }
    return {};
}
#else
inline std::string get_env(const char* var) {
    const char* value = std::getenv(var);
    return value ? std::string(value) : "";
}
#endif

fs::path expand_tilde(fs::path path) {
  if (path.empty()) return path;

#ifdef _WIN32
    const char * home_variable = "USERNAME";
#else
    const char * home_variable = "USER";
#endif
  std::string home = get_env(home_variable);
  
  if (home.empty()) {
      throw std::invalid_argument("error: Unable to expand `~` - HOME environment variable not set.");
  }

  std::string s = path_to_utf8(path);
  if (!s.empty() && s[0] == '~') {
    // home 取自环境变量 (Windows 上为 ANSI 窄编码): 先经普通窄构造得到正确的
    // 原生 path, 再统一转回 UTF-8 与剩余部分 (内部 UTF-8) 拼接, 保证整串一致
    std::string home_utf8 = path_to_utf8(fs::path(get_env(home_variable)));
    s                     = home_utf8 + s.substr(1, s.size() - 1);
    return path_from_utf8(s);
  }
  return path;
}

bool has_magic(const std::string &pathname) {
  static const auto magic_check = std::regex("([*?[])");
  return std::regex_search(pathname, magic_check);
}

inline bool is_hidden(const fs::path &path) noexcept {
#ifdef _WIN32
  std::wstring_view s = path.native();
  auto pos = s.find_last_of(L"/\\");
  if (pos != std::wstring_view::npos) {
    s = s.substr(pos + 1);
  }
  if (s.empty() || s == L"." || s == L"..") return false;
  return s[0] == L'.';
#else
  std::string_view s = path.native();
  auto pos = s.rfind('/');
  if (pos != std::string_view::npos) {
    s = s.substr(pos + 1);
  }
  if (s.empty() || s == "." || s == "..") return false;
  return s[0] == '.';
#endif
}

constexpr bool is_recursive(std::string_view pattern) noexcept { return pattern == std::string_view{"**"}; }

/// 遍历过程上下文 (库内部传递)
/// - [keepPath]: 条目排除过滤, 在**所有层级**生效 (命中目录即整棵子树剪枝)
/// - [countPolicy]: 只有**顶层**调用传入 —— 中间层遍历的是中间结果 (如 `**` 的目录
///   展开), 其数量不代表最终匹配结果, 不能计入上限
struct WalkContext {
  const std::function<bool(const fs::path &, bool)> *keepPath = nullptr;
  WalkPolicy                                        *countPolicy = nullptr;

  /// 中间层上下文: 保留排除过滤, 不计入最终结果数量
  WalkContext excludeOnly() const { return WalkContext{keepPath, nullptr}; }
};

/// 条目是否被遍历策略排除 (无过滤时一律保留)
inline bool isExcludedByWalk(const WalkContext &walk, const fs::path &entryPath,
                             bool isDir) {
  return walk.keepPath != nullptr && false == (*walk.keepPath)(entryPath, isDir);
}

/// 计入一个匹配结果; 超过上限抛 [walk_limit_exceeded] (立即中断遍历)
inline void countWalkResult(const WalkContext &walk) {
  if (walk.countPolicy == nullptr) {
    return;
  }
  ++walk.countPolicy->resultCount;
  if (walk.countPolicy->maxResults > 0
      && walk.countPolicy->resultCount > walk.countPolicy->maxResults) {
    throw walk_limit_exceeded(
        "glob walk matched more than WalkPolicy::maxResults ("
        + std::to_string(walk.countPolicy->maxResults) + ")"
    );
  }
}

std::vector<fs::path> iter_directory(const fs::path &dirname, bool dironly,
                                     const std::atomic<bool> *cancel_flag,
                                     const WalkContext &walk = WalkContext{}) {
  std::vector<fs::path> result;

  auto current_directory = dirname;
  if (current_directory.empty()) {
    current_directory = fs::current_path();
  }

  std::error_code ec;
  if (fs::exists(current_directory, ec)) {
    try {
      for (auto &entry : fs::directory_iterator(
              current_directory,
              fs::directory_options::skip_permission_denied,
              ec)) {
        // 每个目录条目检查一次取消标记, 保证大目录遍历也能及时中断
        check_cancel(cancel_flag);
        // 条目路径与产出写法一致 (绝对 dirname → 绝对路径), 供排除过滤判定;
        // 被排除的条目直接跳过: 目录同时意味着整棵子树不再遍历 (剪枝)
        fs::path entry_path = (dirname.is_absolute() || dirname.empty())
                                  ? entry.path()
                                  : dirname / entry.path().filename();
        // 有排除过滤时才需要区分目录 (目录被排除 = 剪枝, 见 WalkPolicy::keepPath)
        const bool is_dir = walk.keepPath != nullptr ? entry.is_directory(ec) : false;
        if (isExcludedByWalk(walk, entry_path, is_dir)) {
          continue;
        }
        if (!dironly || entry.is_directory(ec)) {
          result.push_back(std::move(entry_path));
        }
      }
    } catch (const cancelled_error &) {
      // 取消必须向上传播, 不能当作 "not a directory" 吞掉
      throw;
    } catch (walk_limit_exceeded &) {
      // 数量上限同样必须向上传播
      throw;
    } catch (std::exception&) {
      // not a directory
      // do nothing
    }
  }

  return result;
}

// Recursively yields relative pathnames inside a literal directory.
void rlistdir_impl(const fs::path &dirname, bool dironly,
                   const std::atomic<bool> *cancel_flag,
                   std::vector<fs::path> &result,
                   const WalkContext &walk = WalkContext{}) {
  auto names = iter_directory(dirname, dironly, cancel_flag, walk);
  for (auto &&name : names) {
    if (!is_hidden(name)) {
      // 这里的产出即最终结果 (顶层 `**`), 因此在产生处即计入数量上限
      countWalkResult(walk);
      result.push_back(name);
      // 仅对真实目录递归，避免符号链接目录导致的循环（“卡住”）以及对普通文件的无效递归
      // - symlink 目录已 push 本身（上层可见），但不再展开其目标，避免 A->B->A 循环
      // - 普通文件无需递归（iter_directory 对文件直接返回空）
      std::error_code ec;
      auto st = fs::symlink_status(name, ec);
      if (ec) {
        continue;
      }
      if (fs::is_symlink(st)) {
        continue;
      }
      if (fs::is_directory(st)) {
        rlistdir_impl(name, dironly, cancel_flag, result, walk);
      }
    }
  }
}

std::vector<fs::path> rlistdir(const fs::path &dirname, bool dironly,
                               const std::atomic<bool> *cancel_flag,
                               const WalkContext &walk = WalkContext{}) {
  std::vector<fs::path> result;
  rlistdir_impl(dirname, dironly, cancel_flag, result, walk);
  return result;
}

// This helper function recursively yields relative pathnames inside a literal
// directory.
std::vector<fs::path> glob2(const fs::path &dirname, [[maybe_unused]] const fs::path &pattern,
                            bool dironly, const std::atomic<bool> *cancel_flag,
                            const WalkContext &walk = WalkContext{}) {
  // std::cout << "In glob2\n";
  std::vector<fs::path> result;
  // look into the base directory as well, but only if it exists
  if (fs::exists(dirname)) {
    countWalkResult(walk);
    result.push_back(".");
  }
  assert(is_recursive(path_to_utf8(pattern)));
  auto matched_dirs = rlistdir(dirname, dironly, cancel_flag, walk);
  std::copy(std::make_move_iterator(matched_dirs.begin()), std::make_move_iterator(matched_dirs.end()), std::back_inserter(result));
  return result;
}

// These 2 helper functions non-recursively glob inside a literal directory.
// They return a list of basenames.  _glob1 accepts a pattern while _glob0
// takes a literal basename (so it only has to check for its existence).

std::vector<fs::path> glob1_compiled(const fs::path &dirname, const std::regex &pattern_re,
                                     bool dironly, const std::atomic<bool> *cancel_flag,
                                     const WalkContext &walk = WalkContext{}) {
  std::vector<fs::path> filtered_names;
  auto names = iter_directory(dirname, dironly, cancel_flag, walk);
  for (auto &&name : names) {
    if (!is_hidden(name)) {
      auto fn = name.filename();
      if (fnmatch(path_to_utf8(fn), pattern_re)) {
        filtered_names.push_back(std::move(fn));
      }
    }
  }
  return filtered_names;
}

std::vector<fs::path> glob1(const fs::path &dirname, const fs::path &pattern,
                            bool dironly, const std::atomic<bool> *cancel_flag,
                            const WalkContext &walk = WalkContext{}) {
  const auto pattern_re = compile_pattern(path_to_utf8(pattern));
  return glob1_compiled(dirname, pattern_re, dironly, cancel_flag, walk);
}

std::vector<fs::path> glob0(const fs::path &dirname, const fs::path &basename,
                            bool /*dironly*/, const std::atomic<bool> *cancel_flag) {
  // std::cout << "In glob0\n";

  // 'q*x/' should match only directories.
  check_cancel(cancel_flag);
  if ((basename.empty() && fs::is_directory(dirname)) || (!basename.empty() && fs::exists(dirname / basename))) {
    return {basename};
  }
  return {};
}

std::vector<fs::path> glob(const fs::path &inpath, bool recursive = false,
                           bool dironly = false,
                           const std::atomic<bool> *cancel_flag = nullptr,
                           bool case_sensitive = true,
                           const WalkContext &walk = WalkContext{}) {
  std::vector<fs::path> result;

  auto pathname = path_to_utf8(inpath);
  if (!case_sensitive) {
    // 大小写不敏感: 将模式中的 ASCII 字母折叠为 [xX] 字符类后走统一流程
    pathname = case_fold_pattern(pathname);
  }
  auto path = path_from_utf8(pathname);

  if (pathname.empty()) {
    return result;
  }

  if (pathname[0] == '~') {
    // expand tilde
    path = expand_tilde(path);
  }

  auto dirname = path.parent_path();
  const auto basename = path.filename();

  if (!has_magic(pathname)) {
    assert(!dironly);

    // Patterns ending with a slash should match only directories
    if ((!basename.empty() && fs::exists(path)) || (basename.empty() && fs::is_directory(dirname))) {
      // 字面路径 (无通配符): 单个条目, 按真实类型判定排除
      if (false == isExcludedByWalk(walk, path, fs::is_directory(path))) {
        countWalkResult(walk);
        result.push_back(path);
      }
    }
    return result;
  }

  if (dirname.empty()) {
    if (recursive && is_recursive(path_to_utf8(basename))) {
      return glob2(dirname, basename, dironly, cancel_flag, walk);
    }
    return glob1(dirname, basename, dironly, cancel_flag, walk);
  }

  std::vector<fs::path> dirs{dirname};
  if (dirname != path_from_utf8(pathname) && has_magic(path_to_utf8(dirname))) {
    // 目录展开是中间层遍历: 保留排除过滤, 但不计入最终结果数量上限
    dirs = glob(dirname, recursive, true, cancel_flag, case_sensitive, walk.excludeOnly());
  }

  const auto basename_u8 = path_to_utf8(basename);
  const bool basename_has_magic = has_magic(basename_u8);
  const bool basename_is_rec = recursive && is_recursive(basename_u8);
  std::optional<std::regex> compiled_basename_re;
  if (basename_has_magic && !basename_is_rec) {
    compiled_basename_re.emplace(compile_pattern(basename_u8));
  }

  for (auto &d : dirs) {
    // 每个目录层级检查一次取消标记
    check_cancel(cancel_flag);
    if (!basename_has_magic) {
      for (auto &&name : glob0(d, basename, dironly, cancel_flag)) {
        fs::path subresult = name;
        if (name.parent_path().empty()) {
          subresult = d / name;
        }
        // 条目级排除已在 iter_directory 完成 (那里能拿到类型, 目录命中即剪枝);
        // 此处只做计数与收集, 不再重复判定 —— 否则会以 isDir=false 误判目录
        auto normalized = subresult.lexically_normal();
        countWalkResult(walk);
        result.push_back(std::move(normalized));
      }
    } else if (basename_is_rec) {
      // `**` 分支的匹配结果已在 glob2/rlistdir_impl 产生处计入数量上限
      for (auto &&name : glob2(d, basename, dironly, cancel_flag, walk)) {
        fs::path subresult = name;
        if (name.parent_path().empty()) {
          subresult = d / name;
        }
        // 同上: 排除判定已在 iter_directory 完成 (此分支的计数在 glob2/rlistdir_impl)
        auto normalized = subresult.lexically_normal();
        result.push_back(std::move(normalized));
      }
    } else {
      for (auto &&name : glob1_compiled(d, *compiled_basename_re, dironly, cancel_flag, walk)) {
        fs::path subresult = name;
        if (name.parent_path().empty()) {
          subresult = d / name;
        }
        // 条目级排除已在 iter_directory 完成 (那里能拿到类型, 目录命中即剪枝);
        // 此处只做计数与收集, 不再重复判定 —— 否则会以 isDir=false 误判目录
        auto normalized = subresult.lexically_normal();
        countWalkResult(walk);
        result.push_back(std::move(normalized));
      }
    }
  }

  return result;
}

} // namespace end

// ── 模式工具函数 (供上层 filesystem 工具使用) ────────────────────────────────

bool has_recursive_segment(std::string_view pattern) noexcept {
  return pattern.find("**") != std::string_view::npos;
}

fs::path static_prefix(std::string_view pattern) {
  auto pos = pattern.find_first_of("*?[");
  if (pos == std::string_view::npos) {
    // 无通配符: 基准为其父目录
    return path_from_utf8(pattern).parent_path();
  }
  auto prefix = pattern.substr(0, pos);
  auto slash  = prefix.find_last_of('/');
  if (slash == std::string_view::npos) {
    return fs::path{"."};
  }
  return path_from_utf8(prefix.substr(0, slash + 1));
}

int path_depth(const fs::path &rel) noexcept {
  int depth = 0;
  for (auto seg = rel.begin(); seg != rel.end(); ++seg) {
    if (*seg != "." && !seg->empty()) {
      depth++;
    }
  }
  return depth;
}

std::string to_regex(std::string_view pattern) {
  std::string re;
  re.reserve(pattern.size() * 2 + 4);
  re            += '^';
  const size_t n = pattern.size();
  for (size_t i = 0; i < n; ++i) {
    char c = pattern[i];
    if (c == '*') {
      re += ".*";
      // 吞掉连续的 `*` (含 `**`)
      while (i + 1 < n && pattern[i + 1] == '*') {
        i++;
      }
    } else if (c == '?') {
      re += '.';
    } else if (c == '[') {
      // 找到配对的 `]`, 字符类整体传递给正则
      size_t j = i + 1;
      if (j < n && (pattern[j] == '!' || pattern[j] == '^')) {
        j++;
      }
      if (j < n && pattern[j] == ']') {
        j++;
      }
      while (j < n && pattern[j] != ']') {
        j++;
      }
      if (j >= n) {
        // 无配对 `]`, 按字面量转义
        re += "\\[";
      } else {
        std::string cls{pattern.substr(i, j - i + 1)};
        // glob 的 `[!...]` 转正则的 `[^...]`
        if (cls.size() > 1 && cls[1] == '!') {
          cls[1] = '^';
        }
        re += cls;
        i   = j;
      }
    } else {
      // 转义正则特殊字符
      static const std::string special = R"(\.^$+(){}|)";
      if (special.find(c) != std::string::npos) {
        re += '\\';
      }
      re += c;
    }
  }
  re += '$';
  return re;
}

std::string case_fold_pattern(std::string_view pattern) {
  std::string out;
  out.reserve(pattern.size() * 2);
  bool inClass = false; // 是否处于字符类 [] 内
  for (size_t i = 0; i < pattern.size(); ++i) {
    char c = pattern[i];
    if (c == '\\' && i + 1 < pattern.size()) {
      // 转义序列原样保留
      out += c;
      out += pattern[++i];
      continue;
    }
    if (c == '[') {
      inClass  = true;
      out     += c;
      continue;
    }
    if (c == ']' && inClass) {
      inClass  = false;
      out     += c;
      continue;
    }
    if (!inClass && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
      auto lower  = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      auto upper  = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      out        += '[';
      out        += lower;
      out        += upper;
      out        += ']';
    } else {
      out += c;
    }
  }
  return out;
}

// 公有窄字符串入口: 调用方按项目约定传入 UTF-8, 显式经 path_from_utf8
// 无损构造, 避免 Windows 下隐式窄构造按 ANSI 代码页误读
std::vector<fs::path> glob(const std::string &pathname) {
  return glob(path_from_utf8(pathname), false, false, nullptr, true);
}

std::vector<fs::path> rglob(const std::string &pathname) {
  return glob(path_from_utf8(pathname), true, false, nullptr, true);
}

std::vector<fs::path> glob(const std::string &pathname,
                           const std::atomic<bool> &cancel_flag) {
  return glob(path_from_utf8(pathname), false, false, &cancel_flag, true);
}

std::vector<fs::path> rglob(const std::string &pathname,
                            const std::atomic<bool> &cancel_flag) {
  return glob(path_from_utf8(pathname), true, false, &cancel_flag, true);
}

std::vector<fs::path> glob(const std::string &pathname, bool case_sensitive) {
  return glob(path_from_utf8(pathname), false, false, nullptr, case_sensitive);
}

std::vector<fs::path> rglob(const std::string &pathname, bool case_sensitive) {
  return glob(path_from_utf8(pathname), true, false, nullptr, case_sensitive);
}

std::vector<fs::path> glob(const std::string &pathname, bool case_sensitive,
                           const std::atomic<bool> &cancel_flag) {
  return glob(path_from_utf8(pathname), false, false, &cancel_flag, case_sensitive);
}

std::vector<fs::path> rglob(const std::string &pathname, bool case_sensitive,
                            const std::atomic<bool> &cancel_flag) {
  return glob(path_from_utf8(pathname), true, false, &cancel_flag, case_sensitive);
}

// 遍历策略版入口: 排除过滤 (命中目录整棵子树剪枝) 与匹配结果数量上限都在
// 遍历过程中生效; 超限抛 walk_limit_exceeded (调用方据 policy.resultCount 报数)

std::vector<fs::path> glob(const std::string &pathname, bool case_sensitive,
                           const std::atomic<bool> &cancel_flag, WalkPolicy &policy) {
  WalkContext walk;
  walk.keepPath    = policy.keepPath ? &policy.keepPath : nullptr;
  walk.countPolicy = &policy;
  return glob(path_from_utf8(pathname), false, false, &cancel_flag, case_sensitive, walk);
}

std::vector<fs::path> rglob(const std::string &pathname, bool case_sensitive,
                            const std::atomic<bool> &cancel_flag, WalkPolicy &policy) {
  WalkContext walk;
  walk.keepPath    = policy.keepPath ? &policy.keepPath : nullptr;
  walk.countPolicy = &policy;
  return glob(path_from_utf8(pathname), true, false, &cancel_flag, case_sensitive, walk);
}

/// 模式是否为"覆盖整棵子树"的排除模式: 除首段外还出现 `**` 段
/// (如 `**/build/**`、`a/**/*` 可剪枝; `**/build/*`、`build/*` 只匹配单层, 不剪枝)
bool is_subtree_exclude_pattern(std::string_view pattern) noexcept {
  size_t begin = 0;
  size_t index = 0;
  while (begin <= pattern.size()) {
    auto end = pattern.find('/', begin);
    if (end == std::string_view::npos) {
      end = pattern.size();
    }
    if (index > 0 && pattern.substr(begin, end - begin) == std::string_view{"**"}) {
      return true;
    }
    if (end == pattern.size()) {
      break;
    }
    begin = end + 1;
    ++index;
  }
  return false;
}

std::vector<fs::path> glob(const std::vector<std::string> &pathnames) {
  std::vector<fs::path> result;
  for (const auto &pathname : pathnames) {
    auto matched_res = glob(path_from_utf8(pathname), false, false, nullptr, true);
    std::copy(std::make_move_iterator(matched_res.begin()), std::make_move_iterator(matched_res.end()), std::back_inserter(result));
  }
  return result;
}

std::vector<fs::path> rglob(const std::vector<std::string> &pathnames) {
  std::vector<fs::path> result;
  for (const auto &pathname : pathnames) {
    auto matched_res = glob(path_from_utf8(pathname), true, false, nullptr, true);
    std::copy(std::make_move_iterator(matched_res.begin()), std::make_move_iterator(matched_res.end()), std::back_inserter(result));
  }
  return result;
}

std::vector<fs::path>
glob(const std::initializer_list<std::string> &pathnames) {
  return glob(std::vector<std::string>(pathnames));
}

std::vector<fs::path>
rglob(const std::initializer_list<std::string> &pathnames) {
  return rglob(std::vector<std::string>(pathnames));
}

} // namespace glob
