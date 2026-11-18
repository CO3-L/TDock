// Rendering adapted from TaskDock native v0.7.4.3.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <winternl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <wincodec.h>
#include <MinHook.h>
#include "TaskbarCenter.h"
#ifdef _MSC_VER
#include <intrin.h>
#define TDOCK_RETURN_ADDRESS() _ReturnAddress()
#else
#define TDOCK_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kControlName[] = L"TDock.Native.Control.v2";
constexpr wchar_t kStartErrorProperty[] = L"TDock.Native.StartError.v2";
constexpr wchar_t kControllerMutexName[] = L"Local\\TDock.Controller.v1";
constexpr UINT_PTR kControllerTimer = 0x54444F4C;

constexpr WPARAM kEnable = 1;
constexpr WPARAM kQuery = 2;
constexpr WPARAM kDisable = 3;
constexpr WPARAM kConfigure = 4;
constexpr WPARAM kQueryCenter = 5;
constexpr unsigned kDurationMask = 0xffff;
constexpr unsigned kAnimate = 0x10000;
constexpr unsigned kCenter = 0x20000;
constexpr LRESULT kBackendVersion = 0x5444010F;
constexpr LRESULT kActive = 0x54440001;
constexpr LRESULT kStopped = 0x54440002;
constexpr LRESULT kUnsupported = 0x54440003;
constexpr LRESULT kStartFailed = 0x54440004;
constexpr LRESULT kBusy = 0x54440005;
constexpr LRESULT kStopFailed = 0x54440006;

HANDLE g_controllerMutex = nullptr;
bool g_hooksPrepared = false;
void* g_buttonCloseEntry = nullptr;
thread_local bool g_buttonCloseEnabled = false;

UINT ControlMessage() {
    static const UINT message = RegisterWindowMessageW(kControlName);
    return message;
}

template <typename... Args>
void Log(PCWSTR format, Args... args) {
    wchar_t message[512];
    _snwprintf_s(message, _countof(message), _TRUNCATE, format, args...);
    OutputDebugStringW(L"TDock: ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
}

bool IsControllerRunning(HANDLE mutex) {
    if (!mutex) return false;
    const DWORD result = WaitForSingleObject(mutex, 0);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) ReleaseMutex(mutex);
    return result == WAIT_TIMEOUT;
}

LRESULT DisableEffects(HWND window);
LRESULT ConfigureEffects(HWND window, WPARAM settings);


constexpr wchar_t kTaskListClass[] = L"MSTaskListWClass";
constexpr wchar_t kThumbnailClass[] = L"TaskListThumbnailWnd";
constexpr wchar_t kThumbnailOverlayClass[] = L"TaskListOverlayWnd";
constexpr size_t kButtonRenderInfoSize = 0x140;
constexpr size_t kHoverOpacityOffset = 0x130;
constexpr UINT_PTR kSubclassId = 0x54444F4B;
constexpr double kDefaultFrameInterval = 1000.0 / 60.0;
constexpr DWORD kMaxFrameRate = 165;
constexpr double kEnterDuration = 85.0;
constexpr double kLeaveDuration = 125.0;
constexpr double kNeighborTravel = 6.0;  // Pixels at 96 DPI.
constexpr double kClickHop = 360.0;
constexpr double kClickRebound = 180.0;
constexpr double kClickFlight = kClickHop + kClickRebound;
constexpr double kClickSettle = 120.0;
constexpr double kClickTiming = kClickFlight + kClickSettle;
constexpr int kDefaultClickDuration = 720;
constexpr int kMinClickDuration = 300;
constexpr int kMaxClickDuration = 1200;
constexpr int kClickDurationStep = 10;
constexpr double kClickCaptureTimeout = 130.0;
constexpr double kClickHeight = 30.0;  // Pixels at 96 DPI.
constexpr GUID kImageListId = {0x46eb5926, 0x582e, 0x4017, {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};

using DrawButton = void (*)(void* self, HDC dc, const void* renderInfo);
using ImageListDraw = HRESULT (STDMETHODCALLTYPE*)(IImageList* self, IMAGELISTDRAWPARAMS* params);
using DrawOverlay = HRESULT (*)(HDC dc, int x, int y, int width, int height, HICON icon, BYTE opacity);

using RenderButton = void (*)(void* self, HDC dc, int index, int state, int mode, POINT* origin);
DrawButton g_drawButtonOriginal;
RenderButton g_renderOriginal;
void* g_renderEntry;
void* g_groupEntry;
void* g_appIdEntry;
void* g_shortcutEntry;
ImageListDraw g_imageListDrawOriginal;
DrawOverlay g_drawOverlayOriginal;
decltype(&AlphaBlend) g_badgeBlendOriginal;
decltype(&CreateWindowExW) g_createWindowOriginal;
decltype(&SetWindowPos) g_setWindowPosOriginal;
void* g_overlayDrawEntry;
const void* g_notificationBadgeCaller;

std::atomic<HWND> g_taskList{nullptr};
std::atomic<bool> g_stopping{true};
std::atomic<bool> g_loggedResize{false};
std::atomic<bool> g_loggedTransparentHover{false};
std::atomic<bool> g_loggedBounce{false};
std::atomic<bool> g_loggedPreviewOrder{false};
constexpr int kMaxScale = 145;
constexpr int kRadius = 100;
UINT g_frameMessage;
std::atomic<int> g_clickDuration{kDefaultClickDuration};
std::atomic<bool> g_framePending{false};

struct Pixel {
    uint8_t blue, green, red, alpha;
};
static_assert(sizeof(Pixel) == 4);

struct Raster {
    int width = 0;
    int height = 0;
    std::vector<Pixel> pixels;
    explicit operator bool() const { return !pixels.empty(); }
    const Pixel& At(int x, int y) const { return pixels[y * width + x]; }
};

struct Dib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    Pixel* pixels = nullptr;

    Dib(int width, int height) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return;
        oldBitmap = SelectObject(dc, bitmap);
        if (oldBitmap && oldBitmap != HGDI_ERROR) pixels = static_cast<Pixel*>(bits);
    }
    ~Dib() {
        if (oldBitmap && oldBitmap != HGDI_ERROR) SelectObject(dc, oldBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;
};

void RecoverAlpha(Pixel* black, const Pixel* white, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        auto& dark = black[i];
        const auto& light = white[i];
        int delta = (light.red-dark.red + light.green-dark.green + light.blue-dark.blue + 1) / 3;
        dark.alpha = static_cast<uint8_t>(255 - std::clamp(delta, 0, 255));
        dark.red = std::min(dark.red, dark.alpha);
        dark.green = std::min(dark.green, dark.alpha);
        dark.blue = std::min(dark.blue, dark.alpha);
    }
}

struct BounceVisual {
    RECT bounds;
    RECT nativeIcon;
    RECT restingIcon;
    Dib pixels;
    std::unique_ptr<Dib> white;
    HWND layer = nullptr;

    BounceVisual(RECT button, RECT native, RECT resting)
        : bounds(button), nativeIcon(native), restingIcon(resting),
          pixels(button.right - button.left, button.bottom - button.top),
          white(std::make_unique<Dib>(button.right - button.left, button.bottom - button.top)) {
        const size_t count = static_cast<size_t>(button.right - button.left) * (button.bottom - button.top);
        if (pixels.pixels) std::fill_n(pixels.pixels, count, Pixel{0, 0, 0, 255});
        if (white->pixels) std::fill_n(white->pixels, count, Pixel{255, 255, 255, 255});
    }

    ~BounceVisual() {
        if (layer) DestroyWindow(layer);
    }

    template <typename Draw>
    bool Capture(const Draw& draw) {
        return pixels.pixels && white && white->pixels && draw(pixels.dc) && draw(white->dc);
    }

    bool FinishCapture() {
        GdiFlush();
        const size_t count = static_cast<size_t>(bounds.right - bounds.left) * (bounds.bottom - bounds.top);
        RecoverAlpha(pixels.pixels, white->pixels, count);
        white.reset();
        return std::any_of(pixels.pixels, pixels.pixels + count, [](Pixel pixel) { return pixel.alpha > 8; });
    }

    bool IsVisible() const { return layer && IsWindowVisible(layer); }

    bool Present(HWND owner, POINT position) {
        constexpr UINT kPositionFlags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;
        if (layer) {
            return IsVisible() &&
                   SetWindowPos(layer, nullptr, position.x, position.y, 0, 0, kPositionFlags | SWP_NOZORDER);
        }
        layer = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                L"STATIC", nullptr, WS_POPUP, 0, 0, 0, 0, owner, nullptr, nullptr, nullptr);
        if (!layer) return false;
        SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
        POINT source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(layer, nullptr, &position, &size, pixels.dc, &source, 0, &blend, ULW_ALPHA)) {
            return false;
        }
        return SetWindowPos(layer, HWND_TOPMOST, 0, 0, 0, 0, kPositionFlags | SWP_NOMOVE | SWP_SHOWWINDOW) &&
               IsWindowVisible(layer);
    }
};

// Animation and its windows are owned by the taskbar window thread.
struct ButtonIdentity {
    const void* group = nullptr;
    int itemIndex = 0;
    bool operator==(const ButtonIdentity&) const = default;
};

struct ClickBounce {
    ButtonIdentity button;
    double pickedAt = 0;
    int durationMs = kDefaultClickDuration;
    double flightStart = 0;
    std::unique_ptr<BounceVisual> visual;
    double nativePaintAt = 0;
    bool nativeIconPainted = false;
    bool landingFrameDrawn = false;
    bool seen = false;
};

struct Animation {
    double cursorX = 0;
    double intensity = 0;
    double transitionStart = 0;
    double transitionIntensity = 0;
    bool cursorInside = false;
    bool tracking = false;
    bool pressed = false;
    bool pickingButton = false;
    POINT pressedAt{};
    POINT releasedAt{};
    RECT dragBounds{};
    std::array<ClickBounce, 4> bounces{};
    double bounceFrameTime = 0;
    bool renderingFrame = false;
};
Animation& AnimationState() {
    // The module is pinned. Windows reclaims remaining visuals at Explorer shutdown.
    static auto* animation = new Animation;
    return *animation;
}
// The frame clock belongs to the task list. Either effect can use it on its own.
struct FrameSchedule {
    double next = 0;
    double interval = kDefaultFrameInterval;
    HANDLE timer = nullptr;
    PTP_WAIT wait = nullptr;
};
FrameSchedule g_frames;

thread_local HWND g_paintWindow;
thread_local bool g_onTaskbarThread;

struct ButtonPaint {
    HDC dc;
    RECT bounds;
    RECT badgeBounds;
    POINT badgeOffset;
    bool drawingOverlay;
    ClickBounce* bounce;
    bool iconInLayer;
};
thread_local ButtonPaint g_buttonPaint;

