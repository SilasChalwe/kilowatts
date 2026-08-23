set(KILOWATTS_ROLE_SOURCES)

if(KILOWATTS_ROLE STREQUAL "smart")
    list(APPEND KILOWATTS_ROLE_SOURCES
        "smart/main.cpp"
        "smart/SmartApplication.cpp"
    )
else()
    list(APPEND KILOWATTS_ROLE_SOURCES
        "central/main.cpp"
        "central/CentralApplication.cpp"
    )
endif()

idf_component_register(
    SRCS ${KILOWATTS_ROLE_SOURCES}
    INCLUDE_DIRS
        "../include"
)
