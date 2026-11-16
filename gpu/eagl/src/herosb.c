/*
 * libherosb.dylib, in SpringBoard (boot-arg herosb=1, bsd/dev/herolte_gl.c): PosterBoard has no poster providers on this
 * root, so the wallpaper windows stay black. The picture of the drop zone goes into them, and behind the home screen.
 * DROP_DIR wallpaper.off turns it off.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define DROP_DIR "/private/var/herolte/"
#define VIEW_TAG 0x48455230

typedef struct { double x, y, w, h; } rect_t;

extern void herodrm_log(const char *format, ...);

static id
nsstring(const char *text)
{
   return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
      sel_registerName("stringWithUTF8String:"), text);
}

static id
send0(id target, const char *selector)
{
   return ((id (*)(id, SEL))objc_msgSend)(target, sel_registerName(selector));
}

static id picture;
static bool gestures_on;
static int status_bar_kept, status_bar_asked, status_bar_replaced;
static unsigned rounds;

static void
give_picture(id window, bool behind_all)
{
   if (((id (*)(id, SEL, long))objc_msgSend)(window, sel_registerName("viewWithTag:"), VIEW_TAG))
      return;
   id view = ((id (*)(id, SEL, id))objc_msgSend)(send0((id)objc_getClass("UIImageView"), "alloc"),
      sel_registerName("initWithImage:"), picture);
   rect_t bounds = ((rect_t (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds"));
   ((void (*)(id, SEL, rect_t))objc_msgSend)(view, sel_registerName("setFrame:"), bounds);
   ((void (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("setContentMode:"), 2);          /* aspect fill */
   ((void (*)(id, SEL, unsigned long))objc_msgSend)(view, sel_registerName("setAutoresizingMask:"), 18);
   ((void (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("setTag:"), VIEW_TAG);
   ((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setUserInteractionEnabled:"), NO);
   if (behind_all)
      ((void (*)(id, SEL, id, long))objc_msgSend)(window, sel_registerName("insertSubview:atIndex:"), view, 0);
   else
      ((void (*)(id, SEL, id))objc_msgSend)(window, sel_registerName("addSubview:"), view);
   herodrm_log("herosb: picture in %s, %.0f x %.0f", object_getClassName(window), bounds.w, bounds.h);
}

static void
look_at_windows(void *unused)
{
   rounds++;
   Class windows_class = objc_getClass("UIWindow");
   SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
   id windows = windows_class && class_respondsToSelector(object_getClass((id)windows_class), all) ?
      ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)windows_class, all, YES, NO) : nil;
   if (!picture && objc_getClass("UIImage")) {
      picture = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("UIImage"), sel_registerName("imageWithContentsOfFile:"),
         nsstring(DROP_DIR "wallpaper.png"));
      if (picture)
         send0(picture, "retain");
   }
   size_t count = windows ? ((size_t (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count")) : 0;
   if (rounds == 1 || rounds == 6)
      herodrm_log("herosb: round %u, %zu windows, picture %d, gestures %d, status bar hooks %d asked %d replaced %d", rounds, count, picture != nil, gestures_on, status_bar_kept, status_bar_asked, status_bar_replaced);
   for (size_t i = 0; picture && i < count; i++) {
      id window = ((id (*)(id, SEL, size_t))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
      const char *name = object_getClassName(window);
      if (rounds == 6)
         herodrm_log("herosb: window %s", name);
      if (strstr(name, "Wallpaper"))
         give_picture(window, false);
      else if (strcmp(name, "SBHomeScreenWindow") == 0)
         give_picture(window, true);
   }
   if (rounds < 60)
      dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, look_at_windows);
}

/*
 * The ADT says "home button" (home-button-type 1): UIKit then has a status bar for this 750x1334 screen, where type 2
 * gets the red rdar:45025538 one. SpringBoard alone is told type 2, which is where the home gestures are decided, as
 * the old home-gesture tweaks did. DROP_DIR gestures.off turns it off.
 */
static long
says_gestures(id self, SEL cmd)
{
   return 2;
}

static BOOL
says_yes(id self, SEL cmd)
{
   return YES;
}

/*
 * ...but told type 2, UIKit in SpringBoard asks each provider class in turn, none takes a 750x1334 screen and it ends on
 * the red rdar:45025538 fallback. The plain phone's class takes any screen here, and is put in where the fallback
 * still reaches the status bar.
 */
static Class
phone_takes_it(id self, SEL cmd, id screen, id info)
{
   status_bar_asked++;
   return objc_getClass("_UIStatusBarVisualProvider_Phone");
}

static void (*set_provider_was)(id, SEL, Class);

static void
set_provider(id self, SEL cmd, Class provider)
{
   if (provider == objc_getClass("_UIStatusBarVisualProvider_Fallback")) {
      status_bar_replaced++;
      provider = objc_getClass("_UIStatusBarVisualProvider_Phone");
   }
   set_provider_was(self, cmd, provider);
}

static void
keep_status_bar(void)
{
   static const char *const providers[] = { "_UIStatusBarVisualProvider_iOS", "_UIStatusBarVisualProvider_Phone",
      "_UIStatusBarVisualProvider_Fallback" };
   SEL pick = sel_registerName("visualProviderSubclassForScreen:visualProviderInfo:");
   for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
      Class provider = objc_getClass(providers[i]);
      Method method = provider ? class_getClassMethod(provider, pick) : NULL;
      if (method) {
         method_setImplementation(method, (IMP)phone_takes_it);
         status_bar_kept++;
      }
   }
   Class bar = objc_getClass("_UIStatusBar");
   Method setter = bar ? class_getInstanceMethod(bar, sel_registerName("setVisualProviderClass:")) : NULL;
   if (setter) {
      set_provider_was = (void (*)(id, SEL, Class))method_setImplementation(setter, (IMP)set_provider);
      status_bar_kept += 10;
   }
}

static void
ask_for_gestures(void)
{
   keep_status_bar();
   Class platform = objc_getClass("BSPlatform");
   Method type = platform ? class_getInstanceMethod(platform, sel_registerName("homeButtonType")) : NULL;
   if (type) {
      method_setImplementation(type, (IMP)says_gestures);
      gestures_on = true;
   }
   Class settings = objc_getClass("SBHomeGestureSettings");
   Method enabled = settings ? class_getInstanceMethod(settings, sel_registerName("isHomeGestureEnabled")) : NULL;
   if (enabled)
      method_setImplementation(enabled, (IMP)says_yes);
}

/*
 * SpringBoard's own frame rate caps (from Scion's inject_sb.c, Galaxy S9). Some of its animations ask for fewer frames
 * than the panel has: the crossfade of the page dots and the search pill after every page's slide is
 * CAFrameRateRangeMake(20, 30, 0), and CoreAnimation gives SpringBoard 30 a second for as long as it runs -- on the S9
 * half of all the refreshes missed while swiping. A range whose most is under 60 gets 60 as its most and preferred.
 * DROP_DIR fps.off leaves them as Apple set them.
 */
typedef struct { float minimum, maximum, preferred; } frame_rate_range_t;
static void (*fluid_set_range_was)(id, SEL, frame_rate_range_t, unsigned);
static void (*animation_set_range_was)(id, SEL, frame_rate_range_t, unsigned);
static unsigned ranges_raised;

static frame_rate_range_t
raise_range(frame_rate_range_t range)
{
   if (range.maximum > 0.0f && range.maximum < 60.0f) {
      range.maximum = 60.0f;
      range.preferred = 60.0f;
      ranges_raised++;
   }
   return range;
}

static void
fluid_set_range(id self, SEL cmd, frame_rate_range_t range, unsigned reason)
{
   fluid_set_range_was(self, cmd, raise_range(range), reason);
}

static void
animation_set_range(id self, SEL cmd, frame_rate_range_t range, unsigned reason)
{
   animation_set_range_was(self, cmd, raise_range(range), reason);
}

static void
raise_frame_rates(void)
{
   SEL set = sel_registerName("setFrameRateRange:highFrameRateReason:");
   Class fluid = objc_getClass("SBFFluidBehaviorSettings"), animation = objc_getClass("SBFAnimationSettings");
   Method m = fluid && !fluid_set_range_was ? class_getInstanceMethod(fluid, set) : NULL;
   if (m)
      fluid_set_range_was = (void (*)(id, SEL, frame_rate_range_t, unsigned))method_setImplementation(m, (IMP)fluid_set_range);
   m = animation && !animation_set_range_was ? class_getInstanceMethod(animation, set) : NULL;
   if (m)
      animation_set_range_was = (void (*)(id, SEL, frame_rate_range_t, unsigned))method_setImplementation(m, (IMP)animation_set_range);
   herodrm_log("herosb: frame rate caps raised to 60: fluid settings %s, animation settings %s",
      fluid_set_range_was ? "yes" : "NO", animation_set_range_was ? "yes" : "NO");
}

static void
report_ranges(void *unused)
{
   herodrm_log("herosb: %u frame rate ranges raised to 60 so far", ranges_raised);
}

/*
 * Liquid Glass theme. The container classes below are SpringBoard's, not public UIKit, so this root is not guaranteed to
 * have them and a wrong method_setImplementation takes the whole UI down: every class and selector is looked up first and
 * a missing one only makes the theme do less. DROP_DIR glass.off turns the whole block off.
 *
 * No @try: this file is not compiled with -fobjc-exceptions (build-eagl.sh), so a @try would not compile. The guards are
 * property existence (class_respondsToSelector before a setter) and a bounded view walk, never an exception.
 */

/* _UIStatusBarBatteryView: the number, inside the battery icon (the stock layout hides it on a notched phone). */
static BOOL battery_percentage(id self, SEL cmd)
{
   return YES;
}

static BOOL
keep_battery_percentage(void)
{
   Class view = objc_getClass("_UIStatusBarBatteryView");
   SEL show = sel_registerName("showsPercentage");
   Method m = view ? class_getInstanceMethod(view, show) : NULL;
   if (!m)
      return NO;
   method_setImplementation(m, (IMP)battery_percentage);
   return YES;
}

/* Where a view's rounded corner, hairline and glass get applied. Values follow the brief. */
static const double DOCK_RADIUS = 28.0;
static const double CARD_RADIUS = 26.0;
static const double GLASS_RADIUS = 20.0;

static void
round_view(id view, double radius, float border_alpha, double shadow_radius)
{
   if (!view)
      return;
   /*
    * setCornerRadius:, setBorderWidth:, setBorderColor:, setShadowRadius:, setShadowOpacity:, setShadowOffset: and
    * masksToBounds all live on CALayer; setClipsToBounds: is the UIView property that mirrors masksToBounds.
    */
   SEL layer_sel = sel_registerName("layer");
   /* A view whose class does not answer "layer" is not a UIView: skip it instead of sending a message it cannot take. */
   if (!class_respondsToSelector(object_getClass(view), layer_sel))
      return;
   id layer = ((id (*)(id, SEL))objc_msgSend)(view, layer_sel);
   if (layer) {
      ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setCornerRadius:"), radius);
      /*
       * A layer with masksToBounds also clips its own shadow, so the shadow is only set when there is none. The hairline
       * does not need the mask: a rounded border follows the radius on its own.
       */
      if (shadow_radius > 0.0) {
         ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setShadowRadius:"), shadow_radius);
         ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setShadowOpacity:"), 0.18);
         /* setShadowOffset: takes a CGSize by value: the cast must name the SDK's typedef, not "struct CGSize". */
         ((void (*)(id, SEL, CGSize))objc_msgSend)(layer, sel_registerName("setShadowOffset:"),
            (CGSize){ 0.0, 2.0 });
      }
      if (border_alpha > 0.0f) {
         id white = ((id (*)(id, SEL, double))objc_msgSend)((id)objc_getClass("UIColor"),
            sel_registerName("colorWithWhite:alpha:"), border_alpha);
         ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setBorderWidth:"), 0.75);
         if (white)
            ((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setBorderColor:"),
               ((id (*)(id, SEL))objc_msgSend)(white, sel_registerName("CGColor")));
      }
   }
   SEL clips = sel_registerName("setClipsToBounds:");
   if (class_respondsToSelector(object_getClass(view), clips))
      ((void (*)(id, SEL, BOOL))objc_msgSend)(view, clips, radius > 0.0);
}

/*
 * Applying the look twice is cheap (same values), skipping a view that was freed and whose address a new view now uses is
 * not: the first version keyed on the plain address and a recycled SBDockView would never be painted again. Each slot
 * also keeps the class name, so a recycled address with a different class is a first sight again.
 */
#define SEEN_MAX 256
static const void *seen_views[SEEN_MAX];
static const char *seen_names[SEEN_MAX];
static size_t seen_count;

static bool
first_sight(id view, const char *name)
{
   for (size_t i = 0; i < seen_count; i++)
      if (seen_views[i] == (const void *)view && strcmp(seen_names[i], name) == 0)
         return false;
   if (seen_count == SEEN_MAX) {           /* forget the oldest half: switcher cards are short-lived */
      memmove(seen_views, seen_views + SEEN_MAX / 2, sizeof(seen_views[0]) * (SEEN_MAX / 2));
      memmove(seen_names, seen_names + SEEN_MAX / 2, sizeof(seen_names[0]) * (SEEN_MAX / 2));
      seen_count = SEEN_MAX / 2;
   }
   seen_views[seen_count] = (const void *)view;
   seen_names[seen_count] = name;          /* class names are interned by the runtime: the pointer stays valid */
   seen_count++;
   return true;
}

static unsigned glass_docks, glass_cards, glass_glass;
static bool glass_on;

static void
paint_glass(void *unused)
{
   if (!glass_on)
      return;
   Class windows_class = objc_getClass("UIWindow");
   SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
   id windows = windows_class && class_respondsToSelector(object_getClass((id)windows_class), all) ?
      ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)windows_class, all, YES, NO) : nil;
   size_t count = windows ? ((size_t (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count")) : 0;
   for (size_t i = 0; i < count; i++) {
      id window = ((id (*)(id, SEL, size_t))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
      id root = ((id (*)(id, SEL))objc_msgSend)(window, sel_registerName("rootViewController"));
      id view = root ? ((id (*)(id, SEL))objc_msgSend)(root, sel_registerName("view")) : nil;
      if (!view)
         continue;
      /*
       * A depth-limited walk of the live view tree, with an explicit position per depth (a plain stack would either drop
       * siblings when it filled up or read past its end). SpringBoard's tree is wider than it is deep and this goes 24
       * levels down, which is more than the dock, the switcher cards and the banners need.
       */
      enum { WALK_LEVELS = 24, WALK_WIDTH = 64 };
      id level_view[WALK_LEVELS];
      id level_subs[WALK_LEVELS];
      size_t level_pos[WALK_LEVELS];
      size_t level_count[WALK_LEVELS];
      size_t depth = 0;
      level_view[0] = view;
      level_subs[0] = nil;
      level_pos[0] = 0;
      level_count[0] = 0;
      while (depth < WALK_LEVELS) {
         id current = level_view[depth];
         if (level_pos[depth] == 0) {         /* first arrival at this depth: paint it, then walk its children */
            const char *name = object_getClassName(current);
            if (name && first_sight(current, name)) {
               if (strcmp(name, "SBDockView") == 0) {
                  round_view(current, DOCK_RADIUS, 0.28f, 3.0);
                  glass_docks++;
               } else if (strstr(name, "AppSwitcherPageView") || strstr(name, "FluidSwitcherItemContainer")) {
                  round_view(current, CARD_RADIUS, 0.28f, 2.0);
                  glass_cards++;
               } else if (strstr(name, "CoverSheetView") || strstr(name, "NotificationBannerView") ||
                          strstr(name, "WidgetContainerView")) {
                  round_view(current, GLASS_RADIUS, 0.28f, 2.5);
                  glass_glass++;
               }
            }
            /* The array is kept per level: asking again on every step would send one message per child. */
            id subs = ((id (*)(id, SEL))objc_msgSend)(current, sel_registerName("subviews"));
            size_t n = subs ? ((size_t (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
            level_subs[depth] = subs;
            level_count[depth] = n > WALK_WIDTH ? WALK_WIDTH : n;
         }
         if (level_pos[depth] < level_count[depth] && depth + 1 < WALK_LEVELS) {
            id next = ((id (*)(id, SEL, size_t))objc_msgSend)(level_subs[depth], sel_registerName("objectAtIndex:"),
               level_pos[depth]++);
            depth++;
            level_view[depth] = next;
            level_subs[depth] = nil;
            level_pos[depth] = 0;
         } else if (level_pos[depth] < level_count[depth]) {
            level_pos[depth] = level_count[depth];  /* at the last level: do not descend, just finish it */
         } else if (depth == 0) {
            break;                                  /* the root is done: depth-- here would wrap size_t and loop for ever */
         } else {
            depth--;                            /* this level is exhausted: back up to the parent */
         }
      }
   }
}

static void
apply_glass(void *unused)
{
   bool battery = keep_battery_percentage();
   herodrm_log("herosb: glass on: battery percentage %s, radii dock %.0f cards %.0f panels %.0f",
      battery ? "yes" : "NO", DOCK_RADIUS, CARD_RADIUS, GLASS_RADIUS);
   glass_on = true;
   /*
    * Rounds, not one walk: the dock, the switcher cards and the banners are made after SpringBoard's first layout, and
    * re-made on every presentation. Five-second rounds for two and a half minutes, then one every twenty seconds.
    */
   dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, paint_glass);
   for (unsigned i = 0; i < 30; i++)
      dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * (i + 1)) * NSEC_PER_SEC),
         dispatch_get_main_queue(), NULL, paint_glass);
   dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, paint_glass);
}

static void
report_glass(void *unused)
{
   herodrm_log("herosb: glass so far: dock %u, cards %u, panels %u (of %zu view objects seen)",
      glass_docks, glass_cards, glass_glass, seen_count);
}

__attribute__((constructor)) static void
herosb_start(void)
{
   if (access(DROP_DIR "fps.off", F_OK) != 0) {
      raise_frame_rates();
      dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, report_ranges);
   }
   if (access(DROP_DIR "gestures.off", F_OK) != 0)
      ask_for_gestures();
   /*
    * The theme is independent of the wallpaper: the old code returned here when wallpaper.off was present, which would
    * have silently disabled the theme too.
    */
   if (access(DROP_DIR "glass.off", F_OK) != 0) {
      dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, apply_glass);
      dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, report_glass);
   } else {
      herodrm_log("herosb: glass.off present, stock look kept");
   }
   if (access(DROP_DIR "wallpaper.off", F_OK) == 0)
      return;
   dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, look_at_windows);
}