RECT InkBounds(const Raster& image) {
    RECT ink{image.width, image.height, 0, 0};
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (image.At(x, y).alpha <= 8) continue;
            ink.left = std::min(ink.left, static_cast<LONG>(x));
            ink.top = std::min(ink.top, static_cast<LONG>(y));
            ink.right = std::max(ink.right, static_cast<LONG>(x + 1));
            ink.bottom = std::max(ink.bottom, static_cast<LONG>(y + 1));
        }
    }
    return ink;
}

Raster Trim(const Raster& image) {
    RECT ink = InkBounds(image);
    if (IsRectEmpty(&ink)) return {};
    Raster result{ink.right - ink.left, ink.bottom - ink.top, {}};
    result.pixels.reserve(result.width * result.height);
    for (LONG y = ink.top; y < ink.bottom; ++y) {
        const Pixel* row = &image.At(ink.left, y);
        result.pixels.insert(result.pixels.end(), row, row + result.width);
    }
    return result;
}

struct ReleaseCom {
    void operator()(IUnknown* object) const { if (object) object->Release(); }
};
template <typename T>
using Com = std::unique_ptr<T, ReleaseCom>;
struct FreeShellMemory {
    void operator()(void* value) const { CoTaskMemFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST_ABSOLUTE, FreeShellMemory>;

Raster ReadBitmap(IWICImagingFactory* factory, IWICBitmapSource* source) {
    UINT width, height;
    if (FAILED(source->GetSize(&width, &height)) || !width || !height || width > 512 || height > 512) return {};
    IWICFormatConverter* raw = nullptr;
    if (FAILED(factory->CreateFormatConverter(&raw))) return {};
    Com<IWICFormatConverter> converter(raw);
    if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
    Raster result{static_cast<int>(width), static_cast<int>(height), std::vector<Pixel>(width * height)};
    if (FAILED(converter->CopyPixels(nullptr, width * 4, width * height * 4,
                                     reinterpret_cast<BYTE*>(result.pixels.data())))) return {};
    return Trim(result);
}

Raster ReadPng(IWICImagingFactory* factory, const std::filesystem::path& path) {
    IWICBitmapDecoder* raw = nullptr;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &raw))) return {};
    Com<IWICBitmapDecoder> decoder(raw);
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame))) return {};
    Com<IWICBitmapFrameDecode> source(frame);
    return ReadBitmap(factory, source.get());
}

Raster ReadShellImage(IWICImagingFactory* factory, IShellItem2* item) {
    IShellItemImageFactory* raw = nullptr;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&raw)))) return {};
    Com<IShellItemImageFactory> images(raw);
    HBITMAP bitmap = nullptr;
    auto flags = static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK);
    if (FAILED(images->GetImage({96, 96}, flags, &bitmap)) || !bitmap) return {};
    IWICBitmap* decoded = nullptr;
    HRESULT result = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &decoded);
    Raster image;
    if (SUCCEEDED(result)) {
        Com<IWICBitmap> source(decoded);
        image = ReadBitmap(factory, source.get());
    }
    DeleteObject(bitmap);
    return image;
}

std::wstring ItemProperty(IShellItem2* item, PCWSTR name) {
    PROPERTYKEY key{};
    PWSTR value = nullptr;
    if (FAILED(PSGetPropertyKeyFromName(name, &key)) || FAILED(item->GetString(key, &value)) || !value) return {};
    std::unique_ptr<wchar_t, FreeShellMemory> owned(value);
    return value;
}

void AddPackageImages(IWICImagingFactory* factory, IShellItem2* item, std::vector<Raster>& images) {
    using std::filesystem::path;
    auto install = path(ItemProperty(item, L"System.AppUserModel.PackageInstallPath"));
    auto relative = path(ItemProperty(item, L"System.Tile.SmallLogoPath"));
    if (!install.is_absolute() || relative.empty() || relative.has_root_path()) return;
    for (const auto& part : relative) if (part == L"..") return;

    auto logo = install / relative;
    auto folder = logo.parent_path();
    const auto pattern = folder / (logo.stem().wstring() + L".*.png");
    path best[2];
    int sizes[2]{};
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring_view name(data.cFileName);
            int variant = name.ends_with(L"altform-lightunplated.png") ? 1 : 0;
            if (!variant && !name.ends_with(L"altform-unplated.png")) continue;
            int size = 1;
            for (auto prefix : {std::wstring_view(L".targetsize-"), std::wstring_view(L".scale-")}) {
                size_t index = name.find(prefix);
                if (index != name.npos) {
                    size = _wtoi(data.cFileName + index + prefix.size());
                    if (prefix == L".scale-") size = size * 44 / 100;
                    break;
                }
            }
            if (size > sizes[variant]) {
                sizes[variant] = size;
                best[variant] = folder / data.cFileName;
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    for (const auto& candidate : best) {
        if (candidate.empty()) continue;
        auto image = ReadPng(factory, candidate);
        if (image) images.push_back(std::move(image));
    }
    if (images.empty()) {
        auto image = ReadPng(factory, logo);
        if (image) images.push_back(std::move(image));
    }
}

struct Artwork {
    std::vector<Raster> images;
    uint64_t revision = 0;
};

struct ArtRequest {
    std::wstring appId;
    Pidl shortcut;
    uint64_t generation = 0;
};

struct ArtCacheEntry {
    std::shared_ptr<const Artwork> artwork;
    uint64_t lastUsed;
    size_t pixelBytes = 0;
    bool loading = true;
};

constexpr size_t kArtCacheEntries = 128;
constexpr size_t kArtCacheBytes = 8 * 1024 * 1024;

struct ArtService {
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::wstring, ArtCacheEntry, std::less<>> cache;
    std::deque<ArtRequest> pending;
    std::thread worker;
    size_t pixelBytes = 0;
    uint64_t accessOrder = 0;
    uint64_t generation = 0;

    // These operations run with the service mutex held.
    bool EvictOldestLoaded() {
        auto oldest = cache.end();
        for (auto item = cache.begin(); item != cache.end(); ++item) {
            if (!item->second.loading &&
                (oldest == cache.end() || item->second.lastUsed < oldest->second.lastUsed)) {
                oldest = item;
            }
        }
        if (oldest == cache.end()) return false;
        pixelBytes -= oldest->second.pixelBytes;
        cache.erase(oldest);
        return true;
    }

    bool Publish(const ArtRequest& request, std::shared_ptr<Artwork> artwork) {
        if (request.generation != generation) return false;
        auto item = cache.find(request.appId);
        if (item == cache.end() || !item->second.loading) return false;
        auto& entry = item->second;
        entry.lastUsed = ++accessOrder;
        entry.loading = false;
        if (!artwork || artwork->images.empty()) return false;

        size_t bytes = 0;
        for (const auto& image : artwork->images) {
            bytes += image.pixels.capacity() * sizeof(Pixel);
        }
        if (bytes > kArtCacheBytes) return false;
        artwork->revision = entry.lastUsed;
        entry.pixelBytes = bytes;
        entry.artwork = std::move(artwork);
        pixelBytes += bytes;
        while (pixelBytes > kArtCacheBytes) EvictOldestLoaded();
        return true;
    }

    void Clear() {
        // The worker can finish an older request after animation is enabled again.
        ++generation;
        pending.clear();
        cache.clear();
        pixelBytes = 0;
    }
};
// The artwork worker is reused across enables and belongs to Explorer.
std::atomic<ArtService*> g_artService{nullptr};
std::atomic<bool> g_artRedrawPending{false};
thread_local bool g_paintCanLoadArt = true;
thread_local void* g_renderedButtonGroup;
std::atomic<bool> g_loggedHd{false};

std::shared_ptr<Artwork> LoadArt(IWICImagingFactory* factory, const ArtRequest& request) {
    auto result = std::make_shared<Artwork>();
    IShellItem2* item = nullptr;
    auto appPath = L"shell:AppsFolder\\" + request.appId;
    if (SUCCEEDED(SHCreateItemFromParsingName(appPath.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        Com<IShellItem2> app(item);
        if (request.appId.find(L'!') != std::wstring::npos) {
            AddPackageImages(factory, app.get(), result->images);
        }
        auto image = ReadShellImage(factory, app.get());
        if (image) result->images.push_back(std::move(image));
    }
    item = nullptr;
    if (request.shortcut && SUCCEEDED(SHCreateItemFromIDList(request.shortcut.get(), IID_PPV_ARGS(&item)))) {
        Com<IShellItem2> shortcut(item);
        auto image = ReadShellImage(factory, shortcut.get());
        if (image) result->images.push_back(std::move(image));
    }
    return result;
}

void ArtWorker(ArtService* service) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
    IWICImagingFactory* raw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) {
        Com<IWICImagingFactory> factory(raw);
        for (;;) {
            ArtRequest request;
            {
                std::unique_lock lock(service->mutex);
                service->wake.wait(lock, [service] { return !service->pending.empty(); });
                request = std::move(service->pending.front());
                service->pending.pop_front();
            }
            std::shared_ptr<Artwork> result;
            try {
                result = LoadArt(factory.get(), request);
            } catch (const std::exception&) {
                // A missing resource leaves the original icon available for resampling.
            }
            std::lock_guard lock(service->mutex);
            if (service->Publish(request, std::move(result)) && !g_stopping.load()) {
                if (HWND window = g_taskList.load(); window && !g_artRedrawPending.exchange(true)) {
                    InvalidateRect(window, nullptr, TRUE);
                }
            }
        }
    }
    CoUninitialize();
}

std::shared_ptr<const Artwork> RequestArt(PCWSTR appId, PCIDLIST_ABSOLUTE shortcut, bool canLoad) {
    auto* service = g_artService.load();
    if (!service || !appId || !*appId) return {};
    std::lock_guard lock(service->mutex);
    auto entry = service->cache.find(appId);
    if (entry != service->cache.end()) {
        entry->second.lastUsed = ++service->accessOrder;
        return entry->second.artwork;
    }
    if (!canLoad) return {};
    if (service->cache.size() >= kArtCacheEntries && !service->EvictOldestLoaded()) return {};
    entry = service->cache.emplace(appId, ArtCacheEntry{nullptr, ++service->accessOrder}).first;
    try {
        service->pending.push_back({entry->first, Pidl(shortcut ? ILCloneFull(shortcut) : nullptr), service->generation});
    } catch (...) {
        service->cache.erase(entry);
        throw;
    }
    service->wake.notify_one();
    return {};
}

double SimilarityError(const Raster& native, const Raster& candidate) {
    constexpr int grid = 16;
    int difference = 0;
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
            const Pixel& a = native.At((2*x+1)*native.width/(2*grid), (2*y+1)*native.height/(2*grid));
            const Pixel& b = candidate.At((2*x+1)*candidate.width/(2*grid), (2*y+1)*candidate.height/(2*grid));
            difference += std::abs(a.blue-b.blue) + std::abs(a.green-b.green) + std::abs(a.red-b.red) + std::abs(a.alpha-b.alpha);
        }
    }
    return difference / (grid * grid * 4.0 * 255.0);
}

