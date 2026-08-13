cmake_minimum_required(VERSION 3.21)

# RC 也要声明：图标和清单在 resources/afmu.rc 里，不声明这门语言的话
# .rc 会被当成一个不认识的源文件默默跳过 —— 编得过，但 exe 上没有图标。
project(afmu VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "这是 FileBridge 的 Windows 版。Linux 上请用同仓库的 afmu-linux。")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 警告常驻。代码本来就是干净的（整棵树零警告），开着的意义是新写的东西没法
# 悄悄带进一条 —— 事后再开永远要先还一堆债。afmu-linux 那边是 -Wall -Wextra，
# 这边 MSVC 用 /W3（下面 target 级再给一次，这里管的是 MinGW 那条分支）。
# 不开 -Werror / /WX：Qt 的 moc 生成代码和不同编译器版本的口味不一样，
# 让别人的机器编不过换不来任何东西。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2)

# ---------------------------------------------------------------- OpenSSL
#
# v2 的设备身份要生成 EC P-256 自签证书，而 Qt 没有证书生成 API（只能用现成的）。
# Linux 上 libcrypto 本来就被 Qt Network 传递链进来了，find_package 直接就有；
# Windows 上得自己找。按可能性从高到低试三条路，都失败才报错并告诉用户怎么办。
#
#   1. vcpkg 的 toolchain，或者自己指了 OPENSSL_ROOT_DIR —— find_package 直接就有
#   2. <Qt>/Tools/OpenSSLv3 —— **较新的 Qt 在线安装器已经不提供这个包了**
#      （2026 年的安装器里搜 openssl 一条都没有）。老机器上装过的还在，所以仍然探一下，
#      但别把它当成一条能推荐给新用户的路。
#   3. 都没有 —— 报错，顺便把可行的装法写在错误信息里
# Qt6_DIR 指到 <Qt>/<版本>/<套件>/lib/cmake/Qt6，往上三级就是套件根目录
get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)

if(NOT OPENSSL_ROOT_DIR)
    foreach(_candidate
            "${_qt_prefix}/../../Tools/OpenSSLv3/Win_x64"
            "${_qt_prefix}/../../Tools/OpenSSLv3/Win_x86"
            "$ENV{ProgramFiles}/OpenSSL-Win64")
        if(EXISTS "${_candidate}/include/openssl/evp.h")
            get_filename_component(OPENSSL_ROOT_DIR "${_candidate}" ABSOLUTE)
            message(STATUS "OpenSSL: 用 Qt 附带的 ${OPENSSL_ROOT_DIR}")
            break()
        endif()
    endforeach()
endif()

find_package(OpenSSL)
# vcpkg 的 toolchain 找到 OpenSSL 时不会设 OPENSSL_ROOT_DIR，只给 include 目录。
# 下面拷 DLL 那一步要用它，从 include 反推一级即可。
if(OpenSSL_FOUND AND NOT OPENSSL_ROOT_DIR AND OPENSSL_INCLUDE_DIR)
    get_filename_component(OPENSSL_ROOT_DIR "${OPENSSL_INCLUDE_DIR}/.." ABSOLUTE)
endif()
if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR
        "找不到 OpenSSL 的开发文件。装一个再来：\n"
        "  vcpkg install openssl:x64-windows\n"
        "  然后加 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
        "或者已经有一份的话直接指过去：-DOPENSSL_ROOT_DIR=C:/OpenSSL-Win64\n"
        "（Qt 的在线安装器**不再**提供 OpenSSL 工具包，别去那里找）")
endif()

qt_standard_project_setup(REQUIRES 6.5)

set(AFMU_CPP
    src/main.cpp
    # 由 AndroidFileManagerUtils/tools/gen_constants.py 生成，别手改
    src/ProtocolConstants.h
    src/Protocol.h            src/Protocol.cpp
    # afmu-linux 用 libqrencode；Windows 上没有对应的系统包，所以这里是自己实现的，
    # 正确性靠 tests/qrcode_test.cpp 里 320 组独立生成的向量兜底
    src/QrCode.h              src/QrCode.cpp
    src/PathSafety.h          src/PathSafety.cpp
    src/JsonFile.h            src/JsonFile.cpp
    src/Config.h              src/Config.cpp
    src/Identity.h            src/Identity.cpp
    src/Tls.h                 src/Tls.cpp
    src/PairSas.h             src/PairSas.cpp
    src/RollingId.h           src/RollingId.cpp
    src/PeerStore.h           src/PeerStore.cpp
    src/I18n.h                src/I18n.cpp
    src/Discovery.h           src/Discovery.cpp
    src/AuthThrottle.h        src/AuthThrottle.cpp
    src/Models.h              src/Models.cpp
    src/PeerClient.h          src/PeerClient.cpp
    src/TransferModel.h       src/TransferModel.cpp
    src/AuthRequests.h        src/AuthRequests.cpp
    src/HttpServer.h          src/HttpServer.cpp
    src/AppController.h       src/AppController.cpp
    # Windows 专属：GUI 子系统的程序怎么把 --fingerprint 这类开关的输出送出去
    src/WinConsole.h          src/WinConsole.cpp
)

