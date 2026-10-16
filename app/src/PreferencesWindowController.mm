#import "PreferencesWindowController.h"

#import "SendTarget.h"

@implementation PreferencesWindowController {
    NSPopUpButton* _appPopup;
    NSPopUpButton* _folderPopup;
}

+ (instancetype)shared {
    static PreferencesWindowController* shared = nil;
    if (!shared) shared = [[PreferencesWindowController alloc] init];
    return shared;
}

- (instancetype)init {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 240)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [w setTitle:@"環境設定"];
    self = [super initWithWindow:w];
    if (self) {
        [self build];
        [self refresh];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(targetChanged:)
                                                     name:RBHSendTargetDidChangeNotification
                                                   object:nil];
    }
    return self;
}

- (NSTextField*)note:(NSString*)text {
    NSTextField* t = [NSTextField wrappingLabelWithString:text];
    [t setFont:[NSFont systemFontOfSize:11]];
    [t setTextColor:[NSColor secondaryLabelColor]];
    return t;
}

- (void)build {
    NSView* root = [[self window] contentView];
    NSTextField* heading = [NSTextField labelWithString:@"「送る」ボタン（合成した DNG を書き出して、ほかのアプリで開く）"];
    [heading setFont:[NSFont boldSystemFontOfSize:13]];

    _appPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_appPopup setTarget:self];
    [_appPopup setAction:@selector(appChosen:)];
    _folderPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_folderPopup setTarget:self];
    [_folderPopup setAction:@selector(folderChosen:)];

    NSTextField* appLabel = [NSTextField labelWithString:@"送る先のアプリケーション:"];
    NSTextField* folderLabel = [NSTextField labelWithString:@"保存先:"];
    NSGridView* grid = [NSGridView gridViewWithViews:@[ @[ appLabel, _appPopup ], @[ folderLabel, _folderPopup ] ]];
    [grid setRowSpacing:10];
    [grid setColumnSpacing:8];
    [[grid columnAtIndex:0] setXPlacement:NSGridCellPlacementTrailing];
    [grid setRowAlignment:NSGridRowAlignmentFirstBaseline];
    [[_appPopup widthAnchor] constraintGreaterThanOrEqualToConstant:280].active = YES;
    [[_folderPopup widthAnchor] constraintEqualToAnchor:[_appPopup widthAnchor]].active = YES;

    NSTextField* n1 = [self note:@"既定は Lightroom Classic です。Lightroom Classic では読み込みの画面が開くので、「追加」を選んで読み込んでください"
                                 @"（ファイルは保存した場所のまま、カタログに登録されます）。"];
    NSTextField* n2 = [self note:@"同じ名前のファイルがあるときは、名前に番号を付けて保存します（上書きしません）。"
                                 @"書き出しの形式・レンズ補正は、メインウインドウの「書き出し」の設定に従います。"];

    NSStackView* all = [NSStackView stackViewWithViews:@[ heading, grid, n1, n2 ]];
    [all setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [all setAlignment:NSLayoutAttributeLeading];
    [all setSpacing:12];
    [all setEdgeInsets:NSEdgeInsetsMake(20, 20, 20, 20)];
    [all setTranslatesAutoresizingMaskIntoConstraints:NO];
    [root addSubview:all];
    [NSLayoutConstraint activateConstraints:@[
        [[all leadingAnchor] constraintEqualToAnchor:[root leadingAnchor]],
        [[all trailingAnchor] constraintEqualToAnchor:[root trailingAnchor]],
        [[all topAnchor] constraintEqualToAnchor:[root topAnchor]],
        [[all bottomAnchor] constraintEqualToAnchor:[root bottomAnchor]],
        [[root widthAnchor] constraintEqualToConstant:540],
        [[n1 widthAnchor] constraintEqualToAnchor:[all widthAnchor] constant:-40],
        [[n2 widthAnchor] constraintEqualToAnchor:[all widthAnchor] constant:-40],
    ]];
    // 高さを中身に合わせる（余った高さが段落の間に入らないように）。
    [[self window] setContentSize:NSMakeSize(540, [all fittingSize].height)];
}

- (void)showWindow:(id)sender {
    [self refresh];
    if (![[self window] isVisible]) [[self window] center];
    [super showWindow:sender];
}

- (void)targetChanged:(NSNotification*)n {
    (void)n;
    [self refresh];
}

