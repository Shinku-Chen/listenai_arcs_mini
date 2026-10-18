listenai_library_named(app-intercom)

listenai_library_sources(
    ic_proto.c
    ic_link.c
    ic_ui.c
    ic_app.c
)

# 界面用到 lisa_ui 的导航管理器与中文字体；lisa_ui 的 PUBLIC 头路径只传给链接它的 target，
# 所以这里显式补上（上游 master 的 apps-ui 布局与我们的分支不同）。
listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_TOP_DIR}/apps-ui
    ${PROJECT_TOP_DIR}/apps-ui/lv-i18n
)
