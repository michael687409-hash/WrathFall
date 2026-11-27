#include <windows.h>
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include "upscaler_events.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace events = asura::upscaler_events;
namespace capture = winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int64_t EpochMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Frame {
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t time = 0;
  std::vector<uint32_t> pixels;
  unsigned segment = 0;
};

using Snapshot = std::shared_ptr<const Frame>;

struct CaptureTick {
  int64_t compositor_time;  // 100 ns units, relative to the performance counter.
  int64_t unix_ms;
  unsigned segment;
};

struct CaptureBatch {
  std::deque<CaptureTick> ticks;
  std::deque<Snapshot> images;
};

struct Findings {
  unsigned green_percent = 0;
  bool green_tint = false;
  bool dark_square = false;
  bool blur = false;
  bool ghosting = false;
  double sharpness = 0;
  unsigned edge_samples = 0;
  unsigned ghost_tiles = 0;
};

int Luma(uint32_t pixel) {
  return int((77 * ((pixel >> 16) & 255) + 150 * ((pixel >> 8) & 255) +
              29 * (pixel & 255) + 128) >> 8);
}

struct MotionImage {
  static constexpr int kWidth = 320, kHeight = 180;
  std::vector<uint8_t> luma;
  int64_t time = 0;
  unsigned segment = 0;
  uint32_t width = 0, height = 0;

  explicit MotionImage(const Frame& frame)
      : luma(kWidth * kHeight), time(frame.time), segment(frame.segment),
        width(frame.width), height(frame.height) {
    for (int y = 0; y < kHeight; ++y) {
      const auto row = (2 * y + 1) * height / (2 * kHeight) * width;
      for (int x = 0; x < kWidth; ++x) {
        luma[y * kWidth + x] = uint8_t(Luma(frame.pixels[row + (2 * x + 1) * width / (2 * kWidth)]));
      }
    }
  }

  int At(int x, int y) const { return luma[y * kWidth + x]; }
};

void InspectSharpness(const Frame& frame, Findings& info) {
  if (frame.width < 32 || frame.height < 32) return;
  const auto sample = [&](uint32_t x, uint32_t y) { return Luma(frame.pixels[y * frame.width + x]); };
  const auto stride = std::max(2u, frame.width / 560);
  uint64_t fine = 0, broad = 0;
  for (uint32_t y = std::max(4u, frame.height / 9); y < frame.height * 8 / 9; y += stride) {
    for (uint32_t x = std::max(4u, frame.width / 9); x < frame.width * 8 / 9; x += stride) {
      const int wide_x = sample(x + 3, y) - sample(x - 3, y);
      const int wide_y = sample(x, y + 3) - sample(x, y - 3);
      if (std::max(std::abs(wide_x), std::abs(wide_y)) < 20) continue;
      ++info.edge_samples;
      const int near_x = sample(x + 1, y) - sample(x - 1, y);
      const int near_y = sample(x, y + 1) - sample(x, y - 1);
      fine += near_x * near_x + near_y * near_y;
      broad += wide_x * wide_x + wide_y * wide_y;
    }
  }
  if (broad) info.sharpness = double(fine) / double(broad);
}

bool HasFadingTrail(const MotionImage& current, const MotionImage& previous, int x, int y) {
  // Fit translation first, then look for a fainter copy at the feature's former position.
  const auto cost = [&](int dx, int dy) {
    int sum = 0;
    for (int j = -10; j <= 10; j += 4) for (int i = -10; i <= 10; i += 4) {
      const int error = current.At(x + i, y + j) - previous.At(x + i + dx, y + j + dy);
      sum += std::abs(error);
    }
    return sum;
  };
  const int stationary = cost(0, 0);
  if (stationary < 36 * 9) return false;
  int best = stationary, best_x = 0, best_y = 0;
  for (int dy = -12; dy <= 12; dy += 3) for (int dx = -12; dx <= 12; dx += 3) {
    const int error = cost(dx, dy);
    if (error < best) { best = error; best_x = dx; best_y = dy; }
  }
  const int coarse_x = best_x, coarse_y = best_y;
  for (int dy = coarse_y - 1; dy <= coarse_y + 1; ++dy) {
    for (int dx = coarse_x - 1; dx <= coarse_x + 1; ++dx) {
      const int error = cost(dx, dy);
      if (error < best) { best = error; best_x = dx; best_y = dy; }
    }
  }
  if (best * 4 > stationary * 3 || std::max(std::abs(best_x), std::abs(best_y)) < 3) return false;
  int original = 0, secondary = 0, overlap = 0;
  for (int j = -10; j <= 10; j += 4) for (int i = -10; i <= 10; i += 4) {
    const int local_x = x + i, local_y = y + j;
    const int moving = previous.At(local_x + best_x, local_y + best_y);
    const int trail = previous.At(local_x, local_y) - moving;
    const int error = current.At(local_x, local_y) - moving;
    original += error * error;
    secondary += trail * trail;
    overlap += error * trail;
  }
  if (secondary < 36 * 14 * 14 || original < 36 * 4 * 4) return false;
  const double alpha = double(overlap) / secondary;
  const double remaining = original - double(overlap) * overlap / secondary;
  return alpha >= 0.12 && alpha <= 0.48 && remaining <= original * 0.48 &&
         remaining <= 36 * 12 * 12;
}

class QualityHistory {
 public:
  void InspectFrame(const Frame& frame, Findings& info) {
    InspectSharpness(frame, info);
    MotionImage current(frame);
    const bool continuous = previous_ && current.segment == previous_->segment &&
        current.width == previous_->width && current.height == previous_->height &&
        current.time > previous_->time && current.time - previous_->time <= 360;
    unsigned difference = 0;
    if (continuous) {
      for (size_t i = 0; i < current.luma.size(); i += 8) {
        difference += std::abs(int(current.luma[i]) - int(previous_->luma[i]));
      }
    }
    const bool same_scene = continuous && difference < current.luma.size() / 8 * 27;
    if (!same_scene) sharpness_.clear();
    if (info.edge_samples >= 120) {
      double reference = 0;
      if (!sharpness_.empty()) {
        std::vector<double> baseline(sharpness_.begin(), sharpness_.end());
        std::nth_element(baseline.begin(), baseline.begin() + baseline.size() / 2, baseline.end());
        reference = baseline[baseline.size() / 2];
      }
      info.blur = same_scene && (info.sharpness < 0.17 ||
          (reference > 0.22 && info.sharpness < 0.27 && info.sharpness < 0.73 * reference));
      if (!info.blur) {
        sharpness_.push_back(info.sharpness);
        if (sharpness_.size() > 12) sharpness_.pop_front();
      }
    }
    if (same_scene && difference >= current.luma.size() / 8) {
      for (int y = 35; y <= 143; y += 18) for (int x = 40; x <= 280; x += 24) {
        info.ghost_tiles += HasFadingTrail(current, *previous_, x, y);
      }
      info.ghosting = info.ghost_tiles >= 2;
    }
    previous_ = std::move(current);
  }