Raster CaptureIcon(IImageList* list, int index, int width, int height) {
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) return {};
    Dib bitmap(width, height);
    Raster result;
    if (bitmap.pixels) {
        // GDI can discard the alpha channel when drawing an icon into a memory DC.
        // Its colors over black and white reconstruct the icon's coverage.
        std::fill_n(bitmap.pixels, width*height, Pixel{0,0,0,255});
        if (DrawIconEx(bitmap.dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL)) {
            GdiFlush();
            result = {width, height, std::vector<Pixel>(bitmap.pixels, bitmap.pixels + width*height)};
            std::fill_n(bitmap.pixels, width*height, Pixel{255,255,255,255});
            if (DrawIconEx(bitmap.dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL)) {
                GdiFlush();
                RecoverAlpha(result.pixels.data(), bitmap.pixels, result.pixels.size());
            } else {
                result = {};
            }
        }
    }
    DestroyIcon(icon);
    RECT ink = InkBounds(result);
    return IsRectEmpty(&ink) ? Raster{} : std::move(result);
}

// Integrate the source pixels covered by each destination pixel when shrinking.
// For upscaling a small icon, use a four-sample cubic kernel in each direction.
using Weights = std::vector<std::vector<std::pair<int, double>>>;
Weights ResamplingWeights(int source, int destination) {
    Weights weights(destination);
    const double ratio = static_cast<double>(source) / destination;
    for (int i = 0; i < destination; ++i) {
        if (ratio >= 1) {
            double left = i * ratio, right = (i + 1) * ratio;
            for (int p = static_cast<int>(left); p < right && p < source; ++p) {
                double weight = (std::min(right, p+1.0) - std::max(left, static_cast<double>(p))) / ratio;
                if (weight > 0) weights[i].emplace_back(p, weight);
            }
        } else {
            double position = (i + 0.5) * ratio - 0.5;
            int start = static_cast<int>(std::floor(position));
            for (int p = start-1; p <= start+2; ++p) {
                double x = std::abs(position-p);
                double weight = x <= 1 ? (1.5*x-2.5)*x*x+1 : x < 2 ? ((-0.5*x+2.5)*x-4)*x+2 : 0;
                weights[i].emplace_back(std::clamp(p, 0, source-1), weight);
            }
        }
    }
    return weights;
}

void Resample(const Raster& source, Pixel* output, int stride, int width, int height) {
    auto horizontal = ResamplingWeights(source.width, width);
    auto vertical = ResamplingWeights(source.height, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double blue = 0, green = 0, red = 0, alpha = 0;
            for (const auto& [sy, wy] : vertical[y]) {
                for (const auto& [sx, wx] : horizontal[x]) {
                    double weight = wx * wy;
                    const auto& pixel = source.At(sx, sy);
                    blue += pixel.blue*weight; green += pixel.green*weight;
                    red += pixel.red*weight; alpha += pixel.alpha*weight;
                }
            }
            int opacity = std::clamp(static_cast<int>(std::lround(alpha)), 0, 255);
            auto channel = [opacity](double value) {
                return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, opacity));
            };
            output[y*stride+x] = {channel(blue), channel(green), channel(red), static_cast<uint8_t>(opacity)};
        }
    }
}

struct IconPaint {
    HIMAGELIST list = nullptr;
    int index = -1, width = 0, height = 0;
    ULONGLONG captured = 0;
    uint64_t artRevision = 0;
    size_t artIndex = 0;
    Raster image;
    RECT ink{};
    bool highRes = false;
    int scaledWidth = 0, scaledHeight = 0;
    std::unique_ptr<Dib> frame;
};
#ifdef __clang__
[[clang::no_destroy]]
#endif
std::vector<IconPaint>& IconPaintCache() {
    static auto* cache = new std::vector<IconPaint>;
    return *cache;
}

IconPaint PrepareIcon(IImageList* list, const IMAGELISTDRAWPARAMS& params, ULONGLONG now, const Artwork* artwork) {
    IconPaint paint;
    paint.list = params.himl;
    paint.index = params.i;
    paint.width = params.cx;
    paint.height = params.cy;
    paint.captured = now;
    paint.artRevision = artwork ? artwork->revision : 0;
    paint.image = CaptureIcon(list, params.i, params.cx, params.cy);
    paint.ink = {0, 0, params.cx, params.cy};

    if (!artwork || !paint.image) return paint;
    RECT nativeInk = InkBounds(paint.image);
    auto native = Trim(paint.image);
    const Raster* best = nullptr;
    double error = 0.24;
    for (const auto& image : artwork->images) {
        if (image.width <= native.width || image.height <= native.height) continue;
        double candidate = SimilarityError(native, image);
        if (candidate < error) {
            best = &image;
            error = candidate;
        }
    }
    if (best) {
        // RenderHook holds the matching artwork for the entire draw.
        paint.artIndex = static_cast<size_t>(best - artwork->images.data());
        paint.image = {};
        paint.ink = nativeInk;
        paint.highRes = true;
    }
    return paint;
}

bool PaintSmoothedIcon(IImageList* list, const IMAGELISTDRAWPARAMS& native,
                       const IMAGELISTDRAWPARAMS& target, const Artwork* artwork) {
    if (native.cx > 128 || native.cy > 128 || target.cx > 192 || target.cy > 192 ||
        native.xBitmap || native.yBitmap || (native.rgbBk != CLR_NONE && !(native.fStyle & ILD_TRANSPARENT)) ||
        (native.fStyle & (ILD_MASK | ILD_BLEND25 | ILD_BLEND50 | ILD_OVERLAYMASK | ILD_ROP)) ||
        (native.fState & ~static_cast<DWORD>(ILS_ALPHA))) return false;

    ULONGLONG now = GetTickCount64();
    auto& cache = IconPaintCache();
    auto found = std::find_if(cache.begin(), cache.end(), [&](const IconPaint& cached) {
        return cached.list == native.himl && cached.index == native.i && cached.width == native.cx && cached.height == native.cy;
    });
    if (found == cache.end()) {
        if (cache.size() >= 64) cache.erase(cache.begin());
        cache.push_back(PrepareIcon(list, native, now, artwork));
        found = std::prev(cache.end());
    } else if (found->artRevision != (artwork ? artwork->revision : 0) || now - found->captured > 1800) {
        *found = PrepareIcon(list, native, now, artwork);
    }

    auto& cached = *found;
    const Raster& image = cached.highRes ? artwork->images[cached.artIndex] : cached.image;
    if (!image) return false;
    if (!cached.frame || cached.scaledWidth != target.cx || cached.scaledHeight != target.cy) {
        auto frame = std::make_unique<Dib>(target.cx, target.cy);
        if (!frame->pixels) return false;
        std::fill_n(frame->pixels, target.cx * target.cy, Pixel{});
        int left = MulDiv(cached.ink.left, target.cx, native.cx);
        int top = MulDiv(cached.ink.top, target.cy, native.cy);
        int right = MulDiv(cached.ink.right, target.cx, native.cx);
        int bottom = MulDiv(cached.ink.bottom, target.cy, native.cy);
        int width = right-left, height = bottom-top;
        if (cached.highRes) {
            double scale = std::min(static_cast<double>(width) / image.width,
                                    static_cast<double>(height) / image.height);
            int fittedWidth = std::clamp(static_cast<int>(std::lround(image.width*scale)), 1, width);
            int fittedHeight = std::clamp(static_cast<int>(std::lround(image.height*scale)), 1, height);
            left += (width-fittedWidth)/2;
            top += (height-fittedHeight)/2;
            width = fittedWidth;
            height = fittedHeight;
        }
        Resample(image, frame->pixels + top*target.cx + left, target.cx, width, height);
        cached.frame = std::move(frame);
        cached.scaledWidth = target.cx;
        cached.scaledHeight = target.cy;
    }
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>((native.fState & ILS_ALPHA) ? std::min(native.Frame, 255UL) : 255), AC_SRC_ALPHA};
    if (!AlphaBlend(target.hdcDst, target.x, target.y, target.cx, target.cy,
                    cached.frame->dc, 0, 0, target.cx, target.cy, blend)) return false;
    if (cached.highRes && !g_loggedHd.exchange(true)) {
        Log(L"High-resolution icon drawn: %dx%d asset -> %dx%d native target",
               image.width, image.height, target.cx, target.cy);
    }
    return true;
}

bool IsClass(HWND window, PCWSTR name) {
    wchar_t buffer[64];
    return window && GetClassNameW(window, buffer, ARRAYSIZE(buffer)) && std::wcscmp(buffer, name) == 0;
}

bool IsBottomTaskList(HWND window) {
    HWND tray = GetAncestor(window, GA_ROOT);
    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    return IsClass(window, kTaskListClass) && IsClass(tray, L"Shell_TrayWnd") &&
           GetWindowRect(tray, &bounds) &&
           GetMonitorInfoW(MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), &monitor) &&
           bounds.right - bounds.left > bounds.bottom - bounds.top &&
           bounds.top >= (monitor.rcMonitor.top + monitor.rcMonitor.bottom) / 2;
}

HWND FindTaskList() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return nullptr;
    HWND rebar = FindWindowExW(tray, nullptr, L"ReBarWindow32", nullptr);
    if (!rebar) return nullptr;
    HWND band = FindWindowExW(rebar, nullptr, L"MSTaskSwWClass", nullptr);
    return band ? FindWindowExW(band, nullptr, kTaskListClass, nullptr) : nullptr;
}

UINT_PTR FrameId() {
    return reinterpret_cast<UINT_PTR>(&g_frames);
}

double FrameTime() {
    static const double ticksPerMillisecond = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart / 1000.0;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart / ticksPerMillisecond;
}

double DisplayFrameInterval(HWND window) {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency >= 24) {
        return 1000.0 / std::min(mode.dmDisplayFrequency, kMaxFrameRate);
    }
    return kDefaultFrameInterval;
}

void CALLBACK FrameTimerReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
    const HWND window = static_cast<HWND>(context);
    if (g_taskList.load() != window || !PostMessageW(window, g_frameMessage, FrameId(), 0)) {
        g_framePending.store(false);
    }
}

void StartFrameRequests(HWND window) {
    g_frames.interval = DisplayFrameInterval(window);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer) return;
    g_frames.wait = CreateThreadpoolWait(FrameTimerReady, window, nullptr);
    if (g_frames.wait) g_frames.timer = timer;
    else CloseHandle(timer);
}

