file(GLOB_RECURSE MY_SRCS
    "./hal/*.c"
    "./hal/*.cc"
    "./hal/*.cpp"
    "./apps/app_roast/*.cpp"
    "./apps/app_roast/*.c"
    "./apps/utils/*.c"
    "./apps/utils/*.cpp"
)

set(MY_INCLUDE_DIRS
    "."
    "./assets"
    "./hal"
    "./apps"
)

idf_component_register(SRCS "main.cpp" ${MY_SRCS}
                    INCLUDE_DIRS ${MY_INCLUDE_DIRS}
)
