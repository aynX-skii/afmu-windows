/**
 * 路径越界防护和文件名安全化（PROTOCOL.md §4.1 / §4.2 / §4.4）。
 *
 * 这个文件在 afmu-linux 里不存在，因为那边 PathSafety.cpp 只有一条规则：
 * 「必须以 / 开头，canonical 之后必须落在某个 root 之下」。Windows 上这条规则
 * 整个不成立 —— 绝对路径有三种写法（`C:\`、`\\server\share`、还有指向"当前盘
 * 当前目录"的 `C:foo` 和 `\foo`），文件名里藏得下备用数据流，`CON` 是设备不是
 * 文件，结尾的点和空格会被系统悄悄吃掉。每一条都能把「校验过的路径」和
 * 「实际打开的路径」变成两个东西，而这正是越界防护唯一要防的事。
 *
 * 所以这里逐条钉住。用真实的临时目录跑，因为 resolveUnderRoots 依赖
 * canonicalFilePath()，而那个函数只对真实存在的路径有意义。
 *
 *   cmake -S . -B build -DAFMU_TESTS=ON && cmake --build build
 *   .\build\afmu_pathsafety_test.exe
 */

#include "../src/PathSafety.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int g_failed = 0;
int g_passed = 0;

void check(bool ok, const QString &what)
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::fprintf(stderr, "  失败：%s\n", qPrintable(what));
    }
}

void checkEqual(const QString &got, const QString &want, const QString &what)
{
    check(got == want, QStringLiteral("%1（得到 \"%2\"，期望 \"%3\"）").arg(what, got, want));
}

/** 越界必须返回空串 —— 调用方一律按 404 处理，不区分原因。 */
void checkRejected(const QString &path, const QStringList &roots, const QString &why)
{
    const QString got = afmu::resolveUnderRoots(path, roots);
    check(got.isEmpty(), QStringLiteral("%1：\"%2\" 不该通过（返回了 \"%3\"）")
                             .arg(why, path, got));
}

void checkAccepted(const QString &path, const QStringList &roots, const QString &why)
{
    check(!afmu::resolveUnderRoots(path, roots).isEmpty(),
          QStringLiteral("%1：\"%2\" 应当通过").arg(why, path));
}

