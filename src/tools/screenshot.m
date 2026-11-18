// screenshot: save the iPod's current screen as PNG (iOS 6, private UIGetScreenImage). Run as root.
//   screenshot /tmp/screen.png
#import <UIKit/UIKit.h>
extern CGImageRef UIGetScreenImage(void);
int main(int argc, char **argv) {
    @autoreleasepool {
        CGImageRef img = UIGetScreenImage();
        if (!img) { fprintf(stderr, "UIGetScreenImage failed\n"); return 1; }
        NSData *png = UIImagePNGRepresentation([UIImage imageWithCGImage:img]);
        CGImageRelease(img);
        return [png writeToFile:[NSString stringWithUTF8String:argc > 1 ? argv[1] : "/tmp/screen.png"] atomically:YES] ? 0 : 1;
    }
}
