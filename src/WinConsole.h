#pragma once

#include <QString>

/**
 * 把命令行那几个开关（`--fingerprint` / `--pair` / `--accept-pairing`）在
 * Windows 上变得可用。
 *
 * afmu.exe 是 GUI 子系统的程序 —— 必须是，否则双击图标会先弹一个黑框。代价是
 * 它启动时**根本没有控制台**：`printf` 写进一个无效句柄，什么都不显示，用户在
 * PowerShell 里敲 `afmu --fingerprint` 只会看到什么都没发生，然后以为程序坏了。
 *
 * 所以这些开关要先把自己接回父进程的控制台（`AttachConsole`），没有父控制台
 * （从资源管理器双击）就自己开一个。
 */
namespace afmu {
namespace win {

/** 接上父进程的控制台，没有就新开一个。重复调用无害。 */
void attachConsole();

/**
 * 往标准输出写一行。
 *
 * 不用 `printf` + `qPrintable()`：那条路会把 QString 转成本地 8 位编码（简体中文
 * 系统上是 GBK），而控制台的代码页未必是 GBK —— PowerShell 7 默认 UTF-8，
 * Windows Terminal 也常被改成 UTF-8。结果是一屏乱码，而这些开关打印的恰好是
 * 指纹和比对码，用户要**逐字符核对**的东西。
 *
 * 真控制台走 WriteConsoleW（宽字符，绕开代码页），重定向到文件/管道时写 UTF-8。
 */
void writeOut(const QString &text);
void writeErr(const QString &text);

/**
 * 阻塞读一行标准输入，读到 EOF 返回 false。
 *
 * 在后台线程里调用 —— Windows 的 QSocketNotifier 只认套接字，拿 fd 0 构造它
 * 在这个平台上是无效的（Qt 会直接警告并且永不触发）。
 */
bool readLine(QString *out);

} // namespace win
} // namespace afmu
