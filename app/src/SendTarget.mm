#import "SendTarget.h"

NSNotificationName const RBHSendTargetDidChangeNotification = @"RBHSendTargetDidChange";

namespace {
NSString* const kAppKey = @"SendApplicationPath";
NSString* const kFolderKey = @"SendFolderPath";
}  // namespace

@implementation SendTarget

+ (NSURL*)lightroomClassicURL {
    // 新しい版から順に（Lightroom Classic、それより前の Lightroom 6・5）。
    for (NSString* bid in @[ @"com.adobe.LightroomClassicCC7", @"com.adobe.Lightroom6", @"com.adobe.Lightroom5" ]) {
        NSURL* url = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:bid];
        if (url) return url;
    }
    return nil;
}

+ (NSURL*)applicationURL {
    NSString* path = [[NSUserDefaults standardUserDefaults] stringForKey:kAppKey];
    if (path.length && [[NSFileManager defaultManager] fileExistsAtPath:path]) return [NSURL fileURLWithPath:path];
    return [self lightroomClassicURL];
}

+ (NSString*)applicationName {
    NSURL* url = [self applicationURL];
    if (!url) return nil;
    NSString* name = [[NSFileManager defaultManager] displayNameAtPath:[url path]];
    if ([[name pathExtension] isEqualToString:@"app"]) name = [name stringByDeletingPathExtension];
    return name;
}

+ (void)setApplicationURL:(NSURL*)url {
    NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
    if (url) {
        [d setObject:[url path] forKey:kAppKey];
    } else {
        [d removeObjectForKey:kAppKey];
    }
    [[NSNotificationCenter defaultCenter] postNotificationName:RBHSendTargetDidChangeNotification object:nil];
}

+ (NSString*)folderPath {
    NSString* path = [[NSUserDefaults standardUserDefaults] stringForKey:kFolderKey];
    return path.length ? path : nil;
}

+ (void)setFolderPath:(NSString*)path {
    NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
    if (path.length) {
        [d setObject:path forKey:kFolderKey];
    } else {
        [d removeObjectForKey:kFolderKey];
    }
    [[NSNotificationCenter defaultCenter] postNotificationName:RBHSendTargetDidChangeNotification object:nil];
}

+ (NSString*)uniquePathInFolder:(NSString*)folder name:(NSString*)name {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* path = [folder stringByAppendingPathComponent:name];
    NSString* base = [name stringByDeletingPathExtension];
    NSString* ext = [name pathExtension];
    for (int k = 2; [fm fileExistsAtPath:path]; ++k) {
        path = [folder stringByAppendingPathComponent:[[NSString stringWithFormat:@"%@-%d", base, k] stringByAppendingPathExtension:ext]];
    }
    return path;
}

+ (void)openFile:(NSString*)path withApplication:(NSURL*)app done:(void (^)(NSError*))done {
    NSURL* file = [NSURL fileURLWithPath:path];
    if (@available(macOS 10.15, *)) {
        NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
        [config setActivates:YES];
        [[NSWorkspace sharedWorkspace] openURLs:@[ file ]
                           withApplicationAtURL:app
                                  configuration:config
                              completionHandler:^(NSRunningApplication* running, NSError* error) {
                                  (void)running;
                                  dispatch_async(dispatch_get_main_queue(), ^{
                                      done(error);
                                  });
                              }];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const BOOL ok = [[NSWorkspace sharedWorkspace] openFile:path withApplication:[app path]];
#pragma clang diagnostic pop
        done(ok ? nil : [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadUnknownError userInfo:nil]);
    }
}

@end