 private:
  std::optional<MotionImage> previous_;
  std::deque<double> sharpness_;
};

Findings Inspect(const Frame& frame) {
  constexpr int columns = 64, rows = 36;
  bool dark[rows][columns]{};
  unsigned green_tiles[6][8]{};
  unsigned green = 0;
  unsigned sampled = 0;
  for (int y = 3; y < rows - 3; ++y) {
    for (int x = 3; x < columns - 3; ++x) {
      const auto px = frame.pixels[(2 * y + 1) * frame.height / (2 * rows) * frame.width +
                                   (2 * x + 1) * frame.width / (2 * columns)];
      const unsigned r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255;
      dark[y][x] = std::max({r, g, b}) < 30;
      if (g >= 24 && g >= std::max(r, b) + 16 && 5 * g >= 6 * std::max(r, b)) {
        ++green;
        ++green_tiles[y / 6][x / 8];
      }
      ++sampled;
    }
  }

  unsigned green_region = 0;
  for (const auto& row : green_tiles) {
    for (const auto count : row) green_region = std::max(green_region, count);
  }
  const unsigned green_percent = 100 * green / sampled;
  Findings result{green_percent, green_percent >= 25 || (green_percent >= 5 && green_region >= 12),
                  false};
  for (const int size : {3, 5, 7, 9}) {
    for (int y = 5; y + size <= rows - 5; ++y) {
      for (int x = 5; x + size <= columns - 5; ++x) {
        if (!dark[y][x] || !dark[y + size - 1][x + size - 1]) continue;
        int interior = 0, perimeter = 0;
        for (int i = 0; i < size; ++i) {
          for (int j = 0; j < size; ++j) interior += dark[y + i][x + j];
          perimeter += dark[y - 1][x + i] + dark[y + size][x + i] +
                       dark[y + i][x - 1] + dark[y + i][x + size];
        }
        if (10 * interior >= 9 * size * size && perimeter <= size) {
          result.dark_square = true;
          return result;
        }
      }
    }
  }
  return result;
}

class WindowCapture {
 public:
  explicit WindowCapture(HWND window) : window_(window) {
    if (!capture::GraphicsCaptureSession::IsSupported()) {
      throw std::runtime_error("Window capture is unavailable on this Windows installation.");
    }
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device_.put(),
        nullptr, context_.put()));
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
        device_.as<IDXGIDevice>().get(), inspectable.put()));
    graphics_device_ = inspectable.as<IDirect3DDevice>();
    auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem,
                                                 IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(window,
        winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item_)));
    size_ = item_.Size();
    pool_ = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
        graphics_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
    session_ = pool_.CreateCaptureSession(item_);
    try {
      session_.IsCursorCaptureEnabled(false);
    } catch (const winrt::hresult_no_interface&) {
      // Older Windows Graphics Capture versions lack the cursor property.
    }
    const auto ready = std::make_shared<winrt::handle>(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!*ready) winrt::throw_last_error();
    frame_arrived_ = pool_.FrameArrived(winrt::auto_revoke,
        [ready](const auto&, const auto&) { SetEvent(ready->get()); });
    session_.StartCapture();
    worker_ = std::jthread([this, ready](std::stop_token stop) {
      try {
        winrt::init_apartment();
        ReadFrames(stop, ready->get());
      } catch (...) {
        std::lock_guard lock(mutex_);
        failure_ = std::current_exception();
      }
    });
  }

  CaptureBatch Take() {
    std::lock_guard lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    return std::exchange(batch_, {});
  }

  void RequestSnapshot() { snapshot_requested_ = true; }

 private:
  void ReadFrames(std::stop_token stop, HANDLE ready) {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    const auto epoch_offset = EpochMilliseconds() -
        static_cast<int64_t>(1000.0 * counter.QuadPart / frequency.QuadPart);
    bool active = false;
    unsigned segment = 0;
    int64_t next_image = 0;
    while (!stop.stop_requested()) {
      WaitForSingleObject(ready, 50);
      const bool foreground = GetForegroundWindow() == window_;
      if (foreground != active) {
        active = foreground;
        ++segment;
        next_image = 0;
      }
      while (!stop.stop_requested()) {
        auto captured = pool_.TryGetNextFrame();
        if (!captured) break;
        const auto size = captured.ContentSize();
        if (size.Width < 2 || size.Height < 2) continue;
        if (size.Width != size_.Width || size.Height != size_.Height) {
          captured.Close();
          size_ = size;
          pool_.Recreate(graphics_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
          staging_ = nullptr;
          ++segment;
          next_image = 0;
          continue;
        }
        const auto stamp = captured.SystemRelativeTime().count();
        if (stamp <= 0) continue;
        const bool requested = snapshot_requested_.exchange(false);
        if (!active && !requested) continue;
        const auto unix_ms = epoch_offset + stamp / 10000;
        Snapshot image;
        if (requested || stamp >= next_image) {
          next_image = stamp + 2000000;  // 200 ms. Keep expensive pixel readbacks at about 5 Hz.
          image = ReadImage(captured, size, unix_ms, segment);
        }
        std::lock_guard lock(mutex_);
        if (active) batch_.ticks.push_back({stamp, unix_ms, segment});
        if (batch_.ticks.size() > 1024) batch_.ticks.pop_front();
        if (image) {
          batch_.images.push_back(std::move(image));
          if (batch_.images.size() > 3) batch_.images.pop_front();
        }
      }
    }
  }

  Snapshot ReadImage(const capture::Direct3D11CaptureFrame& captured,
                     winrt::Windows::Graphics::SizeInt32 size, int64_t unix_ms, unsigned segment) {
    auto access = captured.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> source;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()));
    if (!staging_) {
      D3D11_TEXTURE2D_DESC desc{};
      source->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, staging_.put()));
    }
    auto frame = std::make_shared<Frame>();
    frame->width = static_cast<uint32_t>(size.Width);
    frame->height = static_cast<uint32_t>(size.Height);
    frame->time = unix_ms;
    frame->segment = segment;
    frame->pixels.resize(frame->width * frame->height);

    context_->CopyResource(staging_.get(), source.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (uint32_t y = 0; y < frame->height; ++y) {
      const auto* row = reinterpret_cast<const uint32_t*>(
          static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
      std::transform(row, row + frame->width, frame->pixels.begin() + size_t(y) * frame->width,
                     [](uint32_t pixel) { return pixel | 0xFF000000; });
    }
    context_->Unmap(staging_.get(), 0);
    return frame;
  }

  HWND window_;
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<ID3D11Texture2D> staging_;
  IDirect3DDevice graphics_device_{nullptr};
  capture::GraphicsCaptureItem item_{nullptr};
  capture::Direct3D11CaptureFramePool pool_{nullptr};
  capture::GraphicsCaptureSession session_{nullptr};
  winrt::Windows::Graphics::SizeInt32 size_{};
  std::mutex mutex_;
  std::atomic_bool snapshot_requested_{false};
  CaptureBatch batch_;
  std::exception_ptr failure_;
  capture::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived_;
  std::jthread worker_;
};