void StopFrameRequests(HWND window) {
    KillTimer(window, FrameId());
    if (g_frames.wait) {
        SetThreadpoolWait(g_frames.wait, nullptr, nullptr);
        CancelWaitableTimer(g_frames.timer);
        WaitForThreadpoolWaitCallbacks(g_frames.wait, TRUE);
        CloseThreadpoolWait(g_frames.wait);
        CloseHandle(g_frames.timer);
    }
    g_frames = {};
    g_framePending.store(false);
}

void RequestFrame(HWND window, double now) {
    if (g_framePending.exchange(true)) return;
    const double delay = g_frames.next - now;
    if (delay <= 0 && PostMessageW(window, g_frameMessage, FrameId(), 0)) return;

    if (g_frames.wait) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(std::ceil(std::max(delay, 0.1) * 10000.0));
        if (SetWaitableTimer(g_frames.timer, &due, 0, nullptr, nullptr, FALSE)) {
            SetThreadpoolWait(g_frames.wait, g_frames.timer, nullptr);
            return;
        }
    }
    if (!SetTimer(window, FrameId(), static_cast<UINT>(std::max(delay, 1.0) + 0.5), nullptr)) {
        g_framePending.store(false);
    }
}

bool IsClickDuration(WPARAM duration) {
    return duration >= kMinClickDuration && duration <= kMaxClickDuration &&
           duration % kClickDurationStep == 0;
}

double ClickLift(double elapsed) {
    auto hop = [](double t) {
        t = std::clamp(t, 0.0, 1.0);
        return 4 * t * (1 - t);
    };
    return kClickHeight * (hop(elapsed / kClickHop) + 0.28 * hop((elapsed - kClickHop) / kClickRebound));
}

void PickClickedButton(const RECT& bounds, ButtonIdentity button) {
    auto& animation = AnimationState();
    if (!animation.pickingButton || !PtInRect(&bounds, animation.pressedAt) ||
        !PtInRect(&bounds, animation.releasedAt)) return;
    animation.pickingButton = false;
    const double now = FrameTime();
    ClickBounce* available = nullptr;
    for (auto& bounce : animation.bounces) {
        if (bounce.button == button) return;  // A rapid repeat lets the current hop finish.
        if (!bounce.button.group && !available) available = &bounce;
    }
    if (!available) return;
    *available = {.button = button, .pickedAt = now, .durationMs = g_clickDuration.load(std::memory_order_relaxed)};
    RequestFrame(g_paintWindow, now);
}

ClickBounce* FindClickBounce(ButtonIdentity button) {
    for (auto& bounce : AnimationState().bounces) {
        if (bounce.button == button) return &bounce;
    }
    return nullptr;
}

void TrackButtonPress(HWND window, POINT point) {
    auto& animation = AnimationState();
    const UINT dpi = GetDpiForWindow(window);
    const int dx = std::max(1, GetSystemMetricsForDpi(SM_CXDRAG, dpi ? dpi : 96));
    const int dy = std::max(1, GetSystemMetricsForDpi(SM_CYDRAG, dpi ? dpi : 96));
    animation.pressed = true;
    animation.pressedAt = point;
    animation.dragBounds = {point.x - dx, point.y - dy, point.x + dx + 1, point.y + dy + 1};
}

void ReleaseButtonPress(HWND window, POINT point) {
    auto& animation = AnimationState();
    if (!std::exchange(animation.pressed, false) || !PtInRect(&animation.dragBounds, point)) return;
    RECT client{};
    if (!GetClientRect(window, &client) || !PtInRect(&client, point)) return;
    animation.releasedAt = point;
    animation.pickingButton = true;
    // Identify the button in its native paint before Explorer processes the release.
    const RECT pick{point.x, client.top, point.x + 1, client.bottom};
    InvalidateRect(window, &pick, FALSE);
    UpdateWindow(window);
    animation.pickingButton = false;
}

double HoverIntensity(const Animation& animation, double now) {
    const double target = animation.cursorInside ? 1 : 0;
    const double duration = animation.cursorInside ? kEnterDuration : kLeaveDuration;
    const double remaining = 1 - std::clamp((now - animation.transitionStart) / duration, 0.0, 1.0);
    return std::lerp(target, animation.transitionIntensity, remaining * remaining * remaining);
}

void SetCursorInside(Animation& animation, bool inside, double now) {
    if (inside == animation.cursorInside) return;
    animation.transitionIntensity = HoverIntensity(animation, now);
    animation.transitionStart = now;
    animation.cursorInside = inside;
}

bool IsThumbnailWindow(HWND window);

HWND ReadTaskListCursor(HWND window, POINT* cursor) {
    POINT screen{};
    RECT client{};
    if (!IsWindowVisible(window) || !GetCursorPos(&screen) || !GetClientRect(window, &client)) return nullptr;
    *cursor = screen;
    if (!ScreenToClient(window, cursor) || !PtInRect(&client, *cursor)) return nullptr;

    const HWND hit = WindowFromPoint(screen);
    const HWND root = GetAncestor(hit, GA_ROOT);
    const HWND tray = GetAncestor(window, GA_ROOT);
    // The preview can take the mouse hit during its transition even inside the task list.
    if (tray && (root == tray ||
                 (IsThumbnailWindow(root) && GetWindowThreadProcessId(root, nullptr) == GetCurrentThreadId()))) {
        return hit;
    }
    return nullptr;
}

struct HoverSample {
    HWND hit;
    POINT cursor;
};

HoverSample UpdateHoverFromCursor(HWND window, double now) {
    HoverSample sample{};
    sample.hit = ReadTaskListCursor(window, &sample.cursor);
    SetCursorInside(AnimationState(), sample.hit != nullptr, now);
    if (sample.hit != window) AnimationState().tracking = false;
    return sample;
}

void TrackTaskListLeave(HWND window, HWND hit) {
    if (hit != window || AnimationState().tracking) return;
    const HWND capture = GetCapture();
    if (capture && capture != window) return;

    TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, window, 0};
    AnimationState().tracking = TrackMouseEvent(&event) != FALSE;
}

bool RenderIconFrame(HWND window, double now, bool layoutMoved) {
    auto& animation = AnimationState();
    const double oldX = animation.cursorX;
    const double oldIntensity = animation.intensity;
    if (layoutMoved || animation.cursorInside || animation.intensity > 0 || animation.bounceFrameTime != 0) {
        // Mouse messages update hover state. Only animation frames advance the drawing position.
        const auto [hit, cursor] = UpdateHoverFromCursor(window, now);
        if (hit) animation.cursorX = cursor.x;
        // Tracking from WM_MOUSELEAVE can immediately enqueue another leave.
        TrackTaskListLeave(window, hit);
    }
    animation.intensity = HoverIntensity(animation, now);
    const bool wasBouncing = animation.bounceFrameTime != 0;
    bool bouncing = false;
    for (auto& bounce : animation.bounces) {
        const bool expired = bounce.flightStart ? now - bounce.flightStart >= bounce.durationMs
                                                : now - bounce.pickedAt >= kClickCaptureTimeout;
        if (bounce.button.group && expired && (!bounce.visual || !bounce.visual->IsVisible())) bounce = {};
        bounce.landingFrameDrawn = false;
        bounce.seen = false;
        bouncing |= bounce.button.group != nullptr;
    }
    animation.bounceFrameTime = bouncing ? now : 0;
    if (bouncing || wasBouncing || animation.intensity != oldIntensity ||
        (animation.intensity > 0 && animation.cursorX != oldX)) {
        InvalidateRect(window, nullptr, TRUE);
        // The coalesced frame message already limits how often native painting runs.
        animation.renderingFrame = true;
        UpdateWindow(window);
        animation.renderingFrame = false;
        for (auto& bounce : animation.bounces) {
            if (!bounce.flightStart) continue;
            if (!bounce.seen) {
                bounce = {};
                continue;
            }
            if (!bounce.visual || !bounce.visual->IsVisible()) continue;
            const bool coveredNativeFrame = bounce.landingFrameDrawn &&
                                            now - bounce.nativePaintAt >= g_frames.interval;
            if (!coveredNativeFrame) {
                if (now - bounce.flightStart >= bounce.durationMs + kClickCaptureTimeout) {
                    bounce = {};
                    InvalidateRect(window, nullptr, FALSE);
                }
                continue;
            }

            // The original window procedure has finished drawing. The sprite covers the first
            // native frame while Windows presents the taskbar's updated buffer.
            ShowWindow(bounce.visual->layer, SW_HIDE);
            if (now - bounce.flightStart >= bounce.durationMs) bounce = {};
        }
    }
    const double target = animation.cursorInside ? 1 : 0;
    return bouncing || animation.intensity != target || (animation.cursorInside && !animation.tracking);
}

bool IconFits(const IMAGELISTDRAWPARAMS& icon, const RECT& bounds) {
    return icon.x >= bounds.left && icon.y >= bounds.top &&
           icon.cx <= bounds.right - icon.x && icon.cy <= bounds.bottom - icon.y;
}

bool AnimateIcon(const IMAGELISTDRAWPARAMS& native, const RECT& client, const RECT& buttonBounds,
                 double cursorX, double intensity, double radius, double maximum, double travel,
                 IMAGELISTDRAWPARAMS* adjusted) {
    if (native.cx <= 0 || native.cy <= 0 || maximum <= 1 || !IconFits(native, client)) return false;

    const double centerX = native.x + native.cx / 2.0;
    const double distance = (centerX - cursorX) / radius;
    const double falloff = 1 - distance * distance;
    if (falloff <= 0) return false;
    const double weight = falloff * falloff;

    RECT button{};
    const bool hasButtonBounds = IntersectRect(&button, &client, &buttonBounds) && IconFits(native, button);
    const RECT& limits = hasButtonBounds ? button : client;
    const double zoom = (maximum - 1) * intensity;
    double scale = 1 + zoom * weight;
    const double horizontalSpace = 2 * std::min(centerX - limits.left, limits.right - centerX);
    const double verticalSpace = native.y + native.cy - limits.top;
    scale = std::min({scale, horizontalSpace / native.cx, verticalSpace / native.cy});
    const int width = static_cast<int>(std::lround(native.cx * scale));
    const int height = static_cast<int>(std::lround(native.cy * scale));
    const bool resized = width != native.cx || height != native.cy;

    LONG x = native.x - (width - native.cx) / 2;
    if (hasButtonBounds) {
        // Both the displacement and its slope vanish at the edge of the radius.
        constexpr double peak = 0.2862167011;  // Maximum of |t * (1 - t*t)^2|.
        const double limit = std::min(travel, radius * 0.07);
        const double shift = limit * std::min(zoom / 0.30, intensity) * distance * weight / peak;
        x = std::clamp(x + std::lround(shift), limits.left, limits.right - width);
    }
    if (!resized && x == native.x) return false;

    *adjusted = native;
    adjusted->x = x;
    adjusted->y = native.y - (height - native.cy);
    adjusted->cx = width;
    adjusted->cy = height;
    if (resized) adjusted->fStyle |= ILD_SCALE;
    return true;
}

