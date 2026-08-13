#include "AppController.h"
#include "Config.h"
#include "I18n.h"
#include "Identity.h"
#include "ProtocolConstants.h"
#include "WinConsole.h"

#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

/**
 * 一个专门用来阻塞读标准输入的线程。
 *
 * Linux 那边这里是 `QSocketNotifier(0, Read)` —— 在 Windows 上不成立：这个平台的
 * QSocketNotifier 只认套接字句柄，拿 fd 0 去构造它，Qt 会警告一句然后永远不触发，
 * 表现是「按了回车没反应」。控制台句柄只能阻塞读，那就给它一个自己的线程。
 *
 * 有意放在匿名 namespace 之外：moc 对匿名 namespace 里的 Q_OBJECT 支持一直很微妙，
 * 而这个类只有一个用处，放外面不多花什么。
 */
class StdinReader : public QThread
{
    Q_OBJECT
public:
    using QThread::QThread;

signals:
    void lineRead(const QString &line);
    void eof();

protected:
    void run() override
    {
        QString line;
        if (afmu::win::readLine(&line))
            emit lineRead(line);
        else
            emit eof();
    }
};

namespace {

using afmu::win::writeErr;
using afmu::win::writeOut;

/**
 * `afmu.exe --fingerprint` —— 打印本机 v2 身份的 SPKI 指纹，没有就先生成。
 *
 * 这不只是给用户看的。PROTOCOL.md v2 §12 第 1 步要求两端做完身份层
 * 之后**立刻交叉验证指纹**：这里打印的值必须和下面这条命令的输出一致 ——
 *
 *   openssl x509 -in %LOCALAPPDATA%\afmu\identity.pem -pubkey -noout ^
 *     | openssl pkey -pubin -outform DER | openssl dgst -sha256
 *
 * mTLS 手工钉扎最常见的翻车就是两端对「SPKI」的理解差一层封装（有人哈希了
 * 整张证书，有人哈希了裸 EC 点）。等握手层调通再发现，症状是「证书明明对却
 * 一直不匹配」，非常难查。这一步 10 分钟，省半天。
 *
 * 不开图形界面，所以在 QGuiApplication 之前处理。
 */
int printFingerprint()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString path = QDir(base).filePath(QStringLiteral("afmu/identity.pem"));

    afmu::Identity identity;
    if (!identity.ensure(path)) {
        writeErr(QStringLiteral("身份不可用: %1\n").arg(identity.lastError()));
        return 1;
    }
    writeOut(QStringLiteral("文件      %1\n").arg(QDir::toNativeSeparators(path)));
    writeOut(QStringLiteral("SHA-256   %1\n")
                 .arg(QString::fromLatin1(identity.fingerprint().toHex())));
    writeOut(QStringLiteral("base32    %1\n").arg(identity.fingerprintBase32()));
    writeOut(QStringLiteral("展示形式  %1\n").arg(identity.fingerprintDisplay()));
    writeOut(QStringLiteral("\n交叉验证（上面的 SHA-256 必须和这条命令的输出一致）：\n"
                            "  openssl x509 -in \"%1\" -pubkey -noout ^\n"
                            "    | openssl pkey -pubin -outform DER | openssl dgst -sha256\n")
                 .arg(QDir::toNativeSeparators(path)));
    return 0;
}

/**
 * `afmu.exe --pair <host[:port]>` —— 不开界面跑一遍 v2 配对（草案 §4.2）。
 *
 * 存在的理由有两个，缺一个都不足以加这个开关：
 *
 * 1. 无头机器（家里那台一直开着的小机器、或者跑在 Windows 服务里的实例）
 *    本来就没有界面去点按钮。
 * 2. 配对流程里最要命的一步是「发起方必须显示**自己算的** SAS」，
 *    而这件事只有把整条流程真跑一遍才能验证。有了它，PC ↔ PC
 *    的配对可以在 CI/脚本里端到端跑，不用人对着屏幕点。
 *
 * 比对码打在标准输出上 —— 用户要拿它跟对端屏幕上的核对。
 */
int runPairing(AppController &controller, const QString &target)
{
    QString host = target;
    int port = afmu::kDefaultHttpPort;
    const int colon = host.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        const int p = host.mid(colon + 1).toInt(&ok);
        if (ok && p > 0) {
            port = p;
            host = host.left(colon);
        }
    }

    int exitCode = 1;
    QString lastSas;
    QObject::connect(&controller, &AppController::authChanged, &controller, [&] {
        if (!controller.authSas().isEmpty() && controller.authSas() != lastSas) {
            lastSas = controller.authSas();
            writeOut(QStringLiteral("比对码  %1\n").arg(lastSas));
            writeOut(QStringLiteral("对端指纹 %1\n").arg(controller.authPeerFingerprint()));
            writeOut(QStringLiteral("请在对端屏幕上核对这两行，然后点「允许」。\n"));
        }
        if (controller.authPending())
            return;
        const QString status = controller.authStatus();
        if (status.isEmpty() || status == QLatin1String("sending"))
            return;
        writeOut(QStringLiteral("结果    %1\n").arg(status));
        exitCode = status == QLatin1String("granted") ? 0 : 1;
        QCoreApplication::quit();
    });

    controller.requestPairing(host, port, host, QStringLiteral("unknown"));
    QCoreApplication::exec();
    return exitCode;
}

