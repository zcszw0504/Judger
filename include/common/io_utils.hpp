#pragma once

#include <filesystem>
#include <string>

namespace judge {
using namespace std;

/**
 * @brief 读取文本文件的全部内容
 * @param path 文本文件路径
 * @return 文本文件的内容(没有指定编码)
 */
string read_file_content(const filesystem::path &path);

/**
 * @brief 断言 subpath 一定不会出现返回上一层目录的情况
 * 这里用于确保计算目录时不会出现目录遍历攻击，由于评测系统
 * 运行时需要 root 权限，如果拿到的文件名包含 "../"，那么
 * 最后有可能导致系统重要文件被覆盖导致安全问题。
 * @param subpath 被检查的文件名
 */
string assert_safe_path(const string &subpath);

}  // namespace judge