RECT IconRect(const IMAGELISTDRAWPARAMS& draw) {
    return {draw.x, draw.y, draw.x + draw.cx, draw.y + draw.cy};
}

bool RectContains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

void CaptureClickIcon(ClickBounce& bounce, const RECT& bounds, IImageList* list,
                      const IMAGELISTDRAWPARAMS& native, const IMAGELISTDRAWPARAMS& drawn,
                      bool smoothed, const Artwork* artwork) {
    const RECT icon = IconRect(drawn);
    if (drawn.cx <= 0 || drawn.cy <= 0 || IsRectEmpty(&bounds) || !RectContains(bounds, icon) ||
        bounds.right - bounds.left > 2048 || bounds.bottom - bounds.top > 512 ||
        (GetLayout(native.hdcDst) & LAYOUT_RTL)) {
        bounce = {};
        return;
    }
    try {
        auto visual = std::make_unique<BounceVisual>(bounds, IconRect(native), icon);
        auto local = drawn;
        local.x -= bounds.left;
        local.y -= bounds.top;
        // Replay only the icon and its badges onto two solid mattes to recover their alpha.
        if (visual->Capture([&](HDC dc) {
            local.hdcDst = dc;
            return smoothed ? PaintSmoothedIcon(list, native, local, artwork)
                            : SUCCEEDED(g_imageListDrawOriginal(list, &local));
        })) {
            bounce.visual = std::move(visual);
            return;
        }
    } catch (const std::exception&) {
    }
    bounce = {};
}

template <typename Draw>
void CaptureClickBadge(POINT position, int width, int height, const Draw& draw) {
    auto* bounce = g_buttonPaint.bounce;
    if (!bounce || !bounce->visual || !bounce->visual->white) return;
    auto& visual = *bounce->visual;
    if (width <= 0 || height <= 0 ||
        !RectContains(visual.bounds, {position.x, position.y, position.x + width, position.y + height}) ||
        !visual.Capture([&](HDC dc) { return draw(dc, position.x - visual.bounds.left,
                                                      position.y - visual.bounds.top); })) {
        *bounce = {};
    }
}

bool PresentClickBounce(HWND window, ClickBounce& bounce, const RECT& bounds, const IMAGELISTDRAWPARAMS& native,
                        double dpiScale, IMAGELISTDRAWPARAMS& target) {
    auto& visual = *bounce.visual;
    const LONG dx = native.x - visual.nativeIcon.left;
    const LONG dy = native.y - visual.nativeIcon.top;
    RECT resting = visual.restingIcon;
    OffsetRect(&resting, dx, dy);
    if (native.cx != visual.nativeIcon.right - visual.nativeIcon.left ||
        native.cy != visual.nativeIcon.bottom - visual.nativeIcon.top || !RectContains(bounds, resting)) {
        bounce = {};
        return false;
    }

    const double now = AnimationState().bounceFrameTime;
    const double pace = kClickTiming / bounce.durationMs;
    const double elapsed = bounce.flightStart ? (now - bounce.flightStart) * pace : 0;
    const bool landing = elapsed >= kClickFlight;
    const bool spriteVisible = visual.IsVisible();
    if (landing) {
        if (spriteVisible && !AnimationState().renderingFrame) return true;

        // Draw at the sprite's position until the first full native frame is ready.
        const double settleStart = bounce.nativePaintAt ? bounce.nativePaintAt + g_frames.interval : now;
        const double settleEnd = bounce.flightStart + bounce.durationMs;
        const double t = spriteVisible ? 0.0 : std::clamp(
            (now - settleStart) / std::max(1.0, settleEnd - settleStart), 0.0, 1.0);
        const double eased = t * t * (3 - 2 * t);
        auto interpolate = [eased](int start, int end) { return static_cast<int>(std::lround(std::lerp(start, end, eased))); };
        const RECT desired = IconRect(target);
        target.x = interpolate(resting.left, desired.left);
        target.y = interpolate(resting.top, desired.top);
        target.cx = interpolate(resting.right, desired.right) - target.x;
        target.cy = interpolate(resting.bottom, desired.bottom) - target.y;
        if (target.cx != native.cx || target.cy != native.cy) target.fStyle |= ILD_SCALE;
        if (!spriteVisible) return false;
    }

    // Move the sprite only during a full paint so a clipped redraw cannot expose it twice.
    if (!AnimationState().renderingFrame) return spriteVisible;
    POINT origin{visual.bounds.left + dx, visual.bounds.top + dy};
    HWND owner = GetAncestor(window, GA_ROOT);
    MONITORINFO monitor{sizeof(monitor)};
    if (!owner || !IsWindowVisible(window) || !ClientToScreen(window, &origin) ||
        !GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        bounce = {};
        return false;
    }
    RECT restingOnScreen = visual.restingIcon;
    OffsetRect(&restingOnScreen, origin.x - visual.bounds.left, origin.y - visual.bounds.top);
    if (!RectContains(monitor.rcMonitor, restingOnScreen)) {
        bounce = {};
        return false;
    }
    const LONG lift = landing ? 0 : std::min(std::lround(ClickLift(elapsed) * dpiScale),
                                             restingOnScreen.top - monitor.rcMonitor.top);
    origin.y -= lift;
    if (!visual.Present(owner, origin)) {
        bounce = {};
        return false;
    }
    if (!bounce.flightStart) bounce.flightStart = now;
    if (lift > 0 && !g_loggedBounce.exchange(true)) Log(L"Click bounce presented a transparent icon above the taskbar.");
    return !landing;
}

POINT BadgePosition(int x, int y, int width, int height) {
    const auto& bounds = g_buttonPaint.badgeBounds;
    const auto& offset = g_buttonPaint.badgeOffset;
    if ((!offset.x && !offset.y) || width <= 0 || height <= 0 || x < bounds.left || y < bounds.top ||
        width > bounds.right - x || height > bounds.bottom - y) return {x, y};
    return {std::clamp(x + offset.x, bounds.left, bounds.right - width),
            std::clamp(y + offset.y, bounds.top, bounds.bottom - height)};
}

std::shared_ptr<const Artwork> ArtworkForResizedIcon() {
    void* self = g_renderedButtonGroup;
    if (!self || !g_groupEntry || !g_appIdEntry || !g_shortcutEntry) return {};
    auto* methods = *reinterpret_cast<void***>(self);
    if (methods[6] != g_groupEntry || methods[21] != g_renderEntry) return {};

    void* group = reinterpret_cast<void* (*)(void*)>(g_groupEntry)(self);
    if (!group) return {};
    auto* groupMethods = *reinterpret_cast<void***>(group);
    if (groupMethods[19] != g_appIdEntry || groupMethods[29] != g_shortcutEntry) return {};

    auto appId = reinterpret_cast<PCWSTR (*)(void*)>(g_appIdEntry)(group);
    auto shortcut = reinterpret_cast<PCIDLIST_ABSOLUTE (*)(void*)>(g_shortcutEntry)(group);
    return RequestArt(appId, shortcut, g_paintCanLoadArt);
}

HRESULT STDMETHODCALLTYPE ImageListDrawHook(IImageList* self, IMAGELISTDRAWPARAMS* params) {
    HWND window = g_paintWindow;
    auto* bounce = g_buttonPaint.bounce;
    if (!window || (AnimationState().intensity <= 0 && !bounce) || !g_buttonPaint.dc || !params ||
        params->cbSize < sizeof(*params) || params->hdcDst != g_buttonPaint.dc ||
        g_stopping.load(std::memory_order_relaxed)) return g_imageListDrawOriginal(self, params);

    // The verified DrawIcon32bpp helper uses these flags on its temporary list.
    constexpr UINT kExplorerOverlayStyle = 0x50001;
    if (g_buttonPaint.drawingOverlay ||
        (!params->himl && params->fStyle == kExplorerOverlayStyle &&
         params->fState == ILS_ALPHA)) return g_imageListDrawOriginal(self, params);
    const bool composite = (params->fStyle & ILD_OVERLAYMASK) != 0;
    if (composite && !bounce) return g_imageListDrawOriginal(self, params);

    RECT client{};
    if (!GetClientRect(window, &client)) return g_imageListDrawOriginal(self, params);
    auto adjusted = *params;
    const UINT dpi = GetDpiForWindow(window);
    const double dpiScale = (dpi ? dpi : 96) / 96.0;
    const double radius = kRadius * dpiScale;
    if (!composite) {
        AnimateIcon(*params, client, g_buttonPaint.bounds, AnimationState().cursorX, AnimationState().intensity, radius,
                    kMaxScale / 100.0, kNeighborTravel * dpiScale, &adjusted);
    }
    RECT bounds{};
    IntersectRect(&bounds, &client, &g_buttonPaint.bounds);
    if (bounce) {
        bounce->seen = true;
        if (bounce->visual && !bounce->visual->white &&
            PresentClickBounce(window, *bounce, bounds, *params, dpiScale, adjusted)) {
            // The layer owns this icon only after Windows has accepted the sprite.
            g_buttonPaint.iconInLayer = true;
            return S_OK;
        }
    }

    bool painted = false;
    const bool resized = adjusted.cx != params->cx || adjusted.cy != params->cy;
    const bool moved = resized || adjusted.x != params->x || adjusted.y != params->y;
    std::shared_ptr<const Artwork> artwork;
    if (resized) {
        try {
            artwork = ArtworkForResizedIcon();
            painted = PaintSmoothedIcon(self, *params, adjusted, artwork.get());
        } catch (const std::exception&) {
            // Keep Explorer's draw available if a cached raster cannot be prepared.
        }
    }
    const HRESULT result = painted ? S_OK : g_imageListDrawOriginal(self, moved ? &adjusted : params);
    if (SUCCEEDED(result)) {
        if (resized && !g_loggedResize.exchange(true)) {
            Log(L"Native icon draw resized: %dx%d -> %dx%d", params->cx, params->cy, adjusted.cx, adjusted.cy);
        }
        if (!IsRectEmpty(&bounds) && IconFits(*params, bounds)) {
            g_buttonPaint.badgeBounds = bounds;
            g_buttonPaint.badgeOffset = {adjusted.x + adjusted.cx - (params->x + params->cx),
                                         adjusted.y + adjusted.cy - (params->y + params->cy)};
        }
        if (bounce && bounce->button.group && !bounce->visual) {
            CaptureClickIcon(*bounce, bounds, self, *params, adjusted, painted, artwork.get());
        }
        if (bounce && bounce->visual && AnimationState().renderingFrame && bounce->visual->IsVisible()) {
            bounce->nativeIconPainted = true;
        }
    }
    return result;
}