/**
 * `afmu.exe --accept-pairing` —— 无头机器上接受一次配对请求。
 *
 * 界面上那个弹窗在没有界面的机器上不存在，但配对**必须有人确认**：
 * 自动接受等于谁来都放行，那比不加密还糟。所以这里把同样的信息打到终端，
 * 等一个 y/n，然后退出 —— 一次一个，armed 一次就结束，不常驻。
 *
 * 打印的比对码要和发起方屏幕上的一致，这是用户唯一能识破中间人的地方。
 */
int acceptPairing(AppController &controller)
{
    writeOut(QStringLiteral("等待配对请求…（Ctrl-C 退出）\n"));

    int exitCode = 1;
    bool asked = false;
    auto *reader = new StdinReader(&controller);

    QObject::connect(reader, &StdinReader::eof, &controller, [] { QCoreApplication::quit(); });
    QObject::connect(reader, &StdinReader::lineRead, &controller, [&](const QString &line) {
        const bool yes = line.startsWith(QLatin1Char('y')) || line.startsWith(QLatin1Char('Y'));
        if (yes) {
            controller.approveIncomingAuth();
            writeOut(QStringLiteral("已允许。\n"));
            exitCode = 0;
        } else {
            controller.denyIncomingAuth();
            writeOut(QStringLiteral("已拒绝。\n"));
        }
        // **不能立刻退出。** 决定是要被对方取走的：发起方一秒轮询一次，
        // 服务一停它就只看到连接断开，然后当成"超时"—— 用户这边明明点了允许，
        // 那边却报失败。多留几秒，让它把结果取走。
        QTimer::singleShot(5000, &controller, [] { QCoreApplication::quit(); });
    });

    QObject::connect(&controller, &AppController::incomingAuthChanged, &controller, [&] {
        // 等 SAS 算出来再问：第 2 步之前没有可比对的东西，这时候问「允许吗」
        // 等于让用户凭感觉点。
        if (asked || !controller.incomingAuthPending() || !controller.incomingAuthIsPairing()
            || controller.incomingAuthSas().isEmpty()) {
            return;
        }
        asked = true;
        writeOut(QStringLiteral("\n设备    %1（%2）\n")
                     .arg(controller.incomingAuthName(), controller.incomingAuthHost()));
        writeOut(QStringLiteral("指纹    %1\n").arg(controller.incomingAuthFingerprint()));
        writeOut(QStringLiteral("比对码  %1\n").arg(controller.incomingAuthSas()));
        writeOut(QStringLiteral("这个码必须和对方屏幕上的一模一样。允许配对？[y/N] "));
        reader->start();
    });

    const int rc = QCoreApplication::exec();
    if (reader->isRunning()) {
        // 线程阻塞在 ReadConsoleW 上，等不到它自己回来。进程马上就要退出了，
        // terminate 掉是安全的：这个线程除了一个栈上的缓冲区什么都没碰。
        reader->terminate();
        reader->wait(1000);
    }
    return rc != 0 ? rc : exitCode;
}

} // namespace

int main(int argc, char *argv[])
{
    QString pairTarget;
    bool acceptMode = false;
    bool cliMode = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--fingerprint") == 0) {
            afmu::win::attachConsole();
            return printFingerprint();
        }
        if (qstrcmp(argv[i], "--pair") == 0 && i + 1 < argc) {
            pairTarget = QString::fromLocal8Bit(argv[i + 1]);
            cliMode = true;
        }
        if (qstrcmp(argv[i], "--accept-pairing") == 0) {
            acceptMode = true;
            cliMode = true;
        }
    }
    if (cliMode)
        afmu::win::attachConsole();

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("afmu"));
    app.setApplicationDisplayName(QStringLiteral("FileBridge"));
    app.setOrganizationName(QStringLiteral("aynux"));
    // 任务栏和 Alt-Tab 的图标来自 .exe 里的资源（resources/afmu.rc），窗口图标
    // 得单独给一次 —— 无边框窗口自己不画图标，但系统菜单和某些截图工具会用它
    app.setWindowIcon(QIcon(QStringLiteral(":/afmu.png")));

    // Basic 样式才允许完全自定义控件外观
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // AppController 自己最先建好 I18n，这里直接取
    AppController controller;

    if (!pairTarget.isEmpty())
        return runPairing(controller, pairTarget);
    if (acceptMode)
        return acceptPairing(controller);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Cfg"), controller.config());
    engine.rootContext()->setContextProperty(QStringLiteral("Lang"), controller.i18n());

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("Afmu", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}

#include "main.moc"
