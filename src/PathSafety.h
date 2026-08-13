#pragma once

#include <QString>
#include <QStringList>

// docs/PROTOCOL.md §4.1 / §4.2 / §4.4
namespace afmu {

// 剥掉路径部分、替换非法字符、截断 200 字符，空则 "unnamed"
QString sanitizeFileName(const QString &raw);

// 把用户传入的路径规范化（解析 .. 与符号链接），并检查是否等于某个 root 或位于其下。
// 越界或非法返回空字符串（调用方一律按 404 处理，不泄露真实原因）。
QString resolveUnderRoots(const QString &path, const QStringList &roots);

// overwrite != 1 时的自动改名：a.txt -> "a (1).txt" -> "a (2).txt"，上限 10000
QString uniqueTarget(const QString &dir, const QString &name);

/**
 * 带退避重试的改名 / 删除。
 *
 * Windows 上刚写完并关掉的文件常常还被别人拿着句柄：杀毒软件在扫，Windows Search
 * 在建索引，资源管理器在生成缩略图。这时候 rename 会返回 ERROR_SHARING_VIOLATION，
 * 而这恰好是下载完成的最后一步 —— 表现是「文件明明下完了却报重命名失败」，
 * 而且**只是偶尔**，重试一次就好。POSIX 上不存在这个问题（rename 不看句柄），
 * 所以 afmu-linux 那边直接调 QFile::rename。
 *
 * 占用一般是几十毫秒级的，所以退避总时长控制在半秒以内，不让界面明显卡住。
 */
bool renameWithRetry(const QString &from, const QString &to);
bool removeWithRetry(const QString &path);

} // namespace afmu
