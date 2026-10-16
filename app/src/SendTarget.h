#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

// 「Lightroom に送る」の送る先のアプリと保存先（環境設定。NSUserDefaults に保存する）。
//
// アプリを選んでいなければ Lightroom Classic を探す。保存先を選んでいなければ基準フレームと同じフォルダ。
// Lightroom Classic は送ったファイルをその場所のまま読み込む（参照する）ので、一時フォルダには書かない。
@interface SendTarget : NSObject

// 送る先のアプリ。選んだアプリが無くなっていれば、Lightroom Classic を探す。見つからなければ nil。
+ (nullable NSURL*)applicationURL;
// 表示名（「Adobe Lightroom Classic」など）。アプリが無ければ nil。
+ (nullable NSString*)applicationName;
// nil にすると選んだアプリを忘れ、Lightroom Classic を探す既定に戻す。
+ (void)setApplicationURL:(nullable NSURL*)url;
// 既定で探す Lightroom Classic（無ければ nil）。
+ (nullable NSURL*)lightroomClassicURL;

// 保存先のフォルダ。nil = 基準フレームと同じフォルダ。
+ (nullable NSString*)folderPath;
+ (void)setFolderPath:(nullable NSString*)path;

// folder に name で保存するときのパス。同じ名前のファイルがあれば「名前-2.dng」のように番号を付ける（上書きしない）。
+ (NSString*)uniquePathInFolder:(NSString*)folder name:(NSString*)name;

// file を送る先のアプリで開く。終わったら（失敗したらエラーを付けて）メインスレッドで done を呼ぶ。
+ (void)openFile:(NSString*)path withApplication:(NSURL*)app done:(void (^)(NSError* _Nullable error))done;

@end

// 送る先・保存先が変わったときに送る通知。
extern NSNotificationName const RBHSendTargetDidChangeNotification;

NS_ASSUME_NONNULL_END