HRESULT DrawOverlayHook(HDC dc, int x, int y, int width, int height, HICON icon, BYTE opacity) {
    if (!g_paintWindow || dc != g_buttonPaint.dc || g_stopping.load(std::memory_order_relaxed)) {
        return g_drawOverlayOriginal(dc, x, y, width, height, icon, opacity);
    }
    if (g_buttonPaint.iconInLayer) return S_OK;

    const bool previous = g_buttonPaint.drawingOverlay;
    g_buttonPaint.drawingOverlay = true;
    const POINT position = BadgePosition(x, y, width, height);
    const HRESULT result = g_drawOverlayOriginal(dc, position.x, position.y, width, height, icon, opacity);
    if (SUCCEEDED(result)) {
        CaptureClickBadge(position, width, height, [&](HDC target, int localX, int localY) {
            return SUCCEEDED(g_drawOverlayOriginal(target, localX, localY, width, height, icon, opacity));
        });
    }
    g_buttonPaint.drawingOverlay = previous;
    return result;
}

BOOL WINAPI NotificationBadgeBlendHook(HDC dc, int x, int y, int width, int height,
                                       HDC source, int sx, int sy, int sw, int sh, BLENDFUNCTION blend) {
    if ((g_buttonPaint.bounce || g_buttonPaint.badgeOffset.x || g_buttonPaint.badgeOffset.y) &&
        dc == g_buttonPaint.dc && g_paintWindow &&
        TDOCK_RETURN_ADDRESS() == g_notificationBadgeCaller && !g_stopping.load(std::memory_order_relaxed)) {
        if (g_buttonPaint.iconInLayer) return TRUE;
        const POINT position = BadgePosition(x, y, width, height);
        const BOOL result = g_badgeBlendOriginal(dc, position.x, position.y, width, height, source, sx, sy, sw, sh, blend);
        if (result) {
            CaptureClickBadge(position, width, height, [&](HDC target, int localX, int localY) {
                return g_badgeBlendOriginal(target, localX, localY, width, height, source, sx, sy, sw, sh, blend) != FALSE;
            });
        }
        return result;
    }
    return g_badgeBlendOriginal(dc, x, y, width, height, source, sx, sy, sw, sh, blend);
}

void RenderHook(void* self, HDC dc, int index, int state, int mode, POINT* origin) {
    void* previous = std::exchange(g_renderedButtonGroup, self);
    g_renderOriginal(self, dc, index, state, mode, origin);
    g_renderedButtonGroup = previous;
}

void DrawButtonHook(void* self, HDC dc, const void* renderInfo) {
    const ButtonPaint previous = g_buttonPaint;
    alignas(16) std::array<unsigned char, kButtonRenderInfoSize> paintInfo;
    if (g_paintWindow && !g_stopping.load(std::memory_order_relaxed)) {
        g_buttonPaint = {dc};
        if (renderInfo) {
            // This BUTTONRENDERINFO starts with a 32-bit mode followed by a RECT.
            const auto* bytes = static_cast<const unsigned char*>(renderInfo);
            std::memcpy(&g_buttonPaint.bounds, bytes + sizeof(DWORD), sizeof(RECT));
            if (self && (AnimationState().pickingButton || AnimationState().bounceFrameTime != 0)) {
                // This is the item index used by _DrawRegularButton on both verified builds.
                int itemIndex = 0;
                std::memcpy(&itemIndex, bytes + 0x108, sizeof(itemIndex));
                // A pinned or combined button and its first running item share one visible slot.
                const ButtonIdentity button{self, std::max(0, itemIndex)};
                PickClickedButton(g_buttonPaint.bounds, button);
                g_buttonPaint.bounce = FindClickBounce(button);
            }

            // Explorer supplies the hover fade separately from the button's other states.
            int hoverOpacity = 0;
            std::memcpy(&hoverOpacity, bytes + kHoverOpacityOffset, sizeof(hoverOpacity));
            if (hoverOpacity > 0) {
                std::memcpy(paintInfo.data(), bytes, paintInfo.size());
                std::memset(paintInfo.data() + kHoverOpacityOffset, 0, sizeof(hoverOpacity));
                renderInfo = paintInfo.data();
                if (!g_loggedTransparentHover.exchange(true, std::memory_order_relaxed)) {
                    Log(L"Native taskbar hover background made transparent.");
                }
            }
        }
    }
    g_drawButtonOriginal(self, dc, renderInfo);
    if (auto* bounce = g_buttonPaint.bounce; !g_stopping.load(std::memory_order_relaxed) && g_paintWindow && bounce && bounce->visual &&
        bounce->visual->white && !bounce->visual->FinishCapture()) {
        *bounce = {};
    }
    g_buttonPaint = previous;
}

bool IsThumbnailClass(PCWSTR name) {
    return std::wcscmp(name, kThumbnailClass) == 0 || std::wcscmp(name, kThumbnailOverlayClass) == 0;
}

bool IsThumbnailWindow(HWND window) {
    return IsClass(window, kThumbnailClass) || IsClass(window, kThumbnailOverlayClass);
}

bool IsAbove(HWND upper, HWND lower) {
    for (HWND window = GetWindow(lower, GW_HWNDPREV); window; window = GetWindow(window, GW_HWNDPREV)) {
        if (window == upper) return true;
    }
    return false;
}

bool IsBounceInProgress() {
    return g_onTaskbarThread && !g_stopping.load(std::memory_order_relaxed) && AnimationState().bounceFrameTime != 0;
}

void KeepThumbnailBelowBounce(HWND window, HWND& insertAfter, UINT& flags) {
    HWND lowestLayer = nullptr;
    for (const auto& bounce : AnimationState().bounces) {
        const HWND layer = bounce.visual ? bounce.visual->layer : nullptr;
        if (layer && IsWindowVisible(layer) && (!lowestLayer || IsAbove(lowestLayer, layer))) {
            lowestLayer = layer;
        }
    }
    if (!lowestLayer) return;

    // A newly created preview can already be above the sprite while it is hidden.
    const bool wouldCoverBounce = (flags & SWP_NOZORDER)
        ? IsAbove(window, lowestLayer)
        : insertAfter == HWND_TOP || insertAfter == HWND_TOPMOST || IsAbove(insertAfter, lowestLayer);
    if (wouldCoverBounce) {
        insertAfter = lowestLayer;
        flags &= ~SWP_NOZORDER;
        if (!g_loggedPreviewOrder.exchange(true, std::memory_order_relaxed)) {
            Log(L"Kept the taskbar preview below the bounce in its window positioning operation.");
        }
    }
}

LRESULT CALLBACK ThumbnailSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ThumbnailSubclass, kSubclassId);
    if (message == WM_SHOWWINDOW && wParam && IsBounceInProgress()) {
        // ShowWindow can keep the hidden window's existing z-order when showing it.
        HWND insertAfter = nullptr;
        UINT flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE |
                     SWP_NOZORDER | SWP_NOSENDCHANGING;
        KeepThumbnailBelowBounce(window, insertAfter, flags);
        if (insertAfter) SetWindowPos(window, insertAfter, 0, 0, 0, 0, flags);
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_WINDOWPOSCHANGING && lParam && IsBounceInProgress()) {
        auto& position = *reinterpret_cast<WINDOWPOS*>(lParam);
        KeepThumbnailBelowBounce(window, position.hwndInsertAfter, position.flags);
    }
    return result;
}

BOOL CALLBACK SubclassThumbnails(HWND window, LPARAM attach) {
    if (IsThumbnailWindow(window)) {
        if (attach) SetWindowSubclass(window, ThumbnailSubclass, kSubclassId, 0);
        else RemoveWindowSubclass(window, ThumbnailSubclass, kSubclassId);
    }
    return TRUE;
}

BOOL CALLBACK NoVisibleThumbnail(HWND window, LPARAM) {
    RECT bounds{};
    return !IsThumbnailWindow(window) || !IsWindowVisible(window) ||
           !GetWindowRect(window, &bounds) || IsRectEmpty(&bounds);
}

bool CanMoveTaskList() {
    const auto& animation = AnimationState();
    return !animation.pressed && !animation.bounceFrameTime && !GetCapture() &&
           !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
           std::none_of(animation.bounces.begin(), animation.bounces.end(),
                        [](const ClickBounce& bounce) { return bounce.button.group != nullptr; }) &&
           EnumThreadWindows(GetCurrentThreadId(), NoVisibleThumbnail, 0);
}

void RenderTaskbarFrame(HWND window) {
    const double now = FrameTime();
    if (now + 0.2 < g_frames.next) {
        RequestFrame(window, now);
        return;
    }

    // Keep the display cadence through small thread delays. Skip missed frames without
    // sending a burst of outdated positions.
    const double scheduled = g_frames.next + g_frames.interval;
    g_frames.next = scheduled > now + g_frames.interval / 2 ? scheduled : now + g_frames.interval;
    const bool layoutPending = TaskbarCenter::NeedsFrame(now) || TaskbarCenter::HasDeferredLayout();
    const bool layoutMoved = layoutPending && TaskbarCenter::Advance(window, now, CanMoveTaskList());

    const bool animate = g_taskList.load() == window && !g_stopping.load(std::memory_order_relaxed) &&
                         RenderIconFrame(window, now, layoutMoved);
    if (g_taskList.load() == window && (animate || TaskbarCenter::NeedsFrame(now))) {
        RequestFrame(window, FrameTime());
    }
}

BOOL WINAPI SetWindowPosHook(HWND window, HWND insertAfter, int x, int y, int width, int height, UINT flags) {
    if ((flags & SWP_NOSENDCHANGING) && IsBounceInProgress() && IsThumbnailWindow(window)) {
        KeepThumbnailBelowBounce(window, insertAfter, flags);
    }
    return g_setWindowPosOriginal(window, insertAfter, x, y, width, height, flags);
}

