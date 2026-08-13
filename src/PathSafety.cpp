#include "PathSafety.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

namespace afmu {

namespace {

/**
 * Windows 保留设备名（`CON`、`PRN`、`AUX`、`NUL`、`COM1..9`、`LPT1..9`）。
 *
 * 这不是"不好看的文件名"，是**打不开的文件名**：`CON.txt` 在任何目录下都指向控制台
 * 设备，不指向文件。用它当上传目标，`QFile::open` 会成功、写进去的字节会消失，
 * 而我们会照常回 `{"ok":true}` —— 对端以为文件传完了，实际上什么都没有。
 *
 * 带扩展名一样中招（`NUL.jpg`、`COM1.tar.gz`），所以比对的是第一个点之前的部分。
 */
bool isReservedDeviceName(const QString &name)
{
    const int dot = name.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? name : name.left(dot)).trimmed().toUpper();
    if (stem == QLatin1String("CON") || stem == QLatin1String("PRN")
        || stem == QLatin1String("AUX") || stem == QLatin1String("NUL")
        || stem == QLatin1String("CLOCK$")) {
        return true;
    }
    if (stem.size() == 4 && stem.at(3).isDigit() && stem.at(3) != QLatin1Char('0')
        && (stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT")))) {
        return true;
    }
    return false;
}

/**
 * 路径的根：`C:/`，或者 UNC 的 `//server/share`。没有根就不是绝对路径。
 *
 * Windows 上"绝对"有好几种不算数的形式，都必须挡掉：
 *   · `C:foo`  —— 盘符相对路径，指向 C 盘的**当前目录**，那是个进程全局状态
 *   · `/foo`   —— 无盘符的根路径，指向当前盘的根，同样取决于进程状态
 *   · `foo`    —— 相对路径
 * 这三种的共同点是「同一个字符串，不同时刻指向不同文件」，做越界检查时等于没检查。
 */
QString pathRoot(const QString &p)
{
    if (p.size() >= 3 && p.at(1) == QLatin1Char(':') && p.at(2) == QLatin1Char('/')
        && p.at(0).isLetter()) {
        return p.left(3);
    }
    if (p.startsWith(QLatin1String("//"))) {
        // //server/share —— 少一段就不是完整的共享路径
        const int server = p.indexOf(QLatin1Char('/'), 2);
        if (server < 0)
            return QString();
        const int share = p.indexOf(QLatin1Char('/'), server + 1);
        const QString root = share < 0 ? p : p.left(share);
        return root.size() > server + 1 ? root : QString();
    }
    return QString();
}

/** 反斜杠统一成斜杠，再走一遍 cleanPath（解 `.` 和 `..`）。 */
QString normalized(const QString &p)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(p));
}

/**
 * 返回 path 中最长的、真实存在的前缀的 canonical 形式 + 剩余部分
 * （mkdir / upload 的目标可能还不存在）。
 */
QString canonicalizeLenient(const QString &absPath)
{
    const QString clean = normalized(absPath);
    const QString root = pathRoot(clean);
    if (root.isEmpty())
        return QString();

    QStringList tail;
    QString cur = clean;
    while (true) {
        const QString canon = QFileInfo(cur).canonicalFilePath();
        if (!canon.isEmpty()) {
            QString joined = QDir::fromNativeSeparators(canon);
            for (const QString &t : std::as_const(tail)) {
                if (t.isEmpty() || t == QLatin1String("."))
                    continue;
                if (t == QLatin1String(".."))
                    return QString(); // 已 cleanPath 过，还有 .. 说明是想穿越，直接拒绝
                if (!joined.endsWith(QLatin1Char('/')))
                    joined += QLatin1Char('/');
                joined += t;
            }
            return joined;
        }
        if (cur.size() <= root.size())
            return QString(); // 走到根都不存在（盘没插、共享断了）
        const int slash = cur.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            return QString();
        tail.prepend(cur.mid(slash + 1));
        cur = slash + 1 <= root.size() ? root : cur.left(slash);
    }
}

/**
 * path 是否等于 root 或位于其下。
 *
 * 大小写敏感地比 —— 听起来在 Windows 上是错的，其实反过来：两边都来自
 * `canonicalFilePath()`，那是文件系统自己给出的写法（长文件名、真实大小写），
 * 同一个目录只有一种形式，所以真的在 root 底下时前缀必然逐字符相同。
 *
 * 反过来用 Qt 的大小写不敏感比较才有风险：Qt 用的是 Unicode 折叠表，NTFS 用的是
 * 自己那张大写表，两者对某些字符的判断不一样。不一样的方向恰好是"Qt 认为相同、
 * NTFS 认为不同"—— 那就是把 root 之外的目录判成了 root 之内。
 */
bool isUnder(const QString &path, const QString &root)
{
    if (path == root)
        return true;
    QString r = root;
    if (!r.endsWith(QLatin1Char('/')))
        r += QLatin1Char('/');
    return path.startsWith(r);
}

} // namespace

