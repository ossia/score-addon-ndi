#pragma once
#include <QString>

namespace Ndi
{
struct OutputSettings
{
  QString path;
  int width{};
  int height{};
  double rate{};
  QString format; // RGBA, UYVY...
};
}
