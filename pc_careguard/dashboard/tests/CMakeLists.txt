# 화면 없이 도는 단위 시험(QtTest). 기본으로는 빌드하지 않는다 — 켜려면:
#   cmake -S . -B build -DCAREGUARD_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure
# 시험은 위젯·서버 없이 계산 규칙(모델·판단 규칙·오류 메시지·오디오 시작 실패)만 확인한다.
find_package(Qt6 REQUIRED COMPONENTS Test)

function(careguard_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::WebSockets PkgConfig::OPUS)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

careguard_add_test(tst_p1_overview_model
    tst_p1_overview_model.cpp
    ${CMAKE_SOURCE_DIR}/src/pages/p1/p1_overview_model.cpp)

careguard_add_test(tst_p7_logic
    tst_p7_logic.cpp
    ${CMAKE_SOURCE_DIR}/src/pages/p7/p7_account_rules.cpp
    ${CMAKE_SOURCE_DIR}/src/pages/p7/p7_api.cpp)

careguard_add_test(tst_call_audio_client
    tst_call_audio_client.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/call_audio_client.cpp)

careguard_add_test(tst_server_config
    tst_server_config.cpp
    ${CMAKE_SOURCE_DIR}/src/net/server_config.cpp)
target_compile_definitions(tst_server_config PRIVATE CAREGUARD_DASHBOARD_DIR="/nonexistent")
