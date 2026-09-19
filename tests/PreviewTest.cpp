// The settings-dialog preview, through the real pipeline.
//
// Serves its own source rather than previewing whatever is on the network: the
// other tests' senders come and go, and a preview of one that stops mid-run
// fails for reasons that have nothing to do with the preview.
//
// Usage: PreviewTest [--source=NAME] [--save=PNG] [--dialog-shot=PNG]

#include <Gfx/Settings/Model.hpp>
#include <Gfx/Widgets/CameraPreviewWidget.hpp>

#include <Ndi/InputFactory.hpp>
#include <Ndi/InputStream.hpp>
#include <Ndi/Loader.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QCoreApplication>
#include <QPixmap>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "StubApplication.hpp"

namespace
{
int g_fail = 0;
void check(bool ok, const std::string& what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if(!ok)
    ++g_fail;
}

void pump(int ms)
{
  QElapsedTimer t;
  t.start();
  while(t.elapsed() < ms)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(5);
  }
}

// 75% SMPTE bars in UYVY, Rec.709 limited range.
std::vector<uint8_t> bars(int w, int h)
{
  static const int rgb[7][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191},
                                {0, 191, 0},     {191, 0, 191}, {191, 0, 0},
                                {0, 0, 191}};
  std::vector<uint8_t> buf(size_t(2 * w) * h);
  for(int y = 0; y < h; y++)
  {
    uint8_t* row = buf.data() + size_t(y) * 2 * w;
    for(int x = 0; x < w; x += 2)
    {
      auto yuv = [&](int px) {
        const int b = std::min(px * 7 / w, 6);
        const double r = rgb[b][0], g = rgb[b][1], bl = rgb[b][2];
        const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * bl;
        return std::array<double, 3>{
            16. + luma * 219. / 255., 128. + (bl - luma) / 1.8556 * 224. / 255.,
            128. + (r - luma) / 1.5748 * 224. / 255.};
      };
      const auto a = yuv(x), b = yuv(x + 1);
      row[x * 2 + 0] = uint8_t((a[1] + b[1]) * 0.5 + 0.5);
      row[x * 2 + 1] = uint8_t(a[0] + 0.5);
      row[x * 2 + 2] = uint8_t((a[2] + b[2]) * 0.5 + 0.5);
      row[x * 2 + 3] = uint8_t(b[0] + 0.5);
    }
  }
  return buf;
}

// The SDK publishes a sender as "MACHINE (name)", so the name a receiver must
// connect to is only known once discovery has seen it.
QString discover(const Ndi::Loader& ndi, const QString& contains, int timeoutMs)
{
  Ndi::Finder f{ndi};
  QElapsedTimer t;
  t.start();
  while(t.elapsed() < timeoutMs)
  {
    f.wait_for_sources(250);
    uint32_t n = 0;
    auto* s = f.get_current_sources(&n);
    for(uint32_t i = 0; i < n; i++)
    {
      const auto name = QString::fromUtf8(s[i].p_ndi_name);
      if(name.contains(contains))
        return name;
    }
  }
  return {};
}

struct TestSender
{
  std::atomic_bool running{true};
  std::thread thread;

  TestSender(const Ndi::Loader& ndi, std::string name, int w = 1920, int h = 1080)
  {
    thread = std::thread{[this, &ndi, name = std::move(name), w, h] {
      Ndi::Sender send{ndi, name};
      auto buf = bars(w, h);
      NDIlib_video_frame_v2_t f{};
      f.xres = w;
      f.yres = h;
      f.FourCC = NDIlib_FourCC_video_type_UYVY;
      f.frame_rate_N = 30000;
      f.frame_rate_D = 1001;
      f.frame_format_type = NDIlib_frame_format_type_progressive;
      f.line_stride_in_bytes = 2 * w;
      f.p_data = buf.data();
      while(running.load(std::memory_order_relaxed))
        send.send_video(f);
    }};
  }

