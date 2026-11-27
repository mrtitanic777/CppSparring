cmake_minimum_required(VERSION 3.20)
project(CppSparring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LLVM_DIR "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64")

add_executable(cppsparring WIN32
    src/main.cpp
    src/window.cpp
    src/window.hpp
    src/render.cpp
    src/render.hpp
    src/layout.hpp
    src/layout.cpp
    src/palette.hpp
    src/ast.cpp
    src/ast.hpp
    src/mutate.cpp
    src/mutate.hpp
    src/compile.cpp
    src/compile.hpp
    src/examples.cpp
    src/examples.hpp
    src/explain.cpp
    src/explain.hpp
    src/fix.cpp
    src/fix.hpp
    src/progress.cpp
    src/progress.hpp
    src/replay.cpp
    src/replay.hpp
    src/json.hpp
    src/lessons.cpp
    src/lessons.hpp
    src/quiz.cpp
    src/quiz.hpp
    src/quest.cpp
    src/quest.hpp
    src/buddy.cpp
    src/buddy.hpp
    src/speech.cpp
    src/speech.hpp
    src/teacher.cpp
    src/teacher.hpp
    src/sound.cpp
    src/sound.hpp
    src/app.rc
)

# the lessons (from CodingTeacher's LearnC) sit next to the program, so they can be edited without rebuilding
add_custom_command(TARGET cppsparring POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lessons $<TARGET_FILE_DIR:cppsparring>/lessons
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/tools/voice.py $<TARGET_FILE_DIR:cppsparring>/voice.py)

target_include_directories(cppsparring PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(cppsparring PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(cppsparring PRIVATE /utf-8)

target_link_libraries(cppsparring PRIVATE
    d2d1
    dwrite
    dwmapi
    dxgi
    dxguid
    windowscodecs
    ole32
    shell32
    shlwapi
    comctl32
    winmm
    sapi
)