void SavePng(IWICImagingFactory& factory, const fs::path& path, const Frame& frame) {
  winrt::com_ptr<IWICStream> stream;
  winrt::com_ptr<IWICBitmapEncoder> encoder;
  winrt::com_ptr<IWICBitmapFrameEncode> image;
  winrt::check_hresult(factory.CreateStream(stream.put()));
  winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  winrt::check_hresult(factory.CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
  winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
  winrt::check_hresult(encoder->CreateNewFrame(image.put(), nullptr));
  winrt::check_hresult(image->Initialize(nullptr));
  winrt::check_hresult(image->SetSize(frame.width, frame.height));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  winrt::check_hresult(image->SetPixelFormat(&format));
  if (format != GUID_WICPixelFormat32bppBGRA) {
    throw std::runtime_error("Could not preserve picture colors.");
  }
  winrt::check_hresult(image->WritePixels(frame.height, frame.width * sizeof(uint32_t),
      static_cast<UINT>(frame.pixels.size() * sizeof(uint32_t)),
      reinterpret_cast<BYTE*>(const_cast<uint32_t*>(frame.pixels.data()))));
  winrt::check_hresult(image->Commit());
  winrt::check_hresult(encoder->Commit());
}

class ScreenshotWriter {
 public:
  ScreenshotWriter() : worker_([this] { Run(); }) {}
  ~ScreenshotWriter() { Finish(); }

  bool Enqueue(const fs::path& path, Snapshot frame, bool required) {
    const size_t size = frame->pixels.size() * sizeof(uint32_t);
    std::unique_lock lock(mutex_);
    const auto room = [&] {
      return failed_ || pending_bytes_ == 0 ||
          (size <= kCapacity && pending_bytes_ <= kCapacity - size);
    };
    // Optional automatic captures can yield to the queue. Retain manual, crash, and fallback evidence.
    if (required) ready_.wait(lock, room);
    if (!room() || failed_) return false;
    jobs_.push_back({path, std::move(frame), size});
    pending_bytes_ += size;
    lock.unlock();
    ready_.notify_all();
    return true;
  }

  void Finish() {
    if (!worker_.joinable()) return;
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
    }
    ready_.notify_all();
    worker_.join();
  }

 private:
  struct Job {
    fs::path path;
    Snapshot frame;
    size_t size;
  };
  static constexpr size_t kCapacity = 256 * 1024 * 1024;

  void Run() noexcept {
    try {
      winrt::init_apartment();
      auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
      for (;;) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return closing_ || !jobs_.empty(); });
        if (jobs_.empty()) break;
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        lock.unlock();

        auto temp = job.path;
        temp += L".tmp";
        try {
          SavePng(*factory, temp, *job.frame);
          if (!MoveFileExW(temp.c_str(), job.path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            winrt::throw_last_error();
          }
        } catch (...) {
          std::error_code error;
          fs::remove(temp, error);
          std::fwprintf(stderr, L"Could not write screenshot %ls.\n", job.path.c_str());
        }
        job.frame.reset();
        lock.lock();
        pending_bytes_ -= job.size;
        lock.unlock();
        ready_.notify_all();
      }
      factory = nullptr;
      winrt::uninit_apartment();
    } catch (...) {
      std::fwprintf(stderr, L"Screenshot worker stopped. Screenshot files may be missing.\n");
      {
        std::lock_guard lock(mutex_);
        failed_ = true;
        jobs_.clear();
      }
      ready_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  size_t pending_bytes_ = 0;
  bool closing_ = false, failed_ = false;
  std::thread worker_;
};

fs::path ProcessImage(HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) winrt::throw_last_error();
  path.resize(size);
  return path;
}

void CopyLogTail(const fs::path& project, const fs::path& output) {
  const auto logs = project / "out" / "logs";
  std::error_code error;
  fs::path latest;
  fs::file_time_type newest{};
  for (const auto& file : fs::directory_iterator(logs, error)) {
    if (file.path().extension() != ".log" ||
        !file.path().filename().native().starts_with(L"asuras_wrath_")) continue;
    const auto time = file.last_write_time(error);
    if (!error && (latest.empty() || time > newest)) {
      latest = file.path();
      newest = time;
    }
  }
  if (latest.empty()) return;
  std::ifstream input(latest, std::ios::binary | std::ios::ate);
  if (!input) return;
  const auto length = static_cast<std::streamoff>(input.tellg());
  input.seekg(std::max<std::streamoff>(0, length - 65536));
  std::ofstream(output / "game-log-tail.txt", std::ios::binary) << input.rdbuf();
}