// メニューの項目にアイコンを付ける（小さく）。
- (NSMenuItem*)itemWithTitle:(NSString*)title path:(NSString*)path tag:(NSInteger)tag {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    [item setTag:tag];
    if (path) {
        NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFile:path];
        [icon setSize:NSMakeSize(16, 16)];
        [item setImage:icon];
        [item setToolTip:path];
    }
    return item;
}

enum { kTagCurrent = 1, kTagLightroom = 2, kTagChoose = 3, kTagSameFolder = 10, kTagFolder = 11, kTagChooseFolder = 12 };

- (void)refresh {
    // アプリ: 今の送る先、（別のものを選んでいれば）Lightroom Classic、「選択…」。
    NSMenu* m = [[NSMenu alloc] init];
    NSURL* app = [SendTarget applicationURL];
    NSURL* lr = [SendTarget lightroomClassicURL];
    if (app) {
        [m addItem:[self itemWithTitle:[SendTarget applicationName] path:[app path] tag:kTagCurrent]];
    } else {
        NSMenuItem* none = [self itemWithTitle:@"（Lightroom Classic が見つかりません）" path:nil tag:0];
        [m addItem:none];
    }
    if (lr && ![[[lr path] stringByStandardizingPath] isEqualToString:[[app path] stringByStandardizingPath]]) {
        NSString* name = [[[NSFileManager defaultManager] displayNameAtPath:[lr path]] stringByDeletingPathExtension];
        [m addItem:[self itemWithTitle:[name stringByAppendingString:@"（既定）"] path:[lr path] tag:kTagLightroom]];
    }
    [m addItem:[NSMenuItem separatorItem]];
    [m addItem:[self itemWithTitle:@"ほかのアプリケーションを選ぶ…" path:nil tag:kTagChoose]];
    [_appPopup setMenu:m];
    [_appPopup selectItemAtIndex:0];

    // 保存先: 基準フレームと同じフォルダ、（選んでいれば）そのフォルダ、「選択…」。
    NSMenu* f = [[NSMenu alloc] init];
    [f addItem:[self itemWithTitle:@"基準フレームと同じフォルダ" path:nil tag:kTagSameFolder]];
    NSString* folder = [SendTarget folderPath];
    if (folder) [f addItem:[self itemWithTitle:[[NSFileManager defaultManager] displayNameAtPath:folder] path:folder tag:kTagFolder]];
    [f addItem:[NSMenuItem separatorItem]];
    [f addItem:[self itemWithTitle:@"フォルダを選ぶ…" path:nil tag:kTagChooseFolder]];
    [_folderPopup setMenu:f];
    [_folderPopup selectItemWithTag:folder ? kTagFolder : kTagSameFolder];
}

- (void)appChosen:(id)sender {
    (void)sender;
    const NSInteger tag = [[_appPopup selectedItem] tag];
    if (tag == kTagLightroom) {
        [SendTarget setApplicationURL:nil];  // 既定（Lightroom Classic を探す）に戻す
    } else if (tag == kTagChoose) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [panel setAllowedFileTypes:@[ @"app" ]];  // allowedContentTypes は macOS 11 以降
#pragma clang diagnostic pop
        [panel setDirectoryURL:[NSURL fileURLWithPath:@"/Applications"]];
        [panel setMessage:@"合成した DNG を開くアプリケーションを選んでください"];
        [panel beginSheetModalForWindow:[self window]
                      completionHandler:^(NSModalResponse r) {
                          if (r == NSModalResponseOK) [SendTarget setApplicationURL:[panel URL]];
                          [self refresh];
                      }];
        return;
    }
    [self refresh];
}

- (void)folderChosen:(id)sender {
    (void)sender;
    const NSInteger tag = [[_folderPopup selectedItem] tag];
    if (tag == kTagSameFolder) {
        [SendTarget setFolderPath:nil];
    } else if (tag == kTagChooseFolder) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setCanCreateDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setMessage:@"送るときに DNG を保存するフォルダを選んでください"];
        [panel beginSheetModalForWindow:[self window]
                      completionHandler:^(NSModalResponse r) {
                          if (r == NSModalResponseOK) [SendTarget setFolderPath:[[panel URL] path]];
                          [self refresh];
                      }];
        return;
    }
    [self refresh];
}

@end