QString sanitizeFileName(const QString &raw)
{
    QString s = raw;
    // 剥掉路径部分：取最后一个 / 或 \ 之后的内容
    const int slash = qMax(s.lastIndexOf(QLatin1Char('/')), s.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        s = s.mid(slash + 1);
    // 盘符相对形式 `C:name`：冒号下面会被换成下划线，但先按"取冒号之后"处理，
    // 免得留下 `C_name` 这种莫名其妙的名字
    if (s.size() > 1 && s.at(1) == QLatin1Char(':') && s.at(0).isLetter())
        s = s.mid(2);

    QString out;
    out.reserve(s.size());
    for (QChar c : std::as_const(s)) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7f || c == u'<' || c == u'>' || c == u':' || c == u'"'
            || c == u'|' || c == u'?' || c == u'*' || c == u'/' || c == u'\\') {
            out.append(u'_');
        } else {
            out.append(c);
        }
    }
    out = out.trimmed();
    if (out.size() > 200)
        out.truncate(200);

    // 结尾的点和空格：Windows 打开文件时会**悄悄去掉**它们，于是 "a.txt " 和 "a.txt"
    // 是同一个文件，而我们的重名检查（uniqueTarget）看到的是两个不同的名字 ——
    // 结果是自动改名没触发，直接覆盖了已有文件。截断之后再去一次，因为截断可能
    // 正好把一个点留在末尾。
    while (!out.isEmpty()
           && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))) {
        out.chop(1);
    }

    // "." 和 ".." 会指向目录本身/父目录，必须挡掉
    if (out.isEmpty() || out == QLatin1String(".") || out == QLatin1String(".."))
        out = QStringLiteral("unnamed");
    // 保留设备名：加前缀而不是整个换掉，用户还能认出这是哪个文件
    if (isReservedDeviceName(out))
        out.prepend(QLatin1Char('_'));
    return out;
}

QString resolveUnderRoots(const QString &path, const QStringList &roots)
{
    if (path.isEmpty())
        return QString();
    // 内嵌 NUL：从 JSON 里过来的字符串可以带，而 Win32 的 API 收的是 C 字符串，
    // NUL 之后的部分会被丢掉 —— 于是校验看到的路径和实际打开的路径不是同一个
    if (path.contains(QChar(0)))
        return QString();

    // 设备命名空间：`\\?\` 绕过所有路径规范化（包括 `..` 的解析），`\\.\` 直接指向
    // 设备对象。两者都不该出现在"共享目录里的某个文件"这种请求里。
    //
    // **必须在规范化之前判，而且不能借 QDir::fromNativeSeparators 来换分隔符**：
    // 那个函数会顺手把 `\\?\` 前缀**删掉**（Qt 的 removeUncOrLongPathPrefix），
    // 于是 `\\?\C:\x` 一进来就变成了普通的 `C:/x`，判断完全落空。这里只做字符替换。
    QString raw = path;
    raw.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (raw.startsWith(QLatin1String("//?/")) || raw.startsWith(QLatin1String("//./")))
        return QString();

    const QString clean = normalized(path);
    if (pathRoot(clean).isEmpty())
        return QString(); // 相对路径、盘符相对路径、无盘符根路径，见 pathRoot 的说明
    // 备用数据流 `file.txt:secret`：盘符那个冒号之后就不该再有冒号了。
    // 它读写的是另一份内容，而列目录、大小、删除看到的都是主流。
    if (clean.indexOf(QLatin1Char(':'), 2) >= 0)
        return QString();

    const QString canon = canonicalizeLenient(clean);
    if (canon.isEmpty())
        return QString();

    for (const QString &root : roots) {
        const QString canonRoot = QFileInfo(normalized(root)).canonicalFilePath();
        if (canonRoot.isEmpty())
            continue;
        if (isUnder(canon, QDir::fromNativeSeparators(canonRoot)))
            return canon;
    }
    return QString();
}

namespace {

/** 退避序列，单位毫秒。总共不到半秒，界面不会明显卡。 */
constexpr int kRetryDelaysMs[] = { 20, 40, 80, 150, 200 };

template <typename Op>
bool withRetry(Op op)
{
    if (op())
        return true;
    for (int delay : kRetryDelaysMs) {
        QThread::msleep(unsigned(delay));
        if (op())
            return true;
    }
    return false;
}

} // namespace

bool renameWithRetry(const QString &from, const QString &to)
{
    return withRetry([&] { return QFile::rename(from, to); });
}

bool removeWithRetry(const QString &path)
{
    return withRetry([&] { return QFile::remove(path) || !QFile::exists(path); });
}

QString uniqueTarget(const QString &dir, const QString &name)
{
    QDir d(dir);
    if (!d.exists(name))
        return d.filePath(name);

    QString base = name;
    QString ext;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        base = name.left(dot);
        ext = name.mid(dot); // 含点
    }
    for (int i = 1; i < 10000; ++i) {
        const QString cand = QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(ext);
        if (!d.exists(cand))
            return d.filePath(cand);
    }
    return d.filePath(name);
}

} // namespace afmu
