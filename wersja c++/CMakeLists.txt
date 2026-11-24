cmake_minimum_required(VERSION 3.20)

project(RotateDisplayCpp VERSION 0.1.0 LANGUAGES CXX RC)

add_executable(RotateDisplay WIN32
    main.cpp
    RotateDisplay.rc
    resource.h
)

target_compile_features(RotateDisplay PRIVATE cxx_std_20)
target_compile_definitions(RotateDisplay PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(RotateDisplay PRIVATE
    user32
    shell32
    gdi32
)

if(MSVC)
    set_property(TARGET RotateDisplay PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(RotateDisplay PRIVATE
        /W4
        /permissive-
        /utf-8
        $<$<CONFIG:Release>:/GL>
    )
    target_link_options(RotateDisplay PRIVATE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()
