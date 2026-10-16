# AppKit アプリ（Objective-C++、ARC）。
#
# エンジン（hdrcore）を直接呼ぶ。SwiftUI は 10.15 以降でしか使えないため、UI は AppKit をコードで組む
# （xib は使わない。10.12 から最新まで同じコードで動かすため）。
enable_language(OBJCXX)

set(RBH_THIRD_PARTY_NOTICES
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/LICENSE.LGPL"
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/COPYRIGHT"
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/README.RawHDRComposer.md"
)
set_source_files_properties(${RBH_THIRD_PARTY_NOTICES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/ThirdParty/LibRaw")

# アプリのアイコン（app/resources/AppIcon.icns。scripts/make_icon.sh で icon_1024.png から作る）。
set(RBH_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.icns")
set_source_files_properties(${RBH_APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(RawHDRComposerApp MACOSX_BUNDLE
    src/main.mm
    src/AppDelegate.mm
    src/MainWindowController.mm
    src/PreviewView.mm
    src/DngConverterPrompt.mm
    src/PreferencesWindowController.mm
    src/SendTarget.mm
    ${RBH_THIRD_PARTY_NOTICES}
    ${RBH_APP_ICON}
)
target_include_directories(RawHDRComposerApp PRIVATE src)
target_link_libraries(RawHDRComposerApp PRIVATE hdrcore rbh_flags "-framework Cocoa")
target_compile_options(RawHDRComposerApp PRIVATE -fobjc-arc)
set_target_properties(RawHDRComposerApp PROPERTIES
    # バンドル名と実行ファイル名を揃える（食い違うと open で起動したときに黙って終了する）。
    OUTPUT_NAME "RawHDR Composer"
    OBJCXX_STANDARD 17
    OBJCXX_STANDARD_REQUIRED ON
    MACOSX_BUNDLE_BUNDLE_NAME "RawHDR Composer"
    MACOSX_BUNDLE_ICON_FILE "AppIcon"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.geology-cat.rawhdrcomposer"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
)