LRESULT CALLBACK TaskListSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        DisableEffects(window);
        return DefSubclassProc(window, message, wParam, lParam);
    }
    if (message == ControlMessage()) {
        if (wParam == kQuery) return kBackendVersion;
        if (wParam == kEnable) return kActive;
        if (wParam == kQueryCenter) return static_cast<LRESULT>(TaskbarCenter::Status());
        if (wParam == kDisable || wParam == kConfigure) {
            if (g_paintWindow || AnimationState().renderingFrame || AnimationState().pickingButton ||
                TaskbarCenter::IsUpdating()) return kBusy;
            return wParam == kDisable ? DisableEffects(window) : ConfigureEffects(window, static_cast<WPARAM>(lParam));
        }
    }
    if (message == WM_TIMER && wParam == kControllerTimer) {
        if (!IsControllerRunning(g_controllerMutex) && !g_paintWindow && !AnimationState().renderingFrame &&
            !AnimationState().pickingButton && !TaskbarCenter::IsUpdating()) {
            DisableEffects(window);
        }
        return 0;
    }

    const bool layoutNotice = message == TaskbarCenter::LayoutMessage;
    if (layoutNotice || (message == WM_TIMER && wParam == TaskbarCenter::LayoutTimerId)) {
        const double now = FrameTime();
        TaskbarCenter::RequestLayout(now, layoutNotice);
        if (TaskbarCenter::NeedsFrame(now)) RequestFrame(window, now);
        return 0;
    }

    if ((message == g_frameMessage || message == WM_TIMER) && wParam == FrameId()) {
        if (message == WM_TIMER) KillTimer(window, FrameId());
        if (g_framePending.exchange(false)) RenderTaskbarFrame(window);
        return 0;
    }

    if (message == WM_DISPLAYCHANGE) g_frames.interval = DisplayFrameInterval(window);
    if ((message == WM_WINDOWPOSCHANGING || message == WM_WINDOWPOSCHANGED) && lParam) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        auto& position = *reinterpret_cast<WINDOWPOS*>(lParam);
        if (message == WM_WINDOWPOSCHANGING) TaskbarCenter::OnPositionChanging(window, position);
        else TaskbarCenter::OnPositionChanged(window, position);
        return result;
    }
    if ((message == WM_LBUTTONUP || message == WM_CAPTURECHANGED || message == WM_CANCELMODE) &&
        TaskbarCenter::HasDeferredLayout()) RequestFrame(window, FrameTime());
    if (!g_stopping.load(std::memory_order_relaxed)) {
        if (message == WM_MOUSEMOVE && (AnimationState().cursorInside || IsBottomTaskList(window))) {
            auto& animation = AnimationState();
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (animation.pressed && (!(wParam & MK_LBUTTON) || !PtInRect(&animation.dragBounds, point))) {
                animation.pressed = false;
            }
            const double now = FrameTime();
            // Enter immediately. Once inside, coalesce mouse moves into one sample per frame.
            if (!animation.cursorInside) UpdateHoverFromCursor(window, now);
            RequestFrame(window, now);
        } else if (message == WM_MOUSELEAVE) {
            const double now = FrameTime();
            AnimationState().tracking = false;
            UpdateHoverFromCursor(window, now);
            RequestFrame(window, now);
        } else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
            TrackButtonPress(window, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        } else if (message == WM_LBUTTONUP) {
            ReleaseButtonPress(window, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        } else if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
            AnimationState().pressed = false;
        } else if (message == WM_PAINT) {
            HWND previous = g_paintWindow;
            const bool couldLoadArt = g_paintCanLoadArt;
            const bool artworkReady = g_artRedrawPending.exchange(false);
            // A redraw caused by completing a lookup cannot enqueue another miss.
            g_paintCanLoadArt = couldLoadArt && (!artworkReady || AnimationState().renderingFrame);
            g_paintWindow = window;
            LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            g_paintWindow = previous;
            g_paintCanLoadArt = couldLoadArt;
            if (!previous && AnimationState().renderingFrame && AnimationState().bounceFrameTime != 0) {
                const bool landingDrawn = std::any_of(AnimationState().bounces.begin(), AnimationState().bounces.end(),
                    [](const ClickBounce& bounce) { return bounce.nativeIconPainted; });
                const double committedAt = landingDrawn && GdiFlush() ? FrameTime() : 0;
                for (auto& bounce : AnimationState().bounces) {
                    if (std::exchange(bounce.nativeIconPainted, false) && committedAt) {
                        if (!bounce.nativePaintAt) bounce.nativePaintAt = committedAt;
                        bounce.landingFrameDrawn = true;
                    }
                }
            }
            return result;
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool InstallSubclass(HWND window) {
    if (g_taskList.load() || !IsBottomTaskList(window)) return false;
    if (!SetWindowSubclass(window, TaskListSubclass, kSubclassId, 0)) return false;
    if (!SetTimer(window, kControllerTimer, 1000, nullptr)) {
        RemoveWindowSubclass(window, TaskListSubclass, kSubclassId);
        return false;
    }
    AnimationState() = {};
    g_taskList.store(window);
    StartFrameRequests(window);
    Log(L"Attached to the primary classic task list.");
    return true;
}

HWND WINAPI CreateWindowHook(DWORD exStyle, LPCWSTR className, LPCWSTR title, DWORD style,
                             int x, int y, int width, int height, HWND parent, HMENU menu,
                             HINSTANCE instance, LPVOID parameter) {
    HWND window = g_createWindowOriginal(exStyle, className, title, style, x, y, width, height,
                                          parent, menu, instance, parameter);
    if (window && g_onTaskbarThread && className && !IS_INTRESOURCE(className) && IsThumbnailClass(className)) {
        const DWORD lastError = GetLastError();
        SetWindowSubclass(window, ThumbnailSubclass, kSubclassId, 0);
        SetLastError(lastError);
    }
    return window;
}

struct ExplorerBuild {
    GUID pdb;
    DWORD imageSize;
    DWORD taskButtonVtable;
    DWORD taskGroupVtable;
    DWORD overlayDrawEntry;
};

constexpr ExplorerBuild kKnownExplorerBuilds[] = {
    // Explorer component 19041.7802, Windows 10 KB5126657.
    {{0xec4c6ac6, 0xd86e, 0xfce1, {0x95, 0x7e, 0x20, 0x81, 0x39, 0x6e, 0x1a, 0xe4}},
     0x5c9000, 0x36df78, 0x36dd88, 0x2af094},
    // Explorer component 19041.7844, Windows 10 KB5131096.
    {{0x42960a6a, 0xc0d6, 0xa6f4, {0x47, 0xcd, 0x2f, 0x10, 0xb5, 0x74, 0x80, 0xaf}},
     0x5ca000, 0x36ef78, 0x36ed88, 0x2af394},
};

const ExplorerBuild* FindKnownExplorerBuild(const void* module) {
    if (!module) return nullptr;
    const auto* image = static_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto& header = nt->OptionalHeader;
    if (nt->Signature != IMAGE_NT_SIGNATURE || header.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        header.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) return nullptr;

    const auto& directory = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!directory.VirtualAddress || directory.VirtualAddress >= header.SizeOfImage ||
        directory.Size > header.SizeOfImage - directory.VirtualAddress) return nullptr;
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(image + directory.VirtualAddress);
    constexpr DWORD kRsds = 0x53445352;
    struct CodeViewId { DWORD signature; GUID guid; DWORD age; };
    static_assert(sizeof(CodeViewId) == 24);
    for (size_t i = 0; i < directory.Size / sizeof(*entries); ++i) {
        const auto& entry = entries[i];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entry.AddressOfRawData ||
            entry.AddressOfRawData >= header.SizeOfImage || entry.SizeOfData < sizeof(CodeViewId) ||
            entry.SizeOfData > header.SizeOfImage - entry.AddressOfRawData) continue;
        CodeViewId id{};
        std::memcpy(&id, image + entry.AddressOfRawData, sizeof(id));
        if (id.signature != kRsds || id.age != 1) continue;
        for (const auto& build : kKnownExplorerBuilds) {
            if (header.SizeOfImage == build.imageSize && IsEqualGUID(id.guid, build.pdb)) return &build;
        }
    }
    return nullptr;
}

void* ResolveKnownExplorerMethods(void* module, const ExplorerBuild& build) {
    // The two verified binaries share these code RVAs and the button render layout.
    auto* image = static_cast<unsigned char*>(module);
    auto* draw = image + 0x480f0;        // CTaskBtnGroup::_DrawRegularButton
    auto* render = image + 0x46b80;      // ITaskBtnGroup::Render
    auto* group = image + 0x9d160;       // ITaskBtnGroup::GetGroup
    auto* appId = image + 0x9ce30;       // ITaskGroup::GetAppID
    auto* shortcut = image + 0x9cd90;    // ITaskGroup::GetShortcutIDList
    constexpr unsigned char kDrawPrologue[] = {
        0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    };
    const auto* buttonMethods = reinterpret_cast<void* const*>(image + build.taskButtonVtable);
    const auto* groupMethods = reinterpret_cast<void* const*>(image + build.taskGroupVtable);
    if (std::memcmp(draw, kDrawPrologue, sizeof(kDrawPrologue)) != 0 ||
        buttonMethods[6] != group || buttonMethods[21] != render ||
        groupMethods[19] != appId || groupMethods[29] != shortcut) return nullptr;

    g_renderEntry = render;
    g_groupEntry = group;
    g_appIdEntry = appId;
    g_shortcutEntry = shortcut;
    constexpr unsigned char kOverlayPrologue[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10};
    auto* overlay = image + build.overlayDrawEntry;
    if (std::memcmp(overlay, kOverlayPrologue, sizeof(kOverlayPrologue)) == 0) g_overlayDrawEntry = overlay;
    g_notificationBadgeCaller = image + 0x4a060;  // Instruction after the taskbar badge blend call.
    return draw;
}

void* FindImageListDraw() {
    HMODULE controls = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<PCWSTR>(ImageList_Create), &controls);
    if (!controls) return nullptr;
    using QueryInterface = HRESULT (WINAPI*)(HIMAGELIST, REFIID, void**);
    auto query = reinterpret_cast<QueryInterface>(GetProcAddress(controls, "HIMAGELIST_QueryInterface"));
    if (!query) return nullptr;

    HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
    if (!images) return nullptr;
    IImageList* imageList = nullptr;
    void* address = nullptr;
    if (SUCCEEDED(query(images, kImageListId, reinterpret_cast<void**>(&imageList))) && imageList) {
        // IUnknown and the five image-list operations before Draw.
        constexpr int kDrawIndex = 8;
        address = (*reinterpret_cast<void***>(imageList))[kDrawIndex];
        imageList->Release();
    }
    ImageList_Destroy(images);
    return address;
}

bool IsSupportedWindows() {
    using GetVersion = LONG (NTAPI*)(PRTL_OSVERSIONINFOW);
    auto getVersion = reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{sizeof(version)};
    return getVersion && getVersion(&version) == 0 &&
           version.dwMajorVersion == 10 && version.dwBuildNumber == 19045;
}

bool SetHooksEnabled(bool enabled) {
    const MH_STATUS queued = enabled ? MH_QueueEnableHook(MH_ALL_HOOKS) : MH_QueueDisableHook(MH_ALL_HOOKS);
    if (queued != MH_OK) return false;
    if (g_buttonCloseEntry && g_buttonCloseEnabled != enabled) {
        const MH_STATUS center = g_buttonCloseEnabled ? MH_QueueEnableHook(g_buttonCloseEntry)
                                                      : MH_QueueDisableHook(g_buttonCloseEntry);
        if (center != MH_OK) return false;
    }
    return MH_ApplyQueued() == MH_OK;
}