# WIN32 = 链成 GUI 子系统。少了它，双击 afmu.exe 会先弹一个黑色控制台窗口，
# 而且它会一直挂在那里。命令行开关的输出由 WinConsole.cpp 负责接回去。
qt_add_executable(afmu WIN32
    ${AFMU_CPP}
    resources/afmu.rc
)

set(AFMU_QML
    qml/Main.qml
    qml/Theme.qml
    qml/Tr.qml
    qml/AppIcon.qml
    qml/TitleBar.qml
    qml/WindowButton.qml
    qml/NavRail.qml
    qml/NavItem.qml
    qml/FlatButton.qml
    qml/IconButton.qml
    qml/AppTextField.qml
    qml/AppSwitch.qml
    qml/AppCheckBox.qml
    qml/AppScrollBar.qml
    qml/Card.qml
    qml/SectionLabel.qml
    qml/StatBadge.qml
    qml/StatusDot.qml
    qml/ProgressLine.qml
    qml/EmptyState.qml
    qml/Toast.qml
    qml/ConfirmDialog.qml
    qml/PromptDialog.qml
    qml/DevicesPage.qml
    qml/BrowsePage.qml
    qml/TransfersPage.qml
    qml/ServerPage.qml
    qml/SettingsPage.qml
    qml/PairDialog.qml
    qml/AuthWaitDialog.qml
    qml/IncomingAuthDialog.qml
)

# 全部平铺到模块根目录，保证每个文件都能作为 Afmu 的类型使用
foreach(f IN LISTS AFMU_QML)
    get_filename_component(_fn "${f}" NAME)
    set_source_files_properties("${f}" PROPERTIES QT_RESOURCE_ALIAS "${_fn}")
endforeach()

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/Tr.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(afmu
    URI Afmu
    VERSION 1.0
    QML_FILES ${AFMU_QML}
    # QML_ELEMENT 的类型必须走模块的 SOURCES，才会生成注册代码
    SOURCES
        src/QrImage.h src/QrImage.cpp
)

# 窗口图标（QGuiApplication::setWindowIcon）。exe 自己的图标走 resources/afmu.rc，
# 那是两套不同的东西：一个给运行中的窗口，一个给资源管理器里的文件。
qt_add_resources(afmu "appicon"
    PREFIX "/"
    BASE resources
    FILES resources/afmu.png
)

# 生成的 QML 类型注册代码是 #include <QrImage.h>，得让 src/ 在搜索路径里
target_include_directories(afmu PRIVATE src)

target_link_libraries(afmu PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    OpenSSL::Crypto
)

