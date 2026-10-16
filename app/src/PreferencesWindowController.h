#import <Cocoa/Cocoa.h>

// 環境設定のウインドウ（⌘,）。今は「Lightroom に送る」の送る先のアプリと保存先を選ぶ。
@interface PreferencesWindowController : NSWindowController

+ (instancetype)shared;

@end
