#include "WinConsole.h"

#include <QByteArray>

#include <windows.h>

#include <cstdio>
#include <iterator>
#include <string>

namespace afmu {
namespace win {

namespace {

bool g_attached = false;

/** 句柄是不是真控制台（而不是被重定向到文件或管道）。 */
bool isConsole(HANDLE h)
{
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode) != 0;
}

void writeTo(DWORD stdHandle, const QString &text)
{
    const HANDLE h = GetStdHandle(stdHandle);
    if (h == INVALID_HANDLE_VALUE || h == nullptr)
        return;
    if (isConsole(h)) {
        // 宽字符直接写，代码页完全不参与 —— 控制台是 GBK 还是 UTF-8 都不影响
        DWORD written = 0;
        const std::wstring w = text.toStdWString();
        WriteConsoleW(h, w.c_str(), DWORD(w.size()), &written, nullptr);
    } else {
        // 重定向出去了：写 UTF-8。`afmu --fingerprint > fp.txt` 得到的是一个
        // 能被别的工具读的文件，不是当前 ANSI 代码页的产物
        const QByteArray utf8 = text.toUtf8();
        DWORD written = 0;
        WriteFile(h, utf8.constData(), DWORD(utf8.size()), &written, nullptr);
    }
}

} // namespace

/** 句柄能不能用。GUI 程序从资源管理器启动时这三个多半是 NULL。 */
bool usable(HANDLE h)
{
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

void attachConsole()
{
    if (g_attached)
        return;
    g_attached = true;

    // **先把继承来的句柄记下来。**
    //
    // AttachConsole 和 AllocConsole 都会把进程的标准句柄换成新控制台的，而调用方
    // 很可能做了重定向：`afmu --fingerprint > fp.txt`、`afmu --pair … | findstr`。
    // 换掉之后输出跑进控制台，用户拿到的是一个**空文件** —— 而且命令本身退出码是 0，
    // 看起来一切正常。这个坑只有真跑一遍才会发现。
    const DWORD ids[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    HANDLE inherited[3] = {};
    for (int i = 0; i < 3; ++i)
        inherited[i] = GetStdHandle(ids[i]);

    // 从终端里启动的：接回父进程的控制台，输出就出现在用户敲命令的那个窗口里。
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && GetLastError() != ERROR_ACCESS_DENIED) {
        // 没有父控制台（从资源管理器双击）。只有在输出确实无处可去的时候才自己开一个 ——
        // 输出已经被重定向到文件/管道时开控制台纯属多弹一个黑框。
        if (!usable(inherited[1]))
            AllocConsole();
    }

    static const char *const streams[3] = { "CONIN$", "CONOUT$", "CONOUT$" };
    static const char *const modes[3] = { "r", "w", "w" };
    FILE *const crt[3] = { stdin, stdout, stderr };
    for (int i = 0; i < 3; ++i) {
        if (usable(inherited[i])) {
            SetStdHandle(ids[i], inherited[i]); // 重定向优先，原样还回去
        } else {
            // 本来就没有的那几个才绑到控制台上。我们自己不用 printf，
            // 但 Qt 的 qWarning 之类会用。
            FILE *f = nullptr;
            freopen_s(&f, streams[i], modes[i], crt[i]);
        }
    }
    SetConsoleOutputCP(CP_UTF8);
}

void writeOut(const QString &text) { writeTo(STD_OUTPUT_HANDLE, text); }
void writeErr(const QString &text) { writeTo(STD_ERROR_HANDLE, text); }

bool readLine(QString *out)
{
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr)
        return false;

    if (isConsole(h)) {
        wchar_t buf[256];
        DWORD read = 0;
        if (!ReadConsoleW(h, buf, DWORD(std::size(buf)), &read, nullptr) || read == 0)
            return false;
        if (out)
            *out = QString::fromWCharArray(buf, int(read)).trimmed();
        return true;
    }

    // 管道/文件：按字节读到换行为止
    QByteArray line;
    char c = 0;
    DWORD read = 0;
    while (ReadFile(h, &c, 1, &read, nullptr) && read == 1) {
        if (c == '\n')
            break;
        line.append(c);
    }
    if (line.isEmpty() && read != 1)
        return false;
    if (out)
        *out = QString::fromUtf8(line).trimmed();
    return true;
}

} // namespace win
} // namespace afmu