if(MSVC)
    # /utf-8：源码里全是中文字面量。不给的话 MSVC 按当前 ANSI 代码页读源文件，
    # 在非中文系统上直接编不过，在中文系统上编得过但字符串是 GBK —— 而 Qt 把
    # 窄字符串一律当 UTF-8，结果是界面上一片乱码。
    target_compile_options(afmu PRIVATE /utf-8 /permissive- /W3)
    # NOMINMAX / WIN32_LEAN_AND_MEAN：windows.h 的 min/max 宏会和 std::min/max 打架
    target_compile_definitions(afmu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    # 清单交给链接器：CMake 把 .manifest 源文件转成 /MANIFESTINPUT，和它自己生成的
    # 默认清单合并后嵌进去。**不能**同时让 resources/afmu.rc 也嵌一份 ——
    # exe 里出现两个 ID 为 1 的 RT_MANIFEST，cvtres 会报 CVT1100 直接链接失败。
    target_sources(afmu PRIVATE resources/afmu.manifest)
else()
    # MinGW
    target_compile_options(afmu PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_definitions(afmu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    # 这边链接器不管清单，只能从资源脚本里嵌
    set_source_files_properties(resources/afmu.rc
        PROPERTIES COMPILE_DEFINITIONS AFMU_RC_EMBED_MANIFEST)
endif()

# ---------------------------------------------------------------- 安装 / 打包
#
# windeployqt 把 Qt 的 DLL、QML 模块、平台插件拷到 exe 旁边 —— Windows 上没有
# 系统包管理器帮忙解依赖，缺一个插件的表现是"双击没反应，也没有任何报错"。
install(TARGETS afmu RUNTIME DESTINATION .)

find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_prefix}/bin")
if(WINDEPLOYQT_EXECUTABLE)
    install(CODE "
        execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
            --qmldir \"${CMAKE_CURRENT_SOURCE_DIR}/qml\"
            --no-translations --no-system-d3d-compiler --no-opengl-sw
            \"\${CMAKE_INSTALL_PREFIX}/afmu.exe\")
    ")
    # OpenSSL 的两个 DLL 不归 windeployqt 管：它只认 Qt 自己的依赖。
    # 少了它们，程序能起来，但一走到 TLS 就悄悄失败（Qt 是运行时动态加载的）。
    install(CODE "
        file(GLOB _ssl_dlls
            \"${OPENSSL_ROOT_DIR}/bin/libcrypto-3*.dll\"
            \"${OPENSSL_ROOT_DIR}/bin/libssl-3*.dll\")
        if(_ssl_dlls)
            file(INSTALL \${_ssl_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
        elseif(NOT EXISTS \"\${CMAKE_INSTALL_PREFIX}/libcrypto-3-x64.dll\")
            # vcpkg 会自己做 app-local 部署，所以先看一眼 DLL 是不是已经在了，
            # 别对着一个其实没问题的安装目录喊狼来了
            message(WARNING \"没找到 OpenSSL 的 DLL，装出来的目录在目标机器上会没有加密\")
        endif()
    ")
else()
    message(WARNING "找不到 windeployqt，install 出来的目录需要自己补 Qt 运行时")
endif()

# ---------------------------------------------------------------- 测试
#
# 默认不建。打开也不引入任何新依赖（只多用 Qt6::Core / Qt6::Network）。
#
#   cmake -S . -B build -DAFMU_TESTS=ON && cmake --build build
#   ctest --test-dir build --output-on-failure
option(AFMU_TESTS "编译单元测试" OFF)
if(AFMU_TESTS)
    enable_testing()

    add_executable(afmu_peerstore_test
        tests/peerstore_test.cpp
        src/PeerStore.h  src/PeerStore.cpp
        src/Identity.h   src/Identity.cpp
        src/JsonFile.h   src/JsonFile.cpp
        src/PairSas.h    src/PairSas.cpp
        src/AuthRequests.h src/AuthRequests.cpp
        src/AuthThrottle.h src/AuthThrottle.cpp
        src/I18n.h       src/I18n.cpp
        src/Config.h     src/Config.cpp
        src/RollingId.h  src/RollingId.cpp
        src/Protocol.h   src/Protocol.cpp
    )
    # Protocol.cpp 用到 QHostAddress（Host 白名单那一段），所以多一个 Qt6::Network
    target_link_libraries(afmu_peerstore_test PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)

    # 路径越界防护。Linux 版那套判断在这个平台上整个不成立（盘符、UNC、
    # 保留设备名、备用数据流、大小写），所以重写过，也必须单独测。
    add_executable(afmu_pathsafety_test
        tests/pathsafety_test.cpp
        src/PathSafety.h src/PathSafety.cpp
    )
    target_link_libraries(afmu_pathsafety_test PRIVATE Qt6::Core)

    # 自带的二维码编码器。向量由 tools/gen_qr_vectors.py 用 segno 生成。
    add_executable(afmu_qrcode_test
        tests/qrcode_test.cpp
        src/QrCode.h src/QrCode.cpp
    )
    target_link_libraries(afmu_qrcode_test PRIVATE Qt6::Core)

    foreach(_t afmu_peerstore_test afmu_pathsafety_test afmu_qrcode_test)
        target_include_directories(${_t} PRIVATE src)
        if(MSVC)
            target_compile_options(${_t} PRIVATE /utf-8)
        else()
            target_compile_options(${_t} PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
        endif()
        target_compile_definitions(${_t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME ${_t} COMMAND ${_t})
    endforeach()
endif()