struct FrameTiming {
  int64_t time;
  double milliseconds;
};

struct PacingIssue {
  const char* reason = nullptr;
  double baseline_ms = 0;
  double measured_ms = 0;
};

class FramePacing {
 public:
  std::optional<FrameTiming> Add(CaptureTick tick) {
    if (!previous_ || previous_->segment != tick.segment) {
      previous_ = tick;
      history_.clear();
      return {};
    }
    if (tick.compositor_time <= previous_->compositor_time) return {};
    const FrameTiming sample{tick.unix_ms,
                             (tick.compositor_time - previous_->compositor_time) / 10000.0};
    history_.push_back(sample);
    while (previous_->unix_ms - history_.front().time > 2500 || history_.size() > 1024) {
      history_.pop_front();
    }
    previous_ = tick;
    return sample;
  }

  PacingIssue Check() const {
    if (history_.size() < 9) return {};
    const auto& current = history_.back();
    if (current.time - history_.front().time < 300) return {};

    const auto preceding = history_.rbegin() + 1;
    const double baseline = Median(preceding, history_.rend());
    if (current.milliseconds >= std::max(24.0, baseline * 2.25)) {
      return {"possible-frame-stutter", baseline, current.milliseconds};
    }

    const auto older = std::ranges::find_if(history_.rbegin(), history_.rend(),
        [&](const FrameTiming& sample) { return current.time - sample.time >= 450; });
    if (history_.rend() - older < 8) return {};
    const double old_baseline = Median(older, history_.rend());
    double duration = 0;
    unsigned slow = 0;
    for (auto sample = history_.rbegin(); sample != older; ++sample) {
      duration += sample->milliseconds;
      if (sample->milliseconds >= old_baseline * 1.3) ++slow;
    }
    const auto count = older - history_.rbegin();
    if (slow >= 5 && duration >= 350 && duration / count >= old_baseline * 1.5) {
      return {"possible-fps-drop", old_baseline, duration / count};
    }
    return {};
  }

  const std::deque<FrameTiming>& history() const { return history_; }

 private:
  using Iterator = std::deque<FrameTiming>::const_reverse_iterator;

  static double Median(Iterator sample, Iterator end) {
    std::array<double, 32> intervals;
    const auto count = std::min(end - sample, static_cast<std::ptrdiff_t>(intervals.size()));
    std::transform(sample, sample + count, intervals.begin(),
                   [](const FrameTiming& frame) { return frame.milliseconds; });
    // Average the middle pair so a regular alternating cadence stays stable.
    std::sort(intervals.begin(), intervals.begin() + count);
    return (intervals[(count - 1) / 2] + intervals[count / 2]) / 2.0;
  }

  std::optional<CaptureTick> previous_;
  std::deque<FrameTiming> history_;
};

struct Mark {
  const char* reason;
  int64_t time;
  DWORD exit_code;
  Clock::time_point finish;
  Findings findings;
  std::vector<Snapshot> frames;
  std::vector<FrameTiming> timings;
  PacingIssue pacing;
};

class MarkKey {
 public:
  explicit MarkKey(HWND game) {
    game_ = game;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, Observe, GetModuleHandleW(nullptr), 0);
    if (!hook_) winrt::throw_last_error();
  }
  ~MarkKey() { UnhookWindowsHookEx(hook_); }

  bool TakeMark() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    return std::exchange(marked_, false);
  }

 private:
  static LRESULT CALLBACK Observe(int code, WPARAM event, LPARAM input) {
    if (code == HC_ACTION && reinterpret_cast<const KBDLLHOOKSTRUCT*>(input)->vkCode == 'L') {
      if (event == WM_KEYUP || event == WM_SYSKEYUP) {
        held_ = false;
      } else if (GetForegroundWindow() == game_ && !held_ &&
                 (event == WM_KEYDOWN || event == WM_SYSKEYDOWN)) {
        held_ = true;
        marked_ = true;
      }
    }
    return CallNextHookEx(nullptr, code, event, input);
  }

  HHOOK hook_ = nullptr;
  inline static HWND game_ = nullptr;
  inline static bool held_ = false;
  inline static bool marked_ = false;
};

void SaveMark(ScreenshotWriter& screenshots, const fs::path& project, const fs::path& session,
              const Mark& mark, uint32_t number, DWORD pid) {
  const auto output = session / (std::to_string(number) + "-" + mark.reason);
  fs::create_directories(output);
  std::ofstream info(output / "event.txt");
  info << "event=" << mark.reason << "\nmark_unix_ms=" << mark.time << "\npid=" << pid
       << "\ngame_exit_code=" << mark.exit_code << "\ngreen_percent=" << mark.findings.green_percent
       << "\ngreen_tint_candidate=" << mark.findings.green_tint
       << "\ndark_square_candidate=" << mark.findings.dark_square
       << "\nblur_candidate=" << mark.findings.blur
       << "\nghosting_candidate=" << mark.findings.ghosting
       << "\nsharpness_ratio=" << mark.findings.sharpness
       << "\nedge_samples=" << mark.findings.edge_samples
       << "\nghost_trail_tiles=" << mark.findings.ghost_tiles << "\n";
  if (!mark.timings.empty()) {
    info << "frame_timing_source=windows_graphics_capture_compositor\n";
    std::ofstream timings(output / "frame-times.csv");
    timings << "unix_ms,relative_to_mark_ms,frame_time_ms\n";
    for (const auto& sample : mark.timings) {
      timings << sample.time << ',' << sample.time - mark.time << ',' << sample.milliseconds << '\n';
    }
  }
  if (mark.pacing.reason) {
    info << "baseline_frame_time_ms=" << mark.pacing.baseline_ms
         << "\nobserved_frame_time_ms=" << mark.pacing.measured_ms << '\n';
  }
  std::error_code error;
  fs::copy_file(project / "out" / "launcher-settings.json", output / "launcher-settings.json", error);
  CopyLogTail(project, output);
  std::ofstream timeline(output / "frames.csv");
  timeline << "image,unix_ms,relative_to_mark_ms,width,height\n";
  const std::string_view reason(mark.reason);
  const bool required = reason == "manual" || reason == "game-exit";
  size_t skipped = 0;
  for (size_t i = 0; i < mark.frames.size(); ++i) {
    const auto name = std::to_string(1000 + i).substr(1) + ".png";
    const Frame& frame = *mark.frames[i];
    if (!screenshots.Enqueue(output / name, mark.frames[i], required)) {
      ++skipped;
      continue;
    }
    timeline << name << ',' << frame.time << ',' << frame.time - mark.time << ','
             << frame.width << ',' << frame.height << '\n';
  }
  if (skipped) info << "screenshots_not_queued=" << skipped << '\n';
  std::wprintf(L"Saved %ls\n", output.c_str());
  std::fflush(stdout);
}

