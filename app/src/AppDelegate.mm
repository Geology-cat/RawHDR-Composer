#import "AppDelegate.h"

#import "MainWindowController.h"
#import "PreferencesWindowController.h"

@implementation AppDelegate {
    MainWindowController* _controller;
    // ウインドウができる前に Finder から渡されたファイル（ドックのアイコンへのドロップなど）。
    NSMutableArray<NSURL*>* _pending;
}

- (instancetype)init {
    self = [super init];
    if (self) _pending = [NSMutableArray array];
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [self buildMenu];
    _controller = [[MainWindowController alloc] init];
    [_controller showWindow:nil];
    [[_controller window] makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (_pending.count) {
        [_controller addFiles:_pending];
        [_pending removeAllObjects];
    }
    // 検証用: RBH_PREFS_SNAPSHOT に環境設定のウインドウの中身を PNG で書いて終了する（説明書の図）。
    if (const char* prefsSnap = getenv("RBH_PREFS_SNAPSHOT")) {
        NSString* path = [NSString stringWithUTF8String:prefsSnap];
        PreferencesWindowController* prefs = [PreferencesWindowController shared];
        [prefs showWindow:nil];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            NSView* v = [[prefs window] contentView];
            NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:[v bounds]];
            NSGraphicsContext* ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:ctx];
            [[NSColor windowBackgroundColor] setFill];
            NSRectFill([v bounds]);
            [NSGraphicsContext restoreGraphicsState];
            [v displayRectIgnoringOpacity:[v bounds] inContext:ctx];
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
            [NSApp terminate:nil];
        });
        return;
    }
    // 検証用: RBH_OPEN（改行区切りのパス）を開き、RBH_SNAPSHOT にウインドウの中身を PNG で書いて終了する。
    [_controller runAutomationFromEnvironment];
    // Adobe DNG Converter が無ければ入手を案内する（無くても動くことを明記したうえで。検証用の自動操作のときは出さない）。
    if (!getenv("RBH_OPEN") || getenv("RBH_SHOW_DNG_PROMPT")) [_controller showDngConverterPromptAtLaunchIfNeeded];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (void)application:(NSApplication*)sender openURLs:(NSArray<NSURL*>*)urls {
    (void)sender;
    if (_controller) {
        [_controller addFiles:urls];
    } else {
        [_pending addObjectsFromArray:urls];
    }
}

- (void)showPreferences:(id)sender {
    [[PreferencesWindowController shared] showWindow:sender];
}

- (void)buildMenu {
    NSMenu* bar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"RawHDR Composer"];
    [appMenu addItemWithTitle:@"RawHDR Composer について" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"環境設定…" action:@selector(showPreferences:) keyEquivalent:@","];
    [appMenu addItemWithTitle:@"Adobe DNG Converter…" action:@selector(showDngConverterPrompt:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"RawHDR Composer を隠す" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* hideOthers = [appMenu addItemWithTitle:@"ほかを隠す" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
    [appMenu addItemWithTitle:@"すべてを表示" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"RawHDR Composer を終了" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem* fileItem = [[NSMenuItem alloc] init];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"ファイル"];
    [fileMenu addItemWithTitle:@"RAW を開く…" action:@selector(openDocument:) keyEquivalent:@"o"];
    [fileMenu addItemWithTitle:@"HDR 結合" action:@selector(startMerge:) keyEquivalent:@"r"];
    [fileMenu addItemWithTitle:@"DNG を書き出す…" action:@selector(exportDNG:) keyEquivalent:@"e"];
    // 題名は送る先のアプリの名前に合わせて変わる（メインウインドウの validateMenuItem）。
    NSMenuItem* send = [fileMenu addItemWithTitle:@"Lightroom Classic に送る" action:@selector(sendToApp:) keyEquivalent:@"e"];
    [send setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItemWithTitle:@"すべて取り除く" action:@selector(clearFrames:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"閉じる" action:@selector(performClose:) keyEquivalent:@"w"];
    [fileItem setSubmenu:fileMenu];
    [bar addItem:fileItem];

    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"編集"];
    [editMenu addItemWithTitle:@"取り消す" action:@selector(undo:) keyEquivalent:@"z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"カット" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"コピー" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"ペースト" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"すべてを選択" action:@selector(selectAll:) keyEquivalent:@"a"];
    [editItem setSubmenu:editMenu];
    [bar addItem:editItem];

    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"表示"];
    [viewMenu addItemWithTitle:@"全体を表示" action:@selector(zoomToFit:) keyEquivalent:@"0"];
    [viewMenu addItemWithTitle:@"実寸（100%）" action:@selector(zoomToActual:) keyEquivalent:@"1"];
    [viewMenu addItemWithTitle:@"拡大" action:@selector(zoomIn:) keyEquivalent:@"+"];
    [viewMenu addItemWithTitle:@"縮小" action:@selector(zoomOut:) keyEquivalent:@"-"];
    [viewItem setSubmenu:viewMenu];
    [bar addItem:viewItem];

    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"ウインドウ"];
    [windowMenu addItemWithTitle:@"しまう" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"拡大／縮小" action:@selector(performZoom:) keyEquivalent:@""];
    [windowItem setSubmenu:windowMenu];
    [bar addItem:windowItem];
    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:bar];
}

@end
