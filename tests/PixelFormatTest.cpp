// Drives Ndi::describeVideoFrame directly: no GPU, no NDI runtime, no score.
// What it pins is the description handed to the SDK -- FourCC, data pointer and
// line stride -- for bytes that are ALREADY in the wire format.
//
// There is no colour conversion to test any more: RGBA comes back from the
// scene as RGBA, and UYVY is produced by score::gfx::UYVYEncoder on the GPU
// (covered by Gfx/tests/EncoderTester.cpp). This function only describes those
// bytes, and the property that matters is that it describes them without
// copying: p_data must be the caller's pointer, every time.
#include <Ndi/InputSettings.hpp>
#include <Ndi/Loader.hpp>
#include <Ndi/VideoFrameFormat.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                     \
  do {                                       \
    if(!(cond))                              \
    {                                        \
      std::printf("  FAIL: " __VA_ARGS__);   \
      std::printf("\n");                     \
      ++failures;                            \
    }                                        \
  } while(0)

int main()
{
  const int w = 64, h = 32;

  // RGBA: one texel per pixel, four bytes each.
  {
    std::vector<uint8_t> bytes(size_t(w) * h * 4, 0xAB);
    NDIlib_video_frame_v2_t f{};
    const bool ok = Ndi::describeVideoFrame("RGBA", bytes.data(), w, h, 4 * w, f);
    CHECK(ok, "RGBA was refused");
    CHECK(f.FourCC == NDIlib_FourCC_video_type_RGBA, "RGBA: wrong FourCC");
    CHECK(f.line_stride_in_bytes == 4 * w, "RGBA: stride %d, expected %d",
          f.line_stride_in_bytes, 4 * w);
    CHECK(f.xres == w && f.yres == h, "RGBA: wrong dimensions");
    // Zero copy: the SDK reads the readback itself. What makes that safe is the
    // ReadbackPool's ownership states, not a copy here -- a copy on this path
    // would be pure latency.
    CHECK(f.p_data == bytes.data(), "RGBA: must be sent from the readback, not copied");
    std::printf("  ok RGBA   stride=%d  zero-copy\n", f.line_stride_in_bytes);
  }

  // UYVY: two pixels per four-byte macropixel, so a packed row is 2*w bytes.
  {
    std::vector<uint8_t> bytes(size_t(w) * h * 2, 0xCD);
    NDIlib_video_frame_v2_t f{};
    const bool ok = Ndi::describeVideoFrame("UYVY", bytes.data(), w, h, 2 * w, f);
    CHECK(ok, "UYVY was refused");
    CHECK(f.FourCC == NDIlib_FourCC_video_type_UYVY, "UYVY: wrong FourCC");
    CHECK(f.line_stride_in_bytes == 2 * w, "UYVY: stride %d, expected %d",
          f.line_stride_in_bytes, 2 * w);
    CHECK(f.xres == w && f.yres == h, "UYVY: wrong dimensions");
    CHECK(f.p_data == bytes.data(), "UYVY: must be sent from the readback, not copied");
    std::printf("  ok UYVY   stride=%d  zero-copy\n", f.line_stride_in_bytes);
  }

  // A padded readback: the backend may hand back rows wider than the packed
  // size, and the stride has to be reported as it is rather than recomputed.
  {
    const int padded = 4 * w + 64;
    std::vector<uint8_t> bytes(size_t(padded) * h, 0x11);
    NDIlib_video_frame_v2_t f{};
    const bool ok = Ndi::describeVideoFrame("RGBA", bytes.data(), w, h, padded, f);
    CHECK(ok, "a padded RGBA readback was refused");
    CHECK(f.line_stride_in_bytes == padded, "padded RGBA: stride %d, expected %d",
          f.line_stride_in_bytes, padded);
    std::printf("  ok RGBA   padded stride=%d honoured\n", f.line_stride_in_bytes);
  }

  // readbackStride derives the stride from what actually came back.
  {
    CHECK(Ndi::readbackStride(4 * w * h, h) == 4 * w, "readbackStride: packed RGBA");
    CHECK(Ndi::readbackStride(2 * w * h, h) == 2 * w, "readbackStride: packed UYVY");
    CHECK(Ndi::readbackStride(0, h) == 0, "readbackStride: empty readback must be 0");
    CHECK(Ndi::readbackStride(4 * w * h, 0) == 0, "readbackStride: zero height must be 0");
    std::printf("  ok readbackStride\n");
  }

  // Only the formats the addon writes are accepted. NDIlib_video_frame_v2_t
  // defaults its FourCC to UYVY, so anything accepted by mistake would be sent
  // as a valid-looking UYVY frame. The names are case-sensitive and the two
  // alpha-carrying formats are not written at all: no GPU encoder produces an
  // alpha plane, so UYVA and PA16 have nothing behind them.
  {
    std::vector<uint8_t> bytes(size_t(w) * h * 4, 0);
    for(const char* bad : {"", " ", "uyvy", "rgba", "UYVA", "PA16", "P010", "V210"})
    {
      NDIlib_video_frame_v2_t f{};
      const bool ok = Ndi::describeVideoFrame(bad, bytes.data(), w, h, 4 * w, f);
      CHECK(!ok, "format '%s' was accepted; this addon does not write it", bad);
      CHECK(
          f.p_data == nullptr,
          "format '%s' was refused but left a data pointer behind", bad);
    }
    std::printf("  ok refusal of the formats this addon does not write\n");
  }

  // Every format it does write: the FourCC it claims, and the stride it needs.
  // A wrong FourCC here is not a visible failure downstream -- the receiver
  // reads the bytes as whatever was claimed -- so it is pinned per format.
  {
    struct Case
    {
      const char* name;
      NDIlib_FourCC_video_type_e fourcc;
      int rowBytes;    // packed row of the primary plane, at width w
      int rows;        // framestore rows, at height h
    };
    const Case cases[] = {
        {"RGBA", NDIlib_FourCC_video_type_RGBA, 4 * w, h},
        {"RGBX", NDIlib_FourCC_video_type_RGBX, 4 * w, h},
        {"BGRA", NDIlib_FourCC_video_type_BGRA, 4 * w, h},
        {"BGRX", NDIlib_FourCC_video_type_BGRX, 4 * w, h},
        {"UYVY", NDIlib_FourCC_video_type_UYVY, 2 * w, h},
        {"P216", NDIlib_FourCC_video_type_P216, 2 * w, 2 * h},
        {"NV12", NDIlib_FourCC_video_type_NV12, w, h + h / 2},
        {"I420", NDIlib_FourCC_video_type_I420, w, h + h / 2},
        {"YV12", NDIlib_FourCC_video_type_YV12, w, h + h / 2},
    };

    std::vector<uint8_t> bytes(size_t(w) * h * 4, 0);
    for(const auto& c : cases)
    {
      CHECK(
          Ndi::packedRowBytes(c.name, w) == c.rowBytes,
          "%s: packed row %d, expected %d", c.name, Ndi::packedRowBytes(c.name, w),
          c.rowBytes);
      CHECK(
          Ndi::framestoreRows(c.name, h) == c.rows, "%s: %d framestore rows, expected %d",
          c.name, Ndi::framestoreRows(c.name, h), c.rows);

      NDIlib_video_frame_v2_t f{};
      CHECK(
          Ndi::describeVideoFrame(c.name, bytes.data(), w, h, c.rowBytes, f),
          "%s: refused at its own packed stride", c.name);
      CHECK(f.FourCC == c.fourcc, "%s: wrong FourCC", c.name);
      CHECK(f.xres == w && f.yres == h, "%s: describes the picture, not the store",
            c.name);
      CHECK(f.p_data == bytes.data(), "%s: zero copy", c.name);
      CHECK(f.line_stride_in_bytes == c.rowBytes, "%s: stride", c.name);

      // One byte short of a packed row: the SDK would read each row short and
      // walk into the next one.
      NDIlib_video_frame_v2_t g{};
      CHECK(
          !Ndi::describeVideoFrame(c.name, bytes.data(), w, h, c.rowBytes - 1, g),
          "%s: accepted a stride below its packed row", c.name);

      // And the stride a padded readback of that framestore would produce.
      const int padded = c.rowBytes + 64;
      NDIlib_video_frame_v2_t p{};
      CHECK(
          Ndi::describeVideoFrame(c.name, bytes.data(), w, h, padded, p)
              && p.line_stride_in_bytes == padded,
          "%s: padded stride", c.name);
    }
    std::printf("  ok every format this addon writes\n");
  }

  // The subsampled formats cannot express an odd size, and the 4:2:0 ones
  // cannot express an odd height either -- half of an odd number of lines is
  // not a number of lines.
  {
    std::vector<uint8_t> bytes(size_t(w) * h * 4, 0);
    for(const char* f422 : {"UYVY", "P216"})
    {
      NDIlib_video_frame_v2_t f{};
      CHECK(
          !Ndi::describeVideoFrame(f422, bytes.data(), w - 1, h, 2 * w, f),
          "%s: accepted an odd width", f422);
    }
    for(const char* f420 : {"NV12", "I420", "YV12"})
    {
      NDIlib_video_frame_v2_t f{}, g{};
      CHECK(
          !Ndi::describeVideoFrame(f420, bytes.data(), w - 1, h, w, f),
          "%s: accepted an odd width", f420);
      CHECK(
          !Ndi::describeVideoFrame(f420, bytes.data(), w, h - 1, w, g),
          "%s: accepted an odd height", f420);
    }
    // The packed RGB orders have no such constraint: one texel per pixel.
    NDIlib_video_frame_v2_t odd{};
    CHECK(
        Ndi::describeVideoFrame("RGBA", bytes.data(), w - 1, h - 1, 4 * w, odd),
        "RGBA: an odd size is fine");
    std::printf("  ok odd sizes\n");
  }

  // Assembling planes into one framestore. This is what a planar format costs
  // and it is where a padded readback would corrupt every row after the first,
  // so the padded case is the one that matters.
  {
    // Two planes, 4 rows of 8 bytes each, read back with 3 bytes of padding.
    const int rowBytes = 8, rows = 4, pad = 3;
    std::vector<uint8_t> a(size_t(rowBytes + pad) * rows, 0xAA);
    std::vector<uint8_t> b(size_t(rowBytes + pad) * rows, 0xBB);
    for(int r = 0; r < rows; r++)
    {
      a[size_t(r) * (rowBytes + pad)] = uint8_t(r);       // first byte of each row
      b[size_t(r) * (rowBytes + pad)] = uint8_t(0x10 + r);
    }
    const Ndi::PlaneSource src[2] = {
        {a.data(), rowBytes + pad, rowBytes, rows},
        {b.data(), rowBytes + pad, rowBytes, rows},
    };

    std::vector<uint8_t> dst(size_t(rowBytes) * rows * 2, 0);
    const size_t n = Ndi::assembleFramestore(dst.data(), dst.size(), src, 2);
    CHECK(n == dst.size(), "assemble: wrote %zu of %zu", n, dst.size());

    bool rowsOk = true;
    for(int r = 0; r < rows; r++)
    {
      rowsOk &= dst[size_t(r) * rowBytes] == uint8_t(r);
      rowsOk &= dst[size_t(rows + r) * rowBytes] == uint8_t(0x10 + r);
    }
    CHECK(rowsOk, "assemble: the padding was copied along with the rows");

    // A destination one byte short must write nothing rather than overrun.
    std::vector<uint8_t> small(dst.size() - 1, 0);
    CHECK(
        Ndi::assembleFramestore(small.data(), small.size(), src, 2) == 0,
        "assemble: accepted a destination that cannot hold the framestore");

    // A source stride below its own row size is a contradiction, not padding.
    const Ndi::PlaneSource bad[1] = {{a.data(), rowBytes - 1, rowBytes, rows}};
    CHECK(
        Ndi::assembleFramestore(dst.data(), dst.size(), bad, 1) == 0,
        "assemble: accepted a stride below the row size");
    std::printf("  ok framestore assembly\n");
  }

  // The NDI runtime version rule. An NDI 5 runtime substitutes a placeholder
  // frame for HDR content and reports nothing, so the log line this drives is
  // the only warning anyone gets.
  {
    struct Case { const char* version; bool hdr; };
    for(const Case c : {
            Case{"NDI SDK LINUX 10:39:50 Jun  2 2025 6.2.0.3", true},
            Case{"NDI SDK LINUX 12:52:02 Feb 16 2024 5.6.1", false},
            Case{"NDI SDK LINUX 00:00:00 Jan  1 2030 7.0.0", true},
            Case{"NDI SDK LINUX 00:00:00 Jan  1 2030 10.0.0", true},
            Case{"4.5.1", false},
            Case{"6.0", true},
            Case{"6", true},
            Case{"", false},
            Case{"NDI SDK LINUX unknown", false},
            Case{"trailing space ", false},
        })
    {
      const bool got = Ndi::ndiVersionSupportsHDR(c.version);
      CHECK(got == c.hdr, "version '%s': HDR %d, expected %d", c.version, got, c.hdr);
    }
    std::printf("  ok NDI runtime version rule\n");
  }

  // The receive-format setting. Its default has to be the 8-bit path: that is
  // what every device did before the setting existed, and a device deserialized
  // from an older save has an empty string here.
  {
    CHECK(
        Ndi::receiveFormatFromName(QString{}) == Ndi::ReceiveFormat::EightBit,
        "an empty receive format must default to 8-bit");
    CHECK(
        Ndi::receiveFormatFromName("nonsense") == Ndi::ReceiveFormat::EightBit,
        "an unknown receive format must default to 8-bit");
    for(auto f : Ndi::receiveFormats)
      CHECK(
          Ndi::receiveFormatFromName(Ndi::receiveFormatName(f)) == f,
          "receive format '%s' does not round-trip", Ndi::receiveFormatName(f));
    std::printf("  ok receive format setting\n");
  }

  std::printf(
      "\nndi pixel format tests: %s (%d failure%s)\n", failures ? "FAILED" : "passed",
      failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