struct FallbackIncident {
  events::Record first, last;
  std::vector<events::Cause> causes;
  Clock::time_point capture_until;
  fs::path output;
  Snapshot screenshot;
  bool extra_causes = false;
};

class FallbackMonitor {
 public:
  explicit FallbackMonitor(fs::path source) : source_(std::move(source)) {}

  bool Read() {
    if (source_.empty()) return false;
    if (!input_.is_open()) input_.open(source_, std::ios::binary);
    if (!input_.is_open()) return false;
    input_.clear();
    input_.seekg(offset_);
    bool request_screenshot = false;
    std::string line;
    while (std::getline(input_, line) && !input_.eof()) {
      offset_ = input_.tellg();
      if (line.size() > 1024) continue;
      const auto event = events::Parse(line);
      if (!event) continue;
      if (event->end) {
        Close();
        continue;
      }
      if (active_ && event->frame == active_->last.frame &&
          event->provider == active_->last.provider) continue;
      if (active_ && (event->frame != active_->last.frame + 1 ||
                      event->provider != active_->last.provider)) Close();
      if (!active_) {
        active_ = FallbackIncident{*event, *event, {event->cause}, Clock::now() + 550ms};
        request_screenshot = true;
      } else {
        active_->last = *event;
        auto& causes = active_->causes;
        if (std::find(causes.begin(), causes.end(), event->cause) == causes.end()) {
          if (causes.size() < 24) causes.push_back(event->cause);
          else active_->extra_causes = true;
        }
      }
    }
    return request_screenshot;
  }

  void Publish(ScreenshotWriter& screenshots, const fs::path& project, const fs::path& session,
               const std::deque<Snapshot>& images, uint32_t& written, DWORD pid, bool final) {
    if (final) Close();
    const auto now = Clock::now();
    if (active_ && active_->output.empty() && now >= active_->capture_until) {
      Save(screenshots, project, session, images, written, pid, *active_, true);
    }
    for (auto it = ended_.begin(); it != ended_.end();) {
      if (!final && it->output.empty() && now < it->capture_until) {
        ++it;
        continue;
      }
      Save(screenshots, project, session, images, written, pid, *it, false);
      it = ended_.erase(it);
    }
  }

 private:
  void Close() {
    if (!active_) return;
    ended_.push_back(std::move(*active_));
    active_.reset();
  }

  void Save(ScreenshotWriter& screenshots, const fs::path& project, const fs::path& session,
            const std::deque<Snapshot>& images, uint32_t& written, DWORD pid,
            FallbackIncident& incident, bool ongoing) const {
    const bool first_write = incident.output.empty();
    if (first_write) {
      incident.output = session / (std::to_string(++written) + "-upscaler-fallback");
      fs::create_directories(incident.output);
      // Prefer the first nearby compositor image after the fallback. A preceding
      // game image is useful when Windows stops providing frames during the incident.
      int64_t best_distance = 1501;
      bool best_follows = false;
      for (const auto& image : images) {
        const bool follows = image->time >= incident.first.time;
        const auto distance = std::max(image->time, incident.first.time) -
                              std::min(image->time, incident.first.time);
        if (distance > 1500 || (incident.screenshot &&
            (follows == best_follows ? distance >= best_distance : best_follows))) continue;
        incident.screenshot = image;
        best_distance = distance;
        best_follows = follows;
      }
      if (incident.screenshot &&
          !screenshots.Enqueue(incident.output / "screenshot.png", incident.screenshot, true)) {
        incident.screenshot.reset();
      }
      CopyLogTail(project, incident.output);
      std::error_code error;
      fs::copy_file(project / "out" / "launcher-settings.json",
                    incident.output / "launcher-settings.json", error);
    }
    const auto& first = incident.first;
    const auto& last = incident.last;
    std::ofstream info(incident.output / "event.txt");
    info << "event=upscaler-fallback\npid=" << pid << "\nprovider=" << first.provider
         << "\nfirst_frame=" << first.frame << "\nlast_frame=" << last.frame
         << "\nframe_count=" << last.frame - first.frame + 1
         << "\nmark_unix_ms=" << first.time << "\nlast_unix_ms=" << last.time
         << "\nreason=" << first.cause.reason << "\nvertex_shader=" << std::hex
         << first.cause.draw.vertex << "\npixel_shader=" << first.cause.draw.pixel << std::dec
         << "\nstatus=" << (ongoing ? "ongoing" : "complete")
         << "\nrenderer_log=" << source_.filename().string()
         << "\nadditional_causes_in_renderer_log=" << incident.extra_causes << '\n';
    if (incident.screenshot) {
      info << "screenshot=screenshot.png\nscreenshot_unix_ms=" << incident.screenshot->time
           << "\nscreenshot_offset_ms=" << incident.screenshot->time - first.time
           << "\nscreenshot_width=" << incident.screenshot->width
           << "\nscreenshot_height=" << incident.screenshot->height << '\n';
    } else {
      info << "screenshot=unavailable\n";
    }
    std::ofstream causes(incident.output / "upscaler-details.tsv");
    causes << "reason\tvertex_shader\tpixel_shader\tmotion_vertex_shader\tmotion_pixel_shader\tdetail\n";
    for (const auto& cause : incident.causes) {
      causes << cause.reason << '\t' << std::hex << cause.draw.vertex << '\t' << cause.draw.pixel
             << '\t' << cause.motion.vertex << '\t' << cause.motion.pixel << std::dec
             << '\t' << cause.detail << '\n';
    }
    if (first_write) {
      std::wprintf(L"Saved upscaler fallback %ls\n", incident.output.c_str());
      std::fflush(stdout);
    }
  }