bool makeFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write("x");
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ------------------------------------------------------------ §4.2 文件名安全化
    std::fprintf(stderr, "== 文件名安全化 ==\n");
    {
        checkEqual(afmu::sanitizeFileName(QStringLiteral("a.txt")), QStringLiteral("a.txt"),
                   "普通文件名原样保留");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("中文 名字 😀.jpg")),
                   QStringLiteral("中文 名字 😀.jpg"), "中文和 emoji 不该被动");

        // 路径部分要剥掉，两种分隔符都算
        checkEqual(afmu::sanitizeFileName(QStringLiteral("C:\\Windows\\System32\\evil.dll")),
                   QStringLiteral("evil.dll"), "带盘符的完整路径只留文件名");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("../../../etc/passwd")),
                   QStringLiteral("passwd"), "相对路径只留文件名");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("..\\..\\boot.ini")),
                   QStringLiteral("boot.ini"), "反斜杠的相对路径只留文件名");
        // 盘符相对形式：C:name 指的是 C 盘当前目录下的 name
        checkEqual(afmu::sanitizeFileName(QStringLiteral("C:name.txt")), QStringLiteral("name.txt"),
                   "盘符相对形式要剥掉盘符");

        // Windows 文件名里非法的那几个字符
        checkEqual(afmu::sanitizeFileName(QStringLiteral("a<b>c:d\"e|f?g*h.txt")),
                   QStringLiteral("a_b_c_d_e_f_g_h.txt"), "非法字符换成下划线");
        // 备用数据流：冒号被换掉，于是 "报告.docx:hidden" 落成一个普通文件
        check(!afmu::sanitizeFileName(QStringLiteral("报告.docx:hidden")).contains(QLatin1Char(':')),
              QStringLiteral("文件名里不该留下冒号（备用数据流）"));

        // 结尾的点和空格：系统打开时会悄悄去掉，留着会让重名检查失效
        checkEqual(afmu::sanitizeFileName(QStringLiteral("report.txt.")), QStringLiteral("report.txt"),
                   "结尾的点要去掉");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("report.txt   ")),
                   QStringLiteral("report.txt"), "结尾的空格要去掉");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("report...   ")), QStringLiteral("report"),
                   "点和空格混着结尾也要去干净");

        // 保留设备名：写进去的字节会消失，而我们会回 ok
        checkEqual(afmu::sanitizeFileName(QStringLiteral("CON")), QStringLiteral("_CON"),
                   "CON 是设备名");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("nul.txt")), QStringLiteral("_nul.txt"),
                   "带扩展名的设备名同样中招");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("COM1.tar.gz")),
                   QStringLiteral("_COM1.tar.gz"), "COM1 是设备名");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("LPT9")), QStringLiteral("_LPT9"),
                   "LPT9 是设备名");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("COM0")), QStringLiteral("COM0"),
                   "COM0 不是设备名，不该被改");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("console.log")),
                   QStringLiteral("console.log"), "只是以 CON 开头，不是设备名");

        // 空的、纯点的
        checkEqual(afmu::sanitizeFileName(QStringLiteral(".")), QStringLiteral("unnamed"),
                   "\".\" 指向目录本身");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("..")), QStringLiteral("unnamed"),
                   "\"..\" 指向父目录");
        checkEqual(afmu::sanitizeFileName(QString()), QStringLiteral("unnamed"), "空名字");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("   ")), QStringLiteral("unnamed"),
                   "全是空格");
        checkEqual(afmu::sanitizeFileName(QStringLiteral("/")), QStringLiteral("unnamed"),
                   "只有分隔符");

        check(afmu::sanitizeFileName(QString(500, QLatin1Char('a'))).size() <= 200,
              QStringLiteral("超长名字要截断"));
        // 截断之后仍然不能以点结尾
        const QString truncated =
            afmu::sanitizeFileName(QString(199, QLatin1Char('a')) + QStringLiteral(".b"));
        check(!truncated.endsWith(QLatin1Char('.')),
              QStringLiteral("截断之后不该正好留一个点在结尾"));

        // 控制字符（\r\n 进了 Content-Disposition 就是响应头注入）
        check(!afmu::sanitizeFileName(QStringLiteral("a\r\nb.txt")).contains(QLatin1Char('\n')),
              QStringLiteral("控制字符要换掉"));
    }

    // ------------------------------------------------------------ §4.1 越界防护
    std::fprintf(stderr, "\n== 越界防护 ==\n");
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "建不了临时目录，跳过\n");
        return 1;
    }
    {
        const QString base = QDir(tmp.path()).canonicalPath();
        QDir d(base);
        d.mkpath(QStringLiteral("share/sub"));
        d.mkpath(QStringLiteral("outside"));
        makeFile(base + QStringLiteral("/share/sub/inside.txt"));
        makeFile(base + QStringLiteral("/outside/secret.txt"));

        const QString root = base + QStringLiteral("/share");
        const QStringList roots{ root };

        // --- 该通过的
        checkAccepted(root, roots, "共享根目录本身");
        checkAccepted(root + QStringLiteral("/sub"), roots, "子目录");
        checkAccepted(root + QStringLiteral("/sub/inside.txt"), roots, "子目录里的文件");
        checkAccepted(QDir::toNativeSeparators(root + QStringLiteral("/sub/inside.txt")), roots,
                      "反斜杠写法");
        // 还不存在的目标：upload / mkdir 要靠这条
        checkAccepted(root + QStringLiteral("/sub/not-yet.bin"), roots, "尚不存在的上传目标");
        checkAccepted(root + QStringLiteral("/sub/a/b/c"), roots, "尚不存在的多级 mkdir 目标");
        // 绕一圈还在里面
        checkAccepted(root + QStringLiteral("/sub/../sub/inside.txt"), roots, "绕一圈仍在根内");

        // --- 该拒绝的
        checkRejected(base + QStringLiteral("/outside/secret.txt"), roots, "根之外");
        checkRejected(root + QStringLiteral("/../outside/secret.txt"), roots, "用 .. 穿出去");
        checkRejected(root + QStringLiteral("/sub/../../outside/secret.txt"), roots, "多级 .. 穿出去");
        checkRejected(QDir::toNativeSeparators(root + QStringLiteral("\\..\\outside\\secret.txt")),
                      roots, "反斜杠版的穿越");
        checkRejected(QStringLiteral("C:/Windows/System32/config/SAM"), roots, "完全无关的绝对路径");

        // 前缀相同但不是子目录：share2 不在 share 之下
        d.mkpath(QStringLiteral("share2"));
        makeFile(base + QStringLiteral("/share2/x.txt"));
        checkRejected(base + QStringLiteral("/share2/x.txt"), roots,
                      "同前缀的兄弟目录（share2 不在 share 里）");

        // Windows 上"看起来绝对其实不是"的三种写法
        checkRejected(QStringLiteral("share/sub/inside.txt"), roots, "相对路径");
        checkRejected(QStringLiteral("C:share"), roots, "盘符相对路径（指向 C 盘的当前目录）");
        checkRejected(QStringLiteral("/share/sub/inside.txt"), roots, "无盘符的根路径");
        checkRejected(QString(), roots, "空路径");

        // 备用数据流：读写的是另一份内容，而列目录/大小/删除看到的是主流
        checkRejected(root + QStringLiteral("/sub/inside.txt:hidden"), roots, "备用数据流");
        checkRejected(root + QStringLiteral("/sub/inside.txt::$DATA"), roots, "$DATA 形式的数据流");

        // 设备命名空间：\\?\ 绕过路径规范化（包括 .. 的解析），\\.\ 直接指设备
        checkRejected(QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(root)
                          + QStringLiteral("\\sub\\inside.txt"),
                      roots, "\\\\?\\ 前缀");
        checkRejected(QStringLiteral("\\\\.\\PhysicalDrive0"), roots, "\\\\.\\ 设备路径");

        // 内嵌 NUL：Win32 收的是 C 字符串，NUL 之后会被丢掉 —— 校验的路径和
        // 实际打开的路径于是不是同一个
        checkRejected(root + QStringLiteral("/sub/inside.txt") + QChar(0)
                          + QStringLiteral("/../../outside/secret.txt"),
                      roots, "内嵌 NUL");

        // 多个共享根：命中任意一个都算数
        const QStringList two{ root, base + QStringLiteral("/outside") };
        checkAccepted(base + QStringLiteral("/outside/secret.txt"), two, "第二个共享根");
        checkRejected(base + QStringLiteral("/share2/x.txt"), two, "两个根都不包含它");

        // 不存在的根要被跳过，不能让整张表失效
        const QStringList withGhost{ base + QStringLiteral("/no-such-dir"), root };
        checkAccepted(root + QStringLiteral("/sub/inside.txt"), withGhost,
                      "列表里有个不存在的根，其余的仍该生效");
        checkRejected(root + QStringLiteral("/sub/inside.txt"), QStringList(), "空的共享目录列表");

        // 大小写：NTFS 不区分，所以对方用哪种写法都该能打开同一个文件
        checkAccepted(root + QStringLiteral("/SUB/INSIDE.TXT"), roots, "大小写不同的写法");
        // 但换了大小写之后返回的仍必须落在根内
        const QString resolved = afmu::resolveUnderRoots(root + QStringLiteral("/SUB/INSIDE.TXT"), roots);
        check(resolved.startsWith(root, Qt::CaseInsensitive),
              QStringLiteral("大小写不同的写法解析后仍应在根内"));
    }

    // ------------------------------------------------------------ §4.4 自动改名
    std::fprintf(stderr, "\n== 自动改名 ==\n");
    {
        const QString base = QDir(tmp.path()).canonicalPath();
        QDir(base).mkpath(QStringLiteral("dl"));
        const QString dir = base + QStringLiteral("/dl");

        checkEqual(afmu::uniqueTarget(dir, QStringLiteral("a.txt")),
                   dir + QStringLiteral("/a.txt"), "不存在时用原名");
        makeFile(dir + QStringLiteral("/a.txt"));
        checkEqual(afmu::uniqueTarget(dir, QStringLiteral("a.txt")),
                   dir + QStringLiteral("/a (1).txt"), "重名时加序号");
        makeFile(dir + QStringLiteral("/a (1).txt"));
        checkEqual(afmu::uniqueTarget(dir, QStringLiteral("a.txt")),
                   dir + QStringLiteral("/a (2).txt"), "序号递增");

        // NTFS 不区分大小写：A.TXT 和 a.txt 是同一个文件，必须也改名，
        // 否则"改名"会变成"覆盖"。用干净的目录测，免得撞上上面造的 a (1).txt
        QDir(base).mkpath(QStringLiteral("dl-case"));
        const QString caseDir = base + QStringLiteral("/dl-case");
        makeFile(caseDir + QStringLiteral("/a.txt"));
        checkEqual(afmu::uniqueTarget(caseDir, QStringLiteral("A.TXT")),
                   caseDir + QStringLiteral("/A (1).TXT"), "大小写不同也算重名");

        makeFile(dir + QStringLiteral("/noext"));
        checkEqual(afmu::uniqueTarget(dir, QStringLiteral("noext")),
                   dir + QStringLiteral("/noext (1)"), "没有扩展名时序号加在末尾");

        makeFile(dir + QStringLiteral("/.hidden"));
        checkEqual(afmu::uniqueTarget(dir, QStringLiteral(".hidden")),
                   dir + QStringLiteral("/.hidden (1)"),
                   "以点开头的名字，那个点不是扩展名分隔符");

        makeFile(dir + QStringLiteral("/pkg.tar.gz"));
        checkEqual(afmu::uniqueTarget(dir, QStringLiteral("pkg.tar.gz")),
                   dir + QStringLiteral("/pkg.tar (1).gz"),
                   "多重扩展名按最后一个点切（和 Linux 版行为一致）");
    }

    // ------------------------------------------------------------ 带重试的改名
    std::fprintf(stderr, "\n== 改名 / 删除 ==\n");
    {
        const QString base = QDir(tmp.path()).canonicalPath();
        const QString from = base + QStringLiteral("/rename-src.bin");
        const QString to = base + QStringLiteral("/rename-dst.bin");
        makeFile(from);
        check(afmu::renameWithRetry(from, to), QStringLiteral("正常情况下应当一次就成"));
        check(QFile::exists(to) && !QFile::exists(from), QStringLiteral("改名之后源应当不在了"));

        // 目标已存在时 Windows 的 rename 一定失败，这里不该被重试拖住太久
        makeFile(from);
        check(!afmu::renameWithRetry(from, to), QStringLiteral("目标已存在时应当失败"));

        check(afmu::removeWithRetry(to), QStringLiteral("删除已存在的文件"));
        check(afmu::removeWithRetry(base + QStringLiteral("/never-existed")),
              QStringLiteral("删一个本来就不存在的文件算成功"));
    }

    std::fprintf(stderr, "\n通过 %d / 失败 %d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
