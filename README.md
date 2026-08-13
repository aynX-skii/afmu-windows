# afmu-windows

FileBridge 的 Windows 桌面客户端：Qt 6 + Qt Quick，暗黑扁平风，标题栏 / 边框 / 圆角 / 缩放
全部自绘（`Qt.FramelessWindowHint` + `startSystemMove` / `startSystemResize`）。

界面和协议实现照搬 [afmu-linux](../afmu-linux)，差异集中在平台相关的那几处，
逐条列在下面的[「和 afmu-linux 的差异」](#和-afmu-linux-的差异)里。

协议严格按 `AndroidFileManagerUtils/docs/PROTOCOL.md` 实现，v1 和 v2 都做了，
客户端和服务端两半也都做了：

- **客户端**：发现设备 → 浏览对端目录 → 下载（带断点续传）/ 上传
- **服务端**：本机开同协议的 HTTP 服务，对端可以直接把文件推过来

v2（双向 TLS + 指纹钉扎，见 `AndroidFileManagerUtils/docs/PROTOCOL.md` 第二部分）
**三端都已经全部落地**。Android 和 Linux 两边都做过真机实测；本机这一端的验证
边界见下面的[「验证状态」](#验证状态)，**别默认 Windows ↔ 手机那条已经跑过**。

- 服务端按首字节自动分流：是 TLS 就走加密，否则按 v1 处理，一个端口同时服务新旧客户端。
- 客户端看配对表决定：对方在表里就必须走加密，**握手失败绝不退回明文**，
  指纹对不上直接中止 —— 不给「仍然继续」。
- 「设置 → 加密连接」能看到本机指纹，也能关掉明文；关掉之后本机只接受加密连接。
- **默认只加密**（PROTOCOL.md §8.2 第 3 阶段，三端都已落地）。全新安装开箱即是；
  升级安装走一次性迁移，关掉明文的同时往活动日志里写一条说明 —— 旧设备和浏览器
  从此连不上，不说的话用户只会看到「今天开始连不上了」，然后去查网络和防火墙。
  需要的话在「设置 → 加密连接」重新打开「允许未加密的旧版连接」，**只会被关一次**，
  打开之后不会在下次启动时又被改掉。访客模式默认关，升级安装保持原样。

---

## 构建

需要 Qt ≥ 6.5、CMake ≥ 3.21、支持 C++20 的编译器（MSVC 2019 16.11+ 或 MinGW 11+），
以及 OpenSSL 的开发文件。

二维码不需要额外依赖：Windows 上没有 libqrencode 这种一条命令就装上的系统包，
所以编码器是按 ISO/IEC 18004 自己实现的（`src/QrCode.cpp`），正确性靠
`tests/qrcode_test.cpp` 里 320 组独立生成的向量兜底。

### OpenSSL

v2 的设备身份要生成 EC P-256 自签证书，而 Qt 没有证书生成 API。

**别去 Qt 维护工具里找** —— 早年的在线安装器带过一个 "OpenSSL 3.x Toolkit"，现在
（2026 年的安装器）已经没有了，899 个包里搜 `openssl` 一条都没有。老机器上装过的
还留着，CMake 也仍然会去 `<Qt>/Tools/OpenSSLv3` 探一眼，但新装的机器指望不上它。

```powershell
# vcpkg：会从源码编一遍 OpenSSL，还会先拉 CMake、7-Zip、PowerShell 7 三个自带工具
# （加起来 180 MB 左右，都从 GitHub Releases 下）。国内网络下瓶颈是下载不是编译，
# 提前配好代理能省很多时间。
git clone --depth 1 https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg.exe install openssl:x64-windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# 已经有一份，直接指过去
cmake -S . -B build -DOPENSSL_ROOT_DIR=C:/OpenSSL-Win64
```

### 编译

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64
cmake --build build
.\build\afmu.exe
```

直接从构建目录跑的时候，Qt 的 DLL 要在 `PATH` 里（用 Qt 自带的
「Qt 6.x (MSVC 2022 64-bit)」命令行提示符最省事）。装到一个目录里则由
windeployqt 一次配齐：

```powershell
cmake --install build --prefix dist
.\dist\afmu.exe
```

`cmake --install` 会顺带把 `libcrypto-3-x64.dll` / `libssl-3-x64.dll` 拷进去。
**这两个不能少**：Qt 是运行时才去加载 OpenSSL 的，缺了它们程序照常启动、界面一切正常，
只有加密悄悄不可用 —— 日志里会写明原因，但很容易被略过。

### 测试

默认不建，打开也不引入新依赖（只多用 `Qt6::Core` / `Qt6::Network`）：

```powershell
cmake -S . -B build -G Ninja -DAFMU_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

| 测试 | 覆盖什么 |
|------|---------|
| `afmu_peerstore_test` | 140 条断言：配对表（指纹规范化、去重、落盘、坏文件留底）、SAS 与滚动 rid 的跨实现向量、配对码拼装、失败退避、配置读写，以及 §8.2 第 3 阶段那次明文迁移 |
| `afmu_pathsafety_test` | 越界防护：盘符 / UNC / 保留设备名 / 备用数据流 / 大小写 / 自动改名 |
| `afmu_qrcode_test` | 自带的二维码编码器，320 组向量逐位比对 |

`afmu_peerstore_test` 里的向量和 Android 端 `PeerCodecTest` / `PairSasTest` /
`RollingIdTest` 的断言刻意一一对应 —— 两端算得不一样的地方都不会抛异常，
只会安静地表现成功能没了。它改配置目录的办法和 afmu-linux 不同（那边设
`XDG_CONFIG_HOME` 就够了），原因写在测试里。

协议层面的一致性由 `AndroidFileManagerUtils/tests/` 下的两套黑盒套件验证，
用法见那边的 `tests/README.md`：

- `conformance.py` —— v1 线格式。**注意它会对每个共享目录发删除请求**，
  只能对着隔离配置起的实例跑。
- `conformance_v2.py` —— v2 门禁：未配对连接只能碰 `/api/pair-v2`、
  commit-reveal 三步、SAS 与本机独立算出的是否逐字相同。需要一个 `openssl`
  命令行（Git for Windows 自带一个，在 `C:\Program Files\Git\usr\bin`），不写任何文件。

  门禁那一组要求对端**访客模式关掉**才跑得起来（访客模式是 §4.2.4 唯一的例外，
  开着时那几条会跳过，而「跳过」看起来和「通过」一样无害）。

两套套件在本机的实测结果见下面的[「验证状态」](#验证状态)。

`tools/` 下还有三个脚本，都不参与构建：

```powershell
python tools/gen_qr_vectors.py   # 重新生成二维码测试向量（需要 pip install segno）
python tools/gen_icon.py         # 重新生成 resources/afmu.ico 和 afmu.png
python tools/check_i18n.py       # 检查 Tr.t()/T() 的每条文案都在翻译表里
```

---

## 界面

| 页面 | 做什么 |
|------|--------|
| **设备** | 广播扫描局域网、请求授权连接、显示配对二维码、填对端 token、手动输入 `IP:PORT` |
| **浏览文件** | 面包屑导航、多选下载、新建目录、删除、拖文件进窗口即上传 |
| **传输** | 每个任务的进度 / 速度 / 剩余时间，可取消、重试、在资源管理器里定位 |
| **接收服务** | 开关本机服务端、显示本机 token 和地址、管理共享目录、活动日志 |
| **设置** | 设备名、端口、收件箱、下载目录、发现超时、界面语言、本机指纹与加密开关、已配对设备（v2 配对表） |

快捷键：`Ctrl+1..5` 切页，`F5` / `Ctrl+R` 刷新当前目录。

### 界面语言

中英双语。默认**跟随系统语言**（系统显示语言是中文就用中文，否则英文），在
「设置 → 界面 → 语言」可以改成固定中文或英文。选择写进
`%LOCALAPPDATA%\afmu\config.json` 的 `language` 字段（`system` / `zh` / `en`），
下次启动自动加载，覆盖系统语言。切换即时生效，不用重启。

翻译表内置在 `src/I18n.cpp` 里，没走 Qt Linguist 的 `.ts` / `.qm` 流程 —— 和
afmu-linux 保持一致，两边的文案表因此可以直接对照。漏翻只会显示中文而不会崩，
所以有 `tools/check_i18n.py` 扫一遍。

---

## 用起来

### 从手机拉文件

1. 手机 App 打开服务。
2. 本机「设备」页点「扫描局域网」，双击列出的设备（或点「连接」）。
3. **手机上会弹一个授权通知**，点「允许」即可 —— token 自动送过来，不用手抄。
   两边屏幕上各显示一个 4 位确认码，一致才点允许。
4. 切到「浏览文件」，双击文件即开始下载。

也可以照旧手抄：把手机首页那 10 位 token 填进「设备」页的输入框，填了就用填的那个，
不会再弹授权。token 在手机上重新生成过之后这里会拿到 401，此时同样会自动改走授权流程。

授权连接的约束（细节见 [PROTOCOL.md §3.8](https://github.com/aynX-skii/AndroidFileManagerUtils/blob/main/docs/PROTOCOL.md)）：
同一时刻只允许一个待决请求，被拒绝的地址会进冷却，60 秒没确认按拒绝处理，
两端都可以在设置里把这个功能整个关掉。

### 两台 PC 之间

同一套流程，方向反过来也成立：本机的服务端也实现了 `/api/authorize`，所以另一台跑
FileBridge 的 PC 扫到本机之后点「请求授权」，**本机**会弹确认框，点「允许」才把本机
token 交出去。之后对方回填自己的 token（§3.9），两个方向一次配好。

Windows ↔ Linux 也是同一套：两边的 `os` 字段不同，别的完全一样。

前提是本机的接收服务在跑 —— 被发现、被连接、收文件都靠它，所以它**默认随应用启动**。
另外，连接任何设备时也会顺手把它带起来：报给对方的端口必须真有人听着，否则对方推文件
过来只会撞上一句 "Failed to connect"，而本机这边毫无提示。

不想让它自己起，去「接收服务」页取消「启动应用时自动开启服务」，选择会被记住。
服务跑哪一套协议看「设置 → 加密连接」：默认只加密，是 mTLS + 指纹钉扎，任何网络都能开；
手动打开「允许未加密的旧版连接」之后，那条路仍然是 HTTP + 10 位 token，只防误连，
别在公共 Wi-Fi 上开着。详见下面的[「安全边界」](#安全边界)。

### 把文件推给手机

连上以后进到目标目录，直接把文件拖进窗口，或点右上角「上传文件」。
目录会被跳过并提示（暂不支持递归上传）。

### 让手机推到本机

1. 「接收服务」页点「启动服务」。
2. 点「显示配对二维码」，在手机 App 里点「扫码连接」对着扫。
3. 扫完两个方向就都通了：手机拿到本机地址和 token，同时把自己的 token 回填过来，
   本机也直接连上。推过来的文件落到收件箱，同时出现在「传输」页。

不想扫码就照旧：把页面上显示的**本机 token** 抄到手机 App 的「PC token」输入框。

> **码里是什么，取决于加密可不可用**，两种情况的泄露后果完全相反：
>
> - 加密可用（默认）→ `v=2`，里面是**本机指纹**，没有 token。指纹是公开信息，
>   截图、转发、投屏都不损失什么 —— 它只是让对方知道该钉扎哪一把钥匙，
>   而钥匙本身出不去。扫完还要两边比对 8 位码才算数。
> - 加密不可用（OpenSSL 没加载上）→ 退回 `v=1`，里面是**明文 token**，
>   等价于把本机的访问权交出去，截图、转发、投屏都要当成泄露处理。
>
> 界面上「加密连接」那一栏写着当前是哪一种。拼装逻辑在 `afmu::buildPairUri`。

对端只能访问「共享目录」列表里的目录及其子目录，路径会先 canonicalize 再校验，
越界一律按 404 返回，不泄露真实原因。收件箱始终自动包含在共享目录里。

**默认只共享收件箱一个目录。** 想让手机浏览更多内容，在「接收服务」页显式添加 ——
默认把整个用户目录开成可读可写可删太宽了。共享根目录本身也删不掉（403），
根目录里的单个文件和子目录仍然可以正常删除。

下载落到「设置 → 下载目录」（默认 `%USERPROFILE%\Downloads\FileBridge`）。传输中是
`<文件名>.<远端路径指纹>.afmu-part`，完整收完才改名成正式名 —— 中断了绝不会留下
半个文件冒充完整文件。重试同一个任务会拿 `.afmu-part` 的大小当 `Range` 起点续传。

文件名里带远端路径指纹是必要的：只用文件名的话，两个不同目录下的同名文件（或上次失败
遗留的残片）会共用同一个 part 文件，续传起点就是错的，落盘的文件会静默损坏。
残片大小 ≥ 已知总大小时也会被当成陈旧数据丢弃重来。

### 没有 Wi-Fi 时

```powershell
adb forward tcp:18765 tcp:8765     # 本机 127.0.0.1:18765 就是手机的服务端
adb reverse tcp:8765  tcp:8765     # 反过来，让手机能访问本机的服务端
```

然后在「设备」页手动连 `127.0.0.1:18765`。

### 命令行开关

`afmu.exe` 是 GUI 子系统的程序（双击不会弹黑框），但下面三个开关仍然能在终端里用 ——
它们会把自己接回调用方的控制台：

```powershell
afmu.exe --fingerprint          # 打印本机 v2 身份的 SPKI 指纹，没有就先生成
afmu.exe --pair 192.168.1.30    # 不开界面跑一遍 v2 配对，比对码打在标准输出上
afmu.exe --accept-pairing       # 无头机器上接受一次配对请求，等一个 y/n
```

---

## 连不上的时候

按可能性从高到低：

1. **防火墙**。第一次监听端口时 Windows 会弹一个对话框问要不要放行，点了「取消」
   或者被组策略静默拒绝的话，本机这边一切正常 —— 端口在听、日志没有错误 ——
   只有对端超时。检查「Windows 安全中心 → 防火墙和网络保护 → 允许应用通过防火墙」
   里有没有 `afmu.exe`，**专用网络**那一栏是否勾上。
2. **网络配置文件是「公用」**。公用网络下防火墙默认拦掉入站，而家里的 Wi-Fi 有时
   会被识别成公用。在「设置 → 网络和 Internet」里把当前网络改成「专用网络」。
3. **路由器开了 AP 隔离**。广播被吃掉，扫描列表一直是空的。这种情况下手动输入
   `IP:PORT` 仍然可以连。
4. **加密不可用**（日志里会写）。OpenSSL 的两个 DLL 不在 `afmu.exe` 旁边也不在
   `PATH` 里，见上面的[构建](#构建)。

未签名的 exe 第一次运行可能被 SmartScreen 拦下（「Windows 已保护你的电脑」），
点「更多信息 → 仍要运行」。

---

## 和 afmu-linux 的差异

界面、协议、状态机是同一份代码。改动集中在这些地方，每一处的原因都写在对应的注释里：

| 位置 | 差异 |
|------|------|
| `src/QrCode.cpp` | 不用 libqrencode，按 ISO/IEC 18004 自己实现（Windows 上没有对应的系统包） |
| `src/PathSafety.cpp` | 越界防护整个重写：盘符 / UNC 根、盘符相对路径、备用数据流、`\\?\` 设备命名空间、保留设备名、结尾的点和空格 |
| `src/PathSafety.cpp` | 改名 / 删除带退避重试（刚写完的文件常被杀软和索引器占着，POSIX 上没有这个问题） |
| `src/WinConsole.cpp` | 新增。GUI 子系统的程序怎么把命令行开关的输出送回控制台，以及宽字符输出（绕开代码页） |
| `src/main.cpp` | 读标准输入改用线程（Windows 的 `QSocketNotifier` 只认套接字，`fd 0` 永远不触发） |
| `src/Discovery.cpp` | 发现端口改成**独占**绑定（Windows 的 `SO_REUSEADDR` 允许后来者抢走流量） |
| `src/TransferModel.cpp` | 「定位到文件」用 `explorer /select` 选中文件本身，而不是只打开目录 |
| `src/Identity.cpp`、`src/JsonFile.cpp` | 去掉 `setPermissions(0600)`（在这个平台上只会动只读属性，做不出「仅本用户可读」），改由 `%LOCALAPPDATA%` 的 ACL 承担 |
| `src/AppController.cpp` | 启动服务时提示一次防火墙 —— Windows 上「对方连不上」几乎总是它 |
| `src/HttpServer.cpp` | 加密不可用时的提示改成「OpenSSL DLL 没加载上」，而不是「这个 Qt 构建没有 TLS」 |
| `src/HttpServer.cpp` | 共享目录的显示名认得出盘根（`D:` ）和 UNC 共享 |
| `resources/` | 新增。exe 图标、版本信息、应用清单（PerMonitorV2 DPI 感知 + UTF-8 活动代码页） |
| `CMakeLists.txt` | `WIN32` 子系统、`/utf-8`、windeployqt、OpenSSL 自动探测 |
| `tests/peerstore_test.cpp` | 换配置目录用 `QStandardPaths::setTestModeEnabled`（这个平台的 `QStandardPaths` 走 `SHGetKnownFolderPath`，`XDG_CONFIG_HOME` 设了也没用）；`peers.json` 不断言 0600，理由同上一行 |

配置目录也不同：Linux 是 `~/.config/afmu/`，这里是 `%LOCALAPPDATA%\afmu\`。
选 Local 而不是 Roaming 是有意的 —— `identity.pem` 里那把私钥**就是这台设备的身份**，
跟着漫游配置文件复制到别的机器上，等于两台机器共用一个身份，而钉扎的全部意义
就是「一条记录对一台设备」。

上报给对端的 `os` 字段是 `windows`。手机和 Linux 端都是原样存下来只用于显示，
不参与任何判定。

---

## 验证状态

把没验过的东西写成验过的，比不写更糟，所以这一节按实际做到的程度分两档。

**跑过并且通过**（MSVC 14.51 / Qt 6.11.1 / vcpkg OpenSSL 3.6.3 / Windows 11）：

- 完整构建：`/W3` 下零警告，`afmu.exe` 链接通过，`ctest` 三个测试全绿。
- `afmu_peerstore_test` —— 140 条断言全过，含 §8.2 第 3 阶段那次明文迁移的四种情形
  （新装不提示、升级关掉并提示、只做一次、用户重新打开之后必须留住）。
- `afmu_qrcode_test` —— 320 组向量逐位一致，加上容量边界、空输入、越界读取。
  自己写的那份编码器和 segno 生成的图案**逐个模块相同**。
- `afmu_pathsafety_test` —— 65 条断言全过。盘符 / UNC / 盘符相对路径 / `\\?\` /
  备用数据流 / 内嵌 NUL / 保留设备名 / 结尾的点和空格 / 大小写重名 / 带重试的改名。
- **`conformance_v2.py` 黑盒门禁 —— 16 通过 / 0 失败 / 2 跳过**（对着只加密模式下
  起的隔离实例跑）。这一套用一张现生成的 EC P-256 证书扮演「一台没见过的新设备」，
  验的是门开得对不对：未配对的 TLS 连接除 `/api/pair-v2` 外一律 403（`/`、
  `/api/info`、`/api/list`、`/api/download`、`/api/ticket` 逐个试过）、
  commit-reveal 三步、commit 对不上作废整个 session、访客模式关掉时不出示证书
  的加密连接进不来。**SAS 那一条最值钱**：服务端回的和套件自己独立算出的逐字相同。
  两条跳过是设计如此 —— 它们只在访客模式开着 / 明文开着时才适用。
- **`conformance.py` v1 线格式 —— 61 通过 / 0 失败 / 5 跳过 / 1 已知偏差**
  （换成访客模式 + 明文的配置重起一次）。Range 四种写法 + 416 + 畸形 Range、
  chunked、multipart 多文件段、截断上传不留残片、下载券、自动改名、文件名安全化、
  越界防护、Host / Origin 检查、token 退避、keep-alive、`GET /` 免鉴权 —— 都过了。
  五条跳过里四条是发现协议（这台机器上广播没回来），一条要 `--slow` 才跑。

  > 头一次跑的时候这里是 1 条失败，而**失败的是套件不是实现**：它断言
  > `roots[].path` 以 `/` 开头（写在 Windows 实现存在之前），而这里报的是
  > `C:/…` —— 在这个平台上那就是绝对路径，PROTOCOL.md §3.1 也只说「绝对路径」。
  > 已经在上游改成 `is_absolute()`，POSIX / 盘符 / UNC 三种都收。

  已知偏差那条（截断应回 400 而不是断开连接）和 afmu-linux 同源，
  走的是同一份 Qt 代码，PROTOCOL.md 里有记录。
- **§8.2 第 3 阶段的迁移在真实启动上也验过**：手写一份没有 `plaintextStage3`
  的旧配置，起一次 → 明文被关掉、标记写进文件、明文客户端当场连不上；
  手动把明文改回 `true` 再起一次 → **没有**被第二次关掉。
- `afmu.exe --fingerprint`：输出正确，重定向到文件也正确（GUI 子系统程序的老坑，
  见 `src/WinConsole.cpp` 的注释），身份落在 `%LOCALAPPDATA%\afmu\identity.pem`。
- **指纹交叉验证**（PROTOCOL.md v2 §12 第 1 步）：`--fingerprint` 打印的
  SHA-256、界面日志里那一行、以及
  `openssl x509 -pubkey | openssl pkey -pubin -outform DER | openssl dgst -sha256`
  三方逐字符一致。SPKI 那层封装在这个平台上没有理解偏差。
- 界面真跑起来了：无边框窗体、自绘标题栏和窗口按钮、导航、五个页面、中文渲染、
  自动起服务并监听 `0.0.0.0:8765`、共享目录默认落在 `%USERPROFILE%\Downloads\FileBridge`。
- **二维码端到端**：点「显示配对二维码」，把窗口截图交给 OpenCV（独立解码器）解，
  还原出 `afmu://pair?v=2&…&fp=…&os=windows`，`fp` 和上面那个指纹一致。
  这条链路把自写编码器 → `QrImage` 绘制 → 屏幕像素 → 解码整段串了起来。

**没跑过**：

- **两台真实设备之间的端到端**，一次都没有。上面那两套黑盒是本机对本机跑的
  （套件当客户端，`afmu.exe` 当服务端），所以验到的是**服务端**这一半。
  客户端那一半 —— 尤其是 v2 配对走完之后的第一个真实请求（配对前后各清一次
  空闲连接池那件事）—— 只有代码和注释，没有对着真对端跑过。
  afmu-linux 的 [README「已验证的行为」](../afmu-linux/README.md)那一节列的清单
  同样适用于这里，跑之前**不要**默认它们已经通过。
- Windows ↔ Android 的加密链路。Android 一侧已经真机实测（见 PROTOCOL.md §5.3），
  Linux ↔ Android 也跑通了，但**这不能推出 Windows ↔ Android 也通**。
- 手机从本机拉文件时的断点续传。Android 那边刚加上（`Range` + 416 重来），
  本机的服务端有对应的 206 / `Content-Range` / 416，`conformance.py` 也验过 Range，
  但两者没有对接过。
- 多显示器 / 不同缩放比例下的窗口行为、拖拽上传、`explorer /select`
  这类要有人看着屏幕操作才知道对不对的部分。
- MinGW 构建。CMakeLists 里那条分支是照着写的，没编过。
- CI。afmu-linux 和 AndroidFileManagerUtils 都加了 `.github/workflows/ci.yml`，
  这边没有 —— 这个目录还不是一个 git 仓库，而且 Windows runner 上装 Qt 和 OpenSSL
  的那几步没法在本地验证，写一份从没跑过的 YAML 不如不写。

---

## 代码结构

```
src/
  Protocol.*        协议常量、常数时间 token 比较、token 生成、配对 URI、下载券
  ProtocolConstants.h  ⚙ 由 AndroidFileManagerUtils/tools/gen_constants.py 生成，别手改
  QrCode.*          按 ISO/IEC 18004 自己实现的编码器（字节模式，版本自动选）
  QrImage.*         QrView —— 把二维码画到 QML 里的 QQuickPaintedItem
  PathSafety.*      §4.1 越界防护 / §4.2 文件名安全化 / §4.4 自动改名，外加带重试的改名
  WinConsole.*      GUI 程序怎么用命令行开关：接回控制台、宽字符输出、阻塞读一行
  JsonFile.*        「文件不存在」和「文件在但读不出来」分开处理，坏文件留底不覆盖
  Config.*          %LOCALAPPDATA%\afmu\config.json，含 §8.2 第 3 阶段的迁移
  I18n.*            中英文案表、语言解析与持久化
  Discovery.*       UDP 8766：广播探测 + 应答，过滤自己的应答，滚动 rid
  PeerClient.*      客户端 HTTP 封装，token 走 X-AFMU-Token，钉扎挂在 encrypted 上
  Models.*          设备列表、远端目录列表
  TransferModel.*   传输队列（并发 2），下载续传、上传进度、速度与 ETA
  AuthRequests.*    待决状态机：v1 授权 + v2 配对（共用一个位置）
  AuthThrottle.*    token 猜错的指数退避（§2.2）
  HttpServer.*      服务端：异步 HTTP/1.1，含 Range、chunked、multipart 流式解析
  AppController.*   串起来暴露给 QML 的门面

                    ── 以下是 v2（零信任）那一层 ──
  Identity.*        设备身份：EC P-256 自签证书（OpenSSL 生成）、SPKI 指纹、base32
  Tls.*             双向 TLS 的配置与对端指纹提取
  PeerStore.*       配对表 peers.json，v2 的访问控制列表本身
  PairSas.*         8 位比对码，commit-reveal 绑定会话随机数（§4.2.2）
  RollingId.*       发现应答里的滚动 rid，不再广播设备名（§6.1）
qml/
  Theme.qml         唯一的颜色 / 间距 / 字号来源
  Tr.qml            文案入口 Tr.t("中文")，切换语言时绑定自动重算
  Main.qml          无边框窗体、缩放边、状态栏、拖拽上传
  TitleBar.qml      自绘标题栏与窗口按钮
  AppIcon.qml       Qt Quick Shapes 画的线性图标，无外部资源
  *Page.qml         五个页面
resources/
  afmu.rc           exe 图标 + 版本信息 + 清单
  afmu.manifest     PerMonitorV2 DPI 感知、supportedOS、UTF-8 活动代码页
  afmu.ico/.png     由 tools/gen_icon.py 画出来，不是提交进来的二进制
tools/
  gen_qr_vectors.py 用 segno 生成 tests/qrcode_test.cpp 的向量
  _qrref.py         QrCode.cpp 的 Python 对照实现，被上面那个 import
  gen_icon.py       画图标
  check_i18n.py     检查文案表有没有漏
```

`_qrref.py` 为什么存在：向量的权威是 segno，但只有一份实现的话，对不上时只知道
「不一样」，不知道错在算法还是错在抄写。这份 Python 参考和 `src/QrCode.cpp`
结构一一对应（同样的表、同样的函数划分），于是 `gen_qr_vectors.py` 能先让它和
segno 逐位对齐（算法对了），再把向量交给 C++ 测试（抄对了）—— 两类错误分开。

---

## 安全边界

**有两套，取决于「设置 → 加密连接」那个开关。** 界面上常驻显示当前实际是哪一套：

| | v2（加密，默认） | v1（明文，需手动打开） |
|---|---|---|
| 传输 | TLS 1.3 双向认证，自签证书 + SPKI 指纹钉扎 | 明文 HTTP |
| 凭什么信对面 | 对方持有配对表里那把私钥 | 10 位共享 token |
| 挡嗅探 / 中间人 | 都挡得住 | **都挡不住** |
| 什么网络能开 | 任意，包括公共 Wi-Fi | 只在你信任的网络 |

一台设备一旦用 v2 连过一次，配对表里就会给它置上 `pinned`，从此**不再允许它退回明文**
（PROTOCOL.md v2 §8.2）。这个标志只升不降，要降只能由用户手动解除配对。

私钥在 `%LOCALAPPDATA%\afmu\identity.pem`。**这和 Android 的 TEE 不对等** ——
那边私钥在安全硬件里出不来，这边一次完整的用户目录备份就能把身份带走。
挡住别的用户的是 `%LOCALAPPDATA%` 这个目录的 ACL，不是文件权限位（Windows 上
`setPermissions` 只动只读属性，做不出「仅本用户可读」，所以这边干脆不装那个样子）。
接 DPAPI 或 Windows 凭据管理器是 v2 之后的事，和 afmu-linux 的 keyring 一起记在
PROTOCOL.md §14。

威胁模型的完整版（防谁、不防谁、加密之后还剩什么泄露）见
[PROTOCOL.md](https://github.com/aynX-skii/AndroidFileManagerUtils/blob/main/docs/PROTOCOL.md)
第二部分 §1 和 §10。

---

## 许可证

[LGPL-3.0](COPYING.LESSER)（在 [GPL-3.0](COPYING) 之上附加权限）。

Qt 6 本身以 LGPLv3 提供，本项目**动态链接** Qt 的共享库。`cmake --install` 用
windeployqt 把那些 DLL 拷到 exe 旁边，那仍然是动态链接 —— LGPL 要求的
「使用者能换掉这个库」照样满足：换一份同版本的 Qt 6 DLL 上去就行，
不需要重新编译本程序。

另外两个依赖：OpenSSL 3.x 是 Apache-2.0，相容；二维码这边**没有** libqrencode ——
`src/QrCode.cpp` 是按 ISO/IEC 18004 自己写的，所以 afmu-linux 那条 LGPL-2.1
的依赖在这里不存在。

如果你要**静态链接** Qt 再分发，那条路 LGPL 也允许，但你得自己提供能重新链接的
材料（目标文件或源码）。上面那种拷 DLL 的方式不涉及这个。