  fs::path source_;
  std::ifstream input_;
  std::streamoff offset_ = 0;
  std::optional<FallbackIncident> active_;
  std::deque<FallbackIncident> ended_;
};

struct Graphics {
  std::string renderer = "unknown";
  std::string upscaler = "unknown";
};

void Collect(const fs::path& project, DWORD pid, HWND window, HANDLE process, bool temporal,
             const Graphics& graphics) {
  WindowCapture recorder(window);
  MarkKey hotkey(window);
  ScreenshotWriter screenshots;
  const auto session = project / "out" / "diagnostics" / std::to_string(EpochMilliseconds());
  fs::create_directories(session);
  {
    std::ofstream info(session / "session.txt");
    info << "renderer=" << graphics.renderer << "\nupscaler=" << graphics.upscaler
         << "\npid=" << pid << "\nsource=windows_graphics_capture_game_window\n";
  }
  std::ofstream quality_log(session / "frame-quality.csv");
  quality_log << "unix_ms,width,height,segment,sharpness_ratio,edge_samples,ghost_trail_tiles,blur_candidate,ghosting_candidate\n";
  std::wprintf(L"Press L in the game to mark an anomaly. Reports go to\n%ls\n", session.c_str());
  std::fflush(stdout);
  std::deque<Snapshot> ring;
  FallbackMonitor fallbacks(temporal ? events::File(project / "out" / "logs", pid, process)
                                     : fs::path{});
  auto next_fallback_read = Clock::now();
  FramePacing pacing;
  QualityHistory quality;
  std::vector<Mark> marks;
  uint32_t written = 0;
  unsigned automatic = 0;
  unsigned automatic_timing = 0;
  unsigned suspect_streak = 0;
  unsigned blur_streak = 0, ghost_streak = 0;
  unsigned blur_reports = 0, ghost_reports = 0;
  Findings last{};
  auto next_visual = Clock::now();
  auto next_blur = next_visual, next_ghost = next_visual;
  auto next_stutter = next_visual;
  auto next_drop = next_visual;
  const auto mark = [&](const char* reason, int64_t time, DWORD code = 0, PacingIssue issue = {}) {
    const auto& timings = pacing.history();
    marks.push_back({reason, time, code, Clock::now() + 1600ms, last,
                     {ring.begin(), ring.end()}, {timings.begin(), timings.end()}, issue});
  };
  const auto read_capture = [&](bool active) {
    const auto now = Clock::now();
    auto batch = recorder.Take();
    for (const auto& frame : batch.images) {
      last = Inspect(*frame);
      quality.InspectFrame(*frame, last);
      quality_log << frame->time << ',' << frame->width << ',' << frame->height << ',' << frame->segment
                  << ',' << last.sharpness << ',' << last.edge_samples << ',' << last.ghost_tiles
                  << ',' << last.blur << ',' << last.ghosting << '\n';
      for (auto& pending : marks) {
        if (pending.frames.size() < 7) pending.frames.push_back(frame);
        else pending.frames.back() = frame;
      }
      ring.push_back(frame);
      if (ring.size() > 4) ring.pop_front();
      const bool suspect = last.green_tint || last.dark_square;
      suspect_streak = suspect ? suspect_streak + 1 : 0;
      if (active && suspect_streak >= 2 && now >= next_visual && automatic < 12) {
        mark(last.green_tint ? "possible-green-tint" : "possible-dark-square", frame->time);
        ++automatic;
        next_visual = now + 25s;
      }
      blur_streak = last.blur ? blur_streak + 1 : 0;
      ghost_streak = last.ghosting ? ghost_streak + 1 : 0;
      if (active && blur_streak >= 2 && now >= next_blur && blur_reports < 8) {
        mark("possible-blur", frame->time);
        ++blur_reports;
        next_blur = now + 25s;
      }
      if (active && ghost_streak >= 2 && now >= next_ghost && ghost_reports < 8) {
        mark("possible-ghosting", frame->time);
        ++ghost_reports;
        next_ghost = now + 25s;
      }
    }
    for (const auto tick : batch.ticks) {
      const auto sample = pacing.Add(tick);
      if (!sample) continue;
      for (auto& pending : marks) pending.timings.push_back(*sample);
      if (!active || automatic_timing >= 24) continue;
      const auto issue = pacing.Check();
      if (!issue.reason) continue;
      auto& next = std::string_view(issue.reason) == "possible-frame-stutter" ? next_stutter : next_drop;
      if (now >= next) {
        mark(issue.reason, sample->time, 0, issue);
        ++automatic_timing;
        next = now + 15s;
      }
    }
    if (!active) suspect_streak = blur_streak = ghost_streak = 0;
  };
  while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && IsWindow(window)) {
    const auto now = Clock::now();
    try {
      read_capture(GetForegroundWindow() == window);
    } catch (const winrt::hresult_error&) {
      if (!IsWindow(window) || WaitForSingleObject(process, 250) == WAIT_OBJECT_0) break;
      throw;
    }
    if (hotkey.TakeMark()) {
      mark("manual", EpochMilliseconds());
      recorder.RequestSnapshot();
    }
    if (now >= next_fallback_read) {
      if (fallbacks.Read()) recorder.RequestSnapshot();
      fallbacks.Publish(screenshots, project, session, ring, written, pid, false);
      next_fallback_read = now + 50ms;
    }
    for (auto it = marks.begin(); it != marks.end();) {
      if (now < it->finish) {
        ++it;
        continue;
      }
      SaveMark(screenshots, project, session, *it, ++written, pid);
      it = marks.erase(it);
    }
    Sleep(12);
  }
  try {
    read_capture(false);
  } catch (const winrt::hresult_error&) {
    // The capture target may already have been destroyed.
  }
  if (WaitForSingleObject(process, 1000) == WAIT_OBJECT_0) {
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) && code != 0) mark("game-exit", EpochMilliseconds(), code);
  }
  fallbacks.Read();
  fallbacks.Publish(screenshots, project, session, ring, written, pid, true);
  for (const auto& pending : marks) SaveMark(screenshots, project, session, pending, ++written, pid);
}

