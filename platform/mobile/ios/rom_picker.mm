#import <UIKit/UIKit.h>
#include "rom_picker.h"

@interface DaytonaRomPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, assign) SDL_DialogFileCallback callback;
@property(nonatomic, assign) void *userdata;
@end

// UIDocumentPickerViewController holds its delegate weakly.
static DaytonaRomPicker *activePicker;
@implementation DaytonaRomPicker
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    const char *files[] = {nullptr};
    self.callback(self.userdata, files, -1);
    activePicker = nil;
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *source = urls.firstObject;
    BOOL scoped = [source startAccessingSecurityScopedResource];
    char *pref = SDL_GetPrefPath("daytona-recomp", "daytona93");
    NSError *error = nil;
    NSString *path = pref ? [[NSString stringWithUTF8String:pref]
        stringByAppendingPathComponent:[NSString stringWithFormat:@"rom-%@.%@", NSUUID.UUID.UUIDString, source.pathExtension]] : nil;
    SDL_free(pref);
    // Keep a private copy: provider URLs/grants are not durable across launches.
    BOOL copied = path && [[NSFileManager defaultManager] copyItemAtURL:source
        toURL:[NSURL fileURLWithPath:path] error:&error];
    if (scoped) [source stopAccessingSecurityScopedResource];
    if (copied) {
        const char *files[] = {path.UTF8String, nullptr};
        self.callback(self.userdata, files, 0);
    } else {
        SDL_SetError("Cannot import ROM: %s", error ? error.localizedDescription.UTF8String : "no app storage");
        self.callback(self.userdata, nullptr, -1);
    }
    activePicker = nil;
}
@end

void ios_browse_rom(SDL_Window *window, SDL_DialogFileCallback callback, void *userdata) {
    UIWindow *native = (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController *presenter = native.rootViewController;
    while (presenter.presentedViewController) presenter = presenter.presentedViewController;
    if (!presenter || activePicker) {
        SDL_SetError("ROM picker is not available right now");
        callback(userdata, nullptr, -1);
        return;
    }
    activePicker = [DaytonaRomPicker new];
    activePicker.callback = callback;
    activePicker.userdata = userdata;
    // Import mode asks Files/iCloud to download and copy the chosen document.
    // public.data also admits .7z providers that do not declare an archive UTI.
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initWithDocumentTypes:@[@"public.data"] inMode:UIDocumentPickerModeImport];
    picker.delegate = activePicker;
    picker.allowsMultipleSelection = NO;
    [presenter presentViewController:picker animated:YES completion:nil];
}
