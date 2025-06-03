cmake_minimum_required(VERSION 3.21)
project(AR_Application LANGUAGES C CXX)

# 标准设置
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置库安装路径

find_package(OpenGL REQUIRED)

# 查找Qt库
find_package(Qt6 COMPONENTS Widgets Network Sql Multimedia Core Gui WebSockets  SerialPort  REQUIRED)

# 尝试查找SSL模块，但使其可选
find_package(Qt6 COMPONENTS Ssl QUIET)

# 检查是否找到SSL模块
if(Qt6Ssl_FOUND)
    message(STATUS "找到SSL库，将启用HTTPS支持")
    add_definitions(-DHAS_SSL=1)
    
    # 添加Qt SSL模块
    set(EXTRA_LIBS Qt6::Ssl)
else()
    message(WARNING "未找到Qt6 SSL库，应用将以HTTP模式运行（不支持HTTPS）")
    # 确保没有启用SSL相关定义
    add_definitions(-DHAS_SSL=0)
    set(EXTRA_LIBS "")
endif()

# 查找Tesseract和Leptonica
find_package(PkgConfig REQUIRED)


# 查找 GLEW 库（OpenGL 扩展管理）
find_package(GLEW REQUIRED)
# 添加库搜索路径
include_directories(/home/gdh/Qt/6.8.2/gcc_64/include)
find_package(OpenCV REQUIRED)
# 查找Qt依赖
find_package(Qt6 REQUIRED COMPONENTS 
    Core 
    Gui 
    Widgets 
    Network 
    WebSockets 
    Multimedia
    MultimediaWidgets
    Concurrent
    Sql 
    HttpServer
    Pdf
    Positioning 
    OpenGL 
    OpenGLWidgets
    
)




# 查找其他依赖
find_package(Threads REQUIRED)


# 添加可执行文件
add_executable(AR_Application
    main.cpp
    MainWindow.cpp
    MainWindow.h
    mainwindow.ui
    resources.qrc
    Translate.h
    Translate.cpp
    Databaseworker.cpp
    Databaseworker.h
    Httpserver.cpp
    Httpserver.h
    Requesthandler.cpp
    Requesthandler.h
    PDFViewerPage.cpp
    PDFViewerPage.h
    SpeedCalculationPage.h
    SpeedCalculationPage.cpp
    NavigationDisplayWidget.h
    NavigationDisplayWidget.cpp
    GestureProcessor.cpp
    GestureProcessor.h
    CameraManager.cpp
    CameraManager.h
    ThreadPool.h
    ThreadPool.cpp
    VisionPage.h
    VisionPage.cpp
    CameraResourceManager.h
    CameraResourceManager.cpp
)

# 包含目录设置
target_include_directories(AR_Application PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENGL_INCLUDE_DIR}
    ${GLEW_INCLUDE_DIRS}
    ${Qt6_INCLUDE_DIRS}
    ${TESSERACT_INCLUDE_DIRS}
    ${LEPTONICA_INCLUDE_DIRS}
)

# 设置 RPATH 策略
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# 链接库设置
target_link_libraries(AR_Application PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::WebSockets
    Qt6::Sql
    Qt6::HttpServer  # 链接 HttpServer 模块
    Qt6::Pdf
    Qt6::MultimediaWidgets
    Qt6::Positioning
    Qt6::OpenGLWidgets
    Qt6::SerialPort
    Threads::Threads
    dl
    ${EXTRA_LIBS}  # 如果Qt6Ssl_FOUND为真，这里将包含Qt6::Ssl
    ${OpenCV_LIBS}
    ${OPENGL_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${TESSERACT_LIBRARIES}
    ${LEPTONICA_LIBRARIES}
)



# 启用Qt自动处理
set_target_properties(AR_Application PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# 添加C++文件系统库支持
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++17" HAS_STD_CXX17)
if (HAS_STD_CXX17)
    target_compile_options(AR_Application PRIVATE -std=c++17)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 9)
    target_link_libraries(AR_Application PRIVATE stdc++fs)
endif()

set_target_properties(AR_Application PROPERTIES
    INSTALL_RPATH "$ENV{LD_LIBRARY_PATH}:/usr/lib/x86_64-linux-gnu/"
    BUILD_WITH_INSTALL_RPATH TRUE
)