void SelfTest(const fs::path& output) {
  FramePacing pacing;
  CaptureTick tick{10000000, 1000, 1};
  (void)pacing.Add(tick);
  const auto advance = [&](int64_t interval) {
    tick.compositor_time += interval;
    tick.unix_ms = tick.compositor_time / 10000;
    (void)pacing.Add(tick);
    return pacing.Check();
  };
  for (const int64_t interval : {333333, 166667, 83333, 69444}) {
    ++tick.segment;
    (void)pacing.Add(tick);
    for (int i = 0; i < 160; ++i) {
      if (advance(interval).reason) throw std::runtime_error("Steady cadence was flagged.");
    }
  }
  const auto hitch = advance(750000);
  if (std::string_view(hitch.reason ? hitch.reason : "") != "possible-frame-stutter" ||
      hitch.measured_ms != 75.0) {
    throw std::runtime_error("An isolated 75 ms hitch was missed.");
  }
  if (!advance(35000000).reason) throw std::runtime_error("A long freeze was missed.");
  ++tick.segment;
  if (advance(50000000).reason) throw std::runtime_error("A focus gap was flagged.");
  for (const int64_t interval : {333333, 83333}) {
    ++tick.segment;
    (void)pacing.Add(tick);
    for (int i = 0; i < 160; ++i) (void)advance(interval);
    bool detected = false;
    for (int i = 0; i < 40; ++i) {
      const auto issue = advance(interval * 2);
      if (issue.reason && std::string_view(issue.reason) == "possible-fps-drop") detected = true;
    }
    if (!detected) throw std::runtime_error("A sustained frame rate drop was missed.");
  }

  Frame frame{128, 72, EpochMilliseconds(), std::vector<uint32_t>(128 * 72, 0xFF786464)};
  const auto normal = Inspect(frame);
  if (normal.green_tint || normal.dark_square) throw std::runtime_error("Baseline false alarm.");
  for (size_t y = 28; y < 38; ++y) {
    std::fill_n(frame.pixels.begin() + y * frame.width + 48, 10, 0xFF080808);
  }
  if (!Inspect(frame).dark_square) throw std::runtime_error("Dark square was missed.");
  std::fill(frame.pixels.begin(), frame.pixels.end(), 0xFF786464);
  for (size_t y = 24; y < 48; ++y) {
    std::fill_n(frame.pixels.begin() + y * frame.width + 48, 16, 0xFF205F20);
  }
  const auto local_green = Inspect(frame);
  if (!local_green.green_tint || local_green.green_percent >= 25) {
    throw std::runtime_error("A concentrated green patch was missed.");
  }
  std::fill(frame.pixels.begin(), frame.pixels.end(), 0xFF205F20);
  if (Inspect(frame).green_percent != 100) throw std::runtime_error("Green tint was missed.");

  const auto gray = [](uint32_t value) { return 0xFF000000u | value * 0x010101u; };
  Frame detailed{2560, 1440, 1000, std::vector<uint32_t>(2560 * 1440)};
  Frame blurred = detailed;
  blurred.time += 200;
  const auto stripes = [](int x) { return (std::max(0, x) / 56) % 2 ? 220u : 36u; };
  for (int x = 0; x < static_cast<int>(detailed.width); ++x) {
    unsigned softened = 0;
    for (int dx = -9; dx <= 9; ++dx) softened += stripes(x + dx);
    for (uint32_t y = 0; y < detailed.height; ++y) {
      detailed.pixels[y * detailed.width + x] = gray(stripes(x));
      blurred.pixels[y * detailed.width + x] = gray(softened / 19);
    }
  }
  QualityHistory blur_check;
  Findings sharp_score{}, blurred_score{};
  blur_check.InspectFrame(detailed, sharp_score);
  blur_check.InspectFrame(blurred, blurred_score);
  if (sharp_score.blur || sharp_score.ghosting || !blurred_score.blur ||
      sharp_score.sharpness <= blurred_score.sharpness) throw std::runtime_error("Blur detection did not verify.");

  Frame before{320, 180, 1000, std::vector<uint32_t>(320 * 180)};
  for (int y = 0; y < int(before.height); ++y) for (int x = 0; x < int(before.width); ++x) {
    const unsigned value = (x >= 48 && x < 272 && y >= 42 && y < 140) ?
        44 + ((x / 5 * 19 + y / 5 * 67 + (x / 5 * y / 5) * 13) % 171) : 73;
    before.pixels[y * before.width + x] = gray(value);
  }
  Frame moving = before, trailed = before;
  moving.time = trailed.time = before.time + 200;
  for (int y = 0; y < int(before.height); ++y) for (int x = 0; x < int(before.width); ++x) {
    const auto previous = Luma(before.pixels[y * before.width + x]);
    const auto translated = Luma(before.pixels[y * before.width + std::max(0, x - 9)]);
    moving.pixels[y * before.width + x] = gray(translated);
    trailed.pixels[y * before.width + x] = gray((3 * translated + previous) / 4);
  }
  QualityHistory motion_check, trail_check;
  Findings prior_motion{}, prior_trail{}, clear_motion{}, visible_trail{};
  motion_check.InspectFrame(before, prior_motion);
  motion_check.InspectFrame(moving, clear_motion);
  trail_check.InspectFrame(before, prior_trail);
  trail_check.InspectFrame(trailed, visible_trail);
  if (clear_motion.ghosting || !visible_trail.ghosting)
    throw std::runtime_error("Motion and ghosting were not distinguished.");
  QualityHistory focus_check;
  Findings focus_prior{}, focus_after{};
  focus_check.InspectFrame(before, focus_prior);
  trailed.segment++;
  focus_check.InspectFrame(trailed, focus_after);
  if (focus_after.ghosting || focus_after.blur) throw std::runtime_error("A focus gap was flagged as a visual issue.");

  fs::create_directories(output);
  ScreenshotWriter screenshots;
  auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
  const auto full_image = output / "full-resolution.png";
  if (!screenshots.Enqueue(full_image, std::make_shared<Frame>(detailed), true)) {
    throw std::runtime_error("Could not queue full-resolution screenshot.");
  }
  Mark test{"manual", frame.time, 0, Clock::now(), Inspect(frame),
            {std::make_shared<Frame>(std::move(frame))}, {{frame.time, 75.0}}, hitch};
  SaveMark(screenshots, output, output, test, 1, 0);

  events::SceneEvidence scene{};
  scene.recent_gameplay = true;
  scene.compositor = true;
  scene.compatible_frontbuffer = true;
  if (!events::FallbackReason(scene).empty()) throw std::runtime_error("A loading card was flagged.");
  scene.menu = true;
  scene.depth = true;
  scene.hud_failure = "unsupported-hud-draw";
  if (!events::FallbackReason(scene).empty()) throw std::runtime_error("A menu was flagged.");
  scene.menu = false;
  if (events::FallbackReason(scene) != "unsupported-hud-draw") {
    throw std::runtime_error("The blocking HUD shader was missed.");
  }
  scene.hud_failure = {};
  if (events::FallbackReason(scene) != "missing-camera-matrix") {
    throw std::runtime_error("A 3D frame without a camera was missed.");
  }

  const auto event_file = output / "upscaler-test.tsv";
  FallbackMonitor monitor(event_file);
  if (monitor.Read()) throw std::runtime_error("A missing trace produced a report.");
  {
    std::ofstream stream(event_file, std::ios::binary);
    stream << events::kColumns << '\n';
    events::Write(stream, false, 140, "fsr3", "missing-camera-matrix", {0xabc, 0xdef}, {0x123, 0x456});
    stream << "fallback\t141";
  }
  if (!monitor.Read()) throw std::runtime_error("The fallback did not request a screenshot.");
  {
    std::ofstream stream(event_file, std::ios::binary | std::ios::app);
    stream << '\t' << EpochMilliseconds() << "\tfsr3\tmissing-scene-color\t15\t16\t0\t0\t\n";
    events::Write(stream, true, 142, "fsr3");
    events::Write(stream, false, 190, "dlss", "upscaler-runtime-disabled", {}, {}, "device\tlost");
  }
  if (!monitor.Read()) throw std::runtime_error("The DLSS fallback was missed.");
  uint32_t written = 1;
  auto screenshot = std::make_shared<Frame>(*test.frames.front());
  screenshot->time = EpochMilliseconds();
  monitor.Publish(screenshots, output, output, {screenshot}, written, 22, true);
  screenshots.Finish();
  winrt::com_ptr<IWICBitmapDecoder> decoder;
  winrt::com_ptr<IWICBitmapFrameDecode> decoded;
  winrt::check_hresult(factory->CreateDecoderFromFilename(full_image.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnDemand, decoder.put()));
  winrt::check_hresult(decoder->GetFrame(0, decoded.put()));
  UINT decoded_width = 0, decoded_height = 0;
  winrt::check_hresult(decoded->GetSize(&decoded_width, &decoded_height));
  if (decoded_width != detailed.width || decoded_height != detailed.height ||
      !fs::is_regular_file(output / "1-manual" / "000.png")) {
    throw std::runtime_error("A queued screenshot was resized or lost.");
  }
  const auto first_report = output / "2-upscaler-fallback";
  const auto second_report = output / "3-upscaler-fallback";
  const auto read_file = [](const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
  if (written != 3 || !fs::is_regular_file(first_report / "screenshot.png") ||
      read_file(first_report / "event.txt").find("last_frame=141") == std::string::npos ||
      read_file(first_report / "upscaler-details.tsv").find("missing-scene-color\t15\t16") ==
          std::string::npos ||
      read_file(second_report / "event.txt").find("provider=dlss") == std::string::npos ||
      read_file(second_report / "upscaler-details.tsv").find("device lost") == std::string::npos) {
    throw std::runtime_error("Fallback grouping, details, or screenshot writing did not verify.");
  }
  std::puts("PASS blur, ghosting, 2560x1440 PNG, frame pacing, and DLSS/FSR fallback reports.");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    winrt::init_apartment();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--self-test") {
      SelfTest(argv[2]);
      return 0;
    }
    if (argc != 4 && argc != 6) throw std::runtime_error("Open the game and run Collect-Gameplay.cmd.");
    Graphics graphics;
    if (argc == 6) {
      const std::wstring_view renderer(argv[4]), provider(argv[5]);
      if (renderer != L"vulkan" && renderer != L"d3d12")
        throw std::invalid_argument("Unrecognized renderer for the collector.");
      if (provider != L"native" && provider != L"dlss" && provider != L"fsr3")
        throw std::invalid_argument("Unrecognized upscaler for the collector.");
      graphics.renderer.assign(renderer.begin(), renderer.end());
      graphics.upscaler.assign(provider.begin(), provider.end());
    }
    const fs::path project = argv[1];
    const DWORD pid = std::stoul(argv[2]);
    const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(argv[3])));
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    winrt::handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (owner != pid || !process) {
      throw std::runtime_error("The selected window does not belong to this recomp.");
    }
    const auto executable = ProcessImage(process.get());
    std::error_code identity_error;
    const auto out = project / "out";
    const bool temporal = fs::equivalent(executable, out / "dlss-runtime" / "game" / "asuras_wrath.exe",
                                         identity_error) ||
                          fs::equivalent(executable, out / "vulkan-upscalers" / "game" / "asuras_wrath.exe",
                                         identity_error);
    if (!temporal && !fs::equivalent(executable, out / "build" / "win-amd64" / "Release" /
                                    "asuras_wrath.exe", identity_error)) {
      throw std::runtime_error("The selected window does not belong to this recomp.");
    }
    const auto mutex_name = L"Local\\AsurasWrathGameplayCollector-" + std::to_wstring(pid);
    const HANDLE mutex_handle = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    const auto mutex_error = GetLastError();
    winrt::handle session_mutex(mutex_handle);
    if (!session_mutex) winrt::throw_hresult(HRESULT_FROM_WIN32(mutex_error));
    if (mutex_error == ERROR_ALREADY_EXISTS) {
      throw std::runtime_error("The gameplay collector is already running for this session.");
    }
    Collect(project, pid, window, process.get(), temporal, graphics);
    return 0;
  } catch (const winrt::hresult_error& error) {
    std::fwprintf(stderr, L"Collector error %ls\n", error.message().c_str());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Collector error %s\n", error.what());
  }
  return 1;
}