  ~TestSender()
  {
    running = false;
    if(thread.joinable())
      thread.join();
  }
};
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::string wanted, save, shot;
  for(int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if(a.rfind("--source=", 0) == 0)
      wanted = a.substr(9);
    else if(a.rfind("--save=", 0) == 0)
      save = a.substr(7);
    else if(a.rfind("--dialog-shot=", 0) == 0)
      shot = a.substr(14);
  }

  QApplication app{argc, argv};

  const auto& ndi = Ndi::Loader::instance();
  if(!ndi.available())
  {
    std::printf("no NDI runtime; skipping\n");
    return 77;
  }
  std::printf(
      "runtime: %s (graphics api %d)\n", ndi.version().c_str(),
      int(Gfx::Settings::graphicsApiForCurrentApplication()));

  const QString ownToken
      = QStringLiteral("ndi-preview-test-%1").arg(QCoreApplication::applicationPid());
  std::unique_ptr<TestSender> own;
  QString source = QString::fromStdString(wanted);
  if(wanted.empty())
  {
    own = std::make_unique<TestSender>(ndi, ownToken.toStdString());
    source = discover(ndi, ownToken, 20000);
    if(source.isEmpty())
    {
      std::printf("our own sender was not discovered; skipping\n");
      return 77;
    }
  }
  std::printf("source: %s\n\n", source.toUtf8().constData());

  Gfx::CameraPreviewWidget preview;
  preview.resize(320, 180);
  preview.show();

  std::printf("== with no input ==\n");
  check(preview.metadata() == nullptr, "no metadata before an input is set");

  auto stream = std::make_shared<Ndi::InputStream>(ndi);
  stream->load(source.toStdString());
  preview.setInput(stream);

  std::printf("\n== after connecting ==\n");
  pump(6000);

  const auto* meta = preview.metadata();
  check(meta != nullptr, "the input is exposed");
  if(meta)
  {
    std::printf(
        "     %dx%d, %.2f fps, pixel format %d\n", meta->width, meta->height,
        meta->fps, int(meta->pixel_format));
    check(meta->width > 0 && meta->height > 0, "frames arrived, so the size is known");
  }

  // The preview paints the graph's readback, so a non-uniform image means the
  // whole path ran: receive, decode on the GPU, read back, paint.
  const QImage img = preview.grab().toImage().convertToFormat(QImage::Format_RGB32);
  check(!img.isNull() && img.width() > 1, "the preview painted something");

  if(!save.empty())
    img.save(QString::fromStdString(save));

  quint64 sum = 0;
  int distinct = 0;
  QRgb first = img.pixel(0, 0);
  for(int y = 0; y < img.height(); y++)
    for(int x = 0; x < img.width(); x++)
    {
      const QRgb p = img.pixel(x, y);
      sum += qRed(p) + qGreen(p) + qBlue(p);
      if(p != first)
        distinct++;
    }
  const double mean = double(sum) / (3.0 * img.width() * img.height());
  std::printf("     mean %.1f, %d pixels differ from the first\n", mean, distinct);
  check(distinct > 0, "the preview is not a flat colour");

  // Requirement of the editable source field: a name that nothing is
  // broadcasting yet must be accepted, and must connect by itself when that
  // source appears.
  {
    const QString lateToken
        = QStringLiteral("ndi-preview-late-%1").arg(QCoreApplication::applicationPid());
    // Same machine prefix as the source discovery just resolved, so the name
    // can be known before anything is broadcasting it.
    const int paren = source.lastIndexOf(" (");
    const QString later = paren > 0
                              ? source.left(paren) + " (" + lateToken + ")"
                              : lateToken;
    std::printf("\n== a source that is not online yet ==\n");
    preview.clear();

    auto pending = std::make_shared<Ndi::InputStream>(ndi);
    pending->load(later.toStdString());
    preview.setInput(pending);
    pump(3000);

    const auto* before = preview.metadata();
    check(
        before && before->width == 0,
        "nothing arrives while the source is offline, and nothing breaks");

    TestSender late{ndi, lateToken.toStdString(), 1280, 720};
    QElapsedTimer t;
    t.start();
    while(t.elapsed() < 60000)
    {
      pump(500);
      if(const auto* m = preview.metadata(); m && m->width > 0)
        break;
    }

    const auto* after = preview.metadata();
    check(
        after && after->width > 0,
        "the source is picked up on its own once it appears");
    if(after && after->width > 0)
      std::printf("     connected after %lld ms: %dx%d\n", t.elapsed(), after->width,
                  after->height);
  }

  std::printf("\n== after clearing ==\n");
  preview.clear();
  pump(500);
  check(preview.metadata() == nullptr, "the input is released");

  if(!shot.empty())
  {
    Ndi::Testing::StubApplication stub;
    Ndi::InputSettingsWidget w;

    Device::DeviceSettings dev;
    dev.name = source;
    dev.protocol = Ndi::InputFactory::static_concreteKey();
    Ndi::InputSettings in;
    in.path = source;
    dev.deviceSpecificSettings = QVariant::fromValue(in);
    w.setSettings(dev);

    w.resize(680, 620);
    w.show();
    pump(8000);
    w.grab().save(QString::fromStdString(shot));
    std::printf("\ndialog screenshot: %s\n", shot.c_str());
  }

  std::printf(
      "\nndi preview: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed", g_fail,
      g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