template <typename Function>
bool CreateHook(void* address, Function hook, Function& original) {
    if (!address) return false;
    void* trampoline = nullptr;
    if (MH_CreateHook(address, reinterpret_cast<void*>(hook), &trampoline) != MH_OK) return false;
    original = reinterpret_cast<Function>(trampoline);
    return true;
}

using CanAnimateTask = HRESULT (*)(void*, int);
CanAnimateTask g_canAnimateTaskOriginal = nullptr;
const void* g_closeItemCaller = nullptr;

HRESULT CanAnimateTaskHook(void* self, int type) {
    constexpr int kRemoveButtonAnimation = 0x13;
    constexpr LONG_PTR kAnimationInterfaceOffset = 0x38;
    if (g_buttonCloseEnabled && type == kRemoveButtonAnimation && TDOCK_RETURN_ADDRESS() == g_closeItemCaller &&
        TaskbarCenter::Status() == TaskbarCenter::State::Centered) {
        // The task list window stores the CTaskListWnd base in its first extra slot.
        const HWND taskList = g_taskList.load();
        const LONG_PTR primary = taskList ? GetWindowLongPtrW(taskList, 0) : 0;
        if (primary && reinterpret_cast<LONG_PTR>(self) == primary + kAnimationInterfaceOffset) {
            // Explorer's immediate removal path supplies the final button span.
            // Measure after it returns so both directions use one slide target.
            TaskbarCenter::OnButtonRemoval(taskList);
            return E_FAIL;
        }
    }
    return g_canAnimateTaskOriginal(self, type);
}

void PrepareButtonCloseHook(unsigned char* image) {
    auto* entry = image + 0x6bb00;  // CTaskListWnd::CanAnimate
    auto* call = image + 0x6b602;   // CTaskBtnGroup::RemoveTaskItem
    constexpr unsigned char kEntry[] = {0x48, 0x8b, 0x89, 0x98, 0x04, 0x00, 0x00, 0x48, 0x85, 0xc9};
    constexpr unsigned char kCall[] = {
        0x8d, 0x56, 0x13, 0x48, 0x8b, 0x4b, 0x18, 0x48, 0x83, 0xc1, 0x38, 0xe8, 0xee, 0x04, 0, 0,
        0x44, 0x8d, 0x76, 0x01, 0x85, 0xc0, 0x0f, 0x85
    };
    if (std::memcmp(entry, kEntry, sizeof(kEntry)) != 0 || std::memcmp(call, kCall, sizeof(kCall)) != 0) return;
    if (CreateHook(entry, CanAnimateTaskHook, g_canAnimateTaskOriginal)) {
        g_closeItemCaller = call + 16;
        g_buttonCloseEntry = entry;
    }
}

LRESULT PrepareHooks() {
    if (g_hooksPrepared) return kActive;

    HMODULE explorer = GetModuleHandleW(nullptr);
    const auto* build = FindKnownExplorerBuild(explorer);
    if (!build) return kUnsupported;
    void* buttonDraw = ResolveKnownExplorerMethods(explorer, *build);
    if (!buttonDraw) return kUnsupported;
    void* imageDraw = FindImageListDraw();
    if (!imageDraw) return kStartFailed;

    AnimationState();
    IconPaintCache();

    if (MH_Initialize() != MH_OK) return kStartFailed;
    if (!CreateHook(buttonDraw, DrawButtonHook, g_drawButtonOriginal) ||
        !CreateHook(imageDraw, ImageListDrawHook, g_imageListDrawOriginal) ||
        !CreateHook(reinterpret_cast<void*>(CreateWindowExW), CreateWindowHook, g_createWindowOriginal)) {
        MH_Uninitialize();  // No callbacks have run.
        return kStartFailed;
    }

    if (!CreateHook(g_renderEntry, RenderHook, g_renderOriginal)) {
        Log(L"Artwork lookup unavailable. Using Explorer's icon resampler.");
    }
    CreateHook(g_overlayDrawEntry, DrawOverlayHook, g_drawOverlayOriginal);
    if (g_notificationBadgeCaller) {
        HMODULE gdi = GetModuleHandleW(L"gdi32full.dll");
        auto blend = gdi ? GetProcAddress(gdi, "GdiAlphaBlend") : nullptr;
        CreateHook(reinterpret_cast<void*>(blend), NotificationBadgeBlendHook, g_badgeBlendOriginal);
    }
    if (!CreateHook(reinterpret_cast<void*>(SetWindowPos), SetWindowPosHook, g_setWindowPosOriginal)) {
        Log(L"Using preview window messages for bounce layer ordering.");
    }
    PrepareButtonCloseHook(reinterpret_cast<unsigned char*>(explorer));
    g_hooksPrepared = true;
    return kActive;
}

LRESULT TryPrepareHooks() {
    try {
        return PrepareHooks();
    } catch (const std::exception&) {
        return kStartFailed;
    }
}

bool SetButtonCloseEnabled(bool enabled) {
    g_buttonCloseEnabled = false;
    if (!g_buttonCloseEntry) return true;
    const MH_STATUS status = enabled ? MH_EnableHook(g_buttonCloseEntry) : MH_DisableHook(g_buttonCloseEntry);
    if (status != MH_OK && status != (enabled ? MH_ERROR_ENABLED : MH_ERROR_DISABLED)) return false;
    g_buttonCloseEnabled = enabled;
    return true;
}

void StartArtWorker() {
    if (!g_renderOriginal || g_artService.load()) return;
    try {
        auto service = std::make_unique<ArtService>();
        service->worker = std::thread(ArtWorker, service.get());
        g_artService.store(service.release());
    } catch (const std::exception&) {
        Log(L"Artwork worker unavailable. Using Explorer's icon resampler.");
    }
}

bool StopAnimation(HWND window) {
    if (g_stopping.exchange(true)) return true;
    const bool hooksRestored = SetHooksEnabled(false);
    if (g_onTaskbarThread) {
        g_onTaskbarThread = false;
        EnumThreadWindows(GetCurrentThreadId(), SubclassThumbnails, FALSE);
    }
    AnimationState() = {};
    IconPaintCache().clear();
    if (auto* service = g_artService.load()) {
        std::lock_guard lock(service->mutex);
        service->Clear();
    }
    g_artRedrawPending.store(false);
    InvalidateRect(window, nullptr, TRUE);
    return hooksRestored;
}

LRESULT DisableEffects(HWND window) {
    const bool closeRestored = SetButtonCloseEnabled(false);
    const bool hooksRestored = StopAnimation(window);
    TaskbarCenter::Stop(window);
    KillTimer(window, kControllerTimer);
    RemoveWindowSubclass(window, TaskListSubclass, kSubclassId);
    g_taskList.store(nullptr);
    StopFrameRequests(window);
    if (g_controllerMutex) CloseHandle(std::exchange(g_controllerMutex, nullptr));
    return closeRestored && hooksRestored ? kStopped : kStopFailed;
}

LRESULT StartAnimation(HWND window) {
    const LRESULT prepared = TryPrepareHooks();
    if (prepared != kActive) return prepared;
    if (!SetHooksEnabled(true)) return SetHooksEnabled(false) ? kStartFailed : kStopFailed;

    g_onTaskbarThread = true;
    EnumThreadWindows(GetCurrentThreadId(), SubclassThumbnails, TRUE);
    StartArtWorker();
    g_stopping.store(false);
    InvalidateRect(window, nullptr, TRUE);
    return kActive;
}

LRESULT ConfigureEffects(HWND window, WPARAM settings) {
    const WPARAM duration = settings & kDurationMask;
    if (!IsClickDuration(duration) || (settings & ~(static_cast<WPARAM>(kDurationMask | kAnimate | kCenter)))) {
        return kStartFailed;
    }

    if (settings & kCenter) {
        if (TaskbarCenter::Status() == TaskbarCenter::State::Off) {
            if (!TaskbarCenter::Start(window)) return kStartFailed;
            if (TryPrepareHooks() == kActive && !SetButtonCloseEnabled(true)) {
                Log(L"Using Explorer's default task button removal animation.");
            }
        }
    } else if (TaskbarCenter::Status() != TaskbarCenter::State::Off) {
        if (AnimationState().bounceFrameTime) return kBusy;
        const bool closeRestored = SetButtonCloseEnabled(false);
        TaskbarCenter::Stop(window);
        if (!g_stopping.load(std::memory_order_relaxed)) {
            const double now = FrameTime();
            UpdateHoverFromCursor(window, now);
            RequestFrame(window, now);
        }
        if (!closeRestored) return kStopFailed;
    }

    g_clickDuration.store(static_cast<int>(duration), std::memory_order_relaxed);
    if (settings & kAnimate) {
        if (g_stopping.load()) return StartAnimation(window);
    } else if (!StopAnimation(window)) {
        return kStopFailed;
    }
    return kActive;
}

LRESULT EnableEffects(HWND window, WPARAM settings) {
    if (g_taskList.load() == window) return kActive;
    if (g_taskList.load()) return kBusy;
    if (!IsSupportedWindows() || !IsBottomTaskList(window)) return kUnsupported;
    g_frameMessage = RegisterWindowMessageW(L"TDock.Native.Frame.v2");
    if (!g_frameMessage) return kStartFailed;

    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, kControllerMutexName);
    if (!IsControllerRunning(mutex)) {
        if (mutex) CloseHandle(mutex);
        return kStartFailed;
    }
    // Explorer can still be returning through callbacks after the active subclass is removed.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<PCWSTR>(TaskListSubclass), &self) || !InstallSubclass(window)) {
        CloseHandle(mutex);
        return kStartFailed;
    }

    g_controllerMutex = mutex;
    const LRESULT result = ConfigureEffects(window, settings);
    if (result != kActive && TaskbarCenter::Status() == TaskbarCenter::State::Off) DisableEffects(window);
    return result;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI TDockBackendVersion() {
    return static_cast<DWORD>(kBackendVersion);
}

extern "C" __declspec(dllexport) LRESULT CALLBACK TDockTaskbarHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message.message == ControlMessage() && message.wParam == kEnable && message.hwnd == FindTaskList()) {
            DWORD process = 0;
            const DWORD thread = GetWindowThreadProcessId(message.hwnd, &process);
            if (process == GetCurrentProcessId() && thread == GetCurrentThreadId()) {
                RemovePropW(message.hwnd, kStartErrorProperty);
                const LRESULT result = EnableEffects(message.hwnd, static_cast<WPARAM>(message.lParam));
                if (result != kActive) {
                    SetPropW(message.hwnd, kStartErrorProperty, reinterpret_cast<HANDLE>(result));
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
