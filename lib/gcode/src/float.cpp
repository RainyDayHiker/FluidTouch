// Float helpers: packages/core/src/num/float.ts translated line for line (specs/15 §13.2). Only
// operations IEEE 754 defines exactly, so these give the bits TS gives on every platform. The
// platform's own hypot/atan2/sin/cos are never called. atan2, sin and cos follow fdlibm 5.3
// (Sun Microsystems, freely redistributable).
#include <cmath>
#include <cstdint>
#include <cstring>

#include "gcode/num.h"

namespace gcode {
namespace fmath {
namespace {

int32_t hi(double x) {
  uint64_t u;
  std::memcpy(&u, &x, sizeof u);
  return static_cast<int32_t>(static_cast<uint32_t>(u >> 32));
}

uint32_t lo(double x) {
  uint64_t u;
  std::memcpy(&u, &x, sizeof u);
  return static_cast<uint32_t>(u);
}

double fromHi(int32_t h) {
  const uint64_t u = static_cast<uint64_t>(static_cast<uint32_t>(h)) << 32;
  double x;
  std::memcpy(&x, &u, sizeof x);
  return x;
}

const double ATAN_HI[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00};
const double ATAN_LO[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17};
const double AT[] = {
    3.33333333333329318027e-01,  -1.99999999998764832476e-01, 1.42857142725034663711e-01,
    -1.11111104054623557880e-01, 9.09088713343650656196e-02,  -7.69187620504482999495e-02,
    6.66107313738753120669e-02,  -5.83357013379057348645e-02, 4.97687799461593236017e-02,
    -3.65315727442169155270e-02, 1.62858201153657823623e-02,
};
const double HUGE_ = 1.0e300;
const double TINY = 1.0e-300;

double atan(double x) {
  const int32_t hx = hi(x);
  const int32_t ix = hx & 0x7fffffff;
  int id;
  if (ix >= 0x44100000) {
    if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lo(x) != 0)) return x + x;
    return hx > 0 ? ATAN_HI[3] + ATAN_LO[3] : -ATAN_HI[3] - ATAN_LO[3];
  }
  if (ix < 0x3fdc0000) {
    if (ix < 0x3e200000) {
      if (HUGE_ + x > 1) return x;
    }
    id = -1;
  } else {
    x = std::fabs(x);
    if (ix < 0x3ff30000) {
      if (ix < 0x3fe60000) {
        id = 0;
        x = (2.0 * x - 1.0) / (2.0 + x);
      } else {
        id = 1;
        x = (x - 1.0) / (x + 1.0);
      }
    } else if (ix < 0x40038000) {
      id = 2;
      x = (x - 1.5) / (1.0 + 1.5 * x);
    } else {
      id = 3;
      x = -1.0 / x;
    }
  }
  const double z = x * x;
  const double w = z * z;
  const double s1 = z * (AT[0] + w * (AT[2] + w * (AT[4] + w * (AT[6] + w * (AT[8] + w * AT[10])))));
  const double s2 = w * (AT[1] + w * (AT[3] + w * (AT[5] + w * (AT[7] + w * AT[9]))));
  if (id < 0) return x - x * (s1 + s2);
  const double r = ATAN_HI[id] - ((x * (s1 + s2) - ATAN_LO[id]) - x);
  return hx < 0 ? -r : r;
}

const double PI_O_4 = 7.8539816339744827900e-01;
const double PI_O_2 = 1.5707963267948965580e+00;
const double PI = 3.1415926535897931160e+00;
const double PI_LO = 1.2246467991473531772e-16;

const double S1 = -1.66666666666666324348e-01;
const double S2 = 8.33333333332248946124e-03;
const double S3 = -1.98412698298579493134e-04;
const double S4 = 2.75573137070700676789e-06;
const double S5 = -2.50507602534068634195e-08;
const double S6 = 1.58969099521155010221e-10;

double kernelSin(double x, double y, int iy) {
  const int32_t ix = hi(x) & 0x7fffffff;
  if (ix < 0x3e400000) {
    if (static_cast<int>(x) == 0) return x;
  }
  const double z = x * x;
  const double v = z * x;
  const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  if (iy == 0) return x + v * (S1 + z * r);
  return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

const double C1 = 4.16666666666666019037e-02;
const double C2 = -1.38888888888741095749e-03;
const double C3 = 2.48015872894767294178e-05;
const double C4 = -2.75573143513906633035e-07;
const double C5 = 2.08757232129817482790e-09;
const double C6 = -1.13596475577881948265e-11;

double kernelCos(double x, double y) {
  const int32_t ix = hi(x) & 0x7fffffff;
  if (ix < 0x3e400000) {
    if (static_cast<int>(x) == 0) return 1.0;
  }
  const double z = x * x;
  const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  if (ix < 0x3fd33333) return 1.0 - (0.5 * z - (z * r - x * y));
  const double qx = ix > 0x3fe90000 ? 0.28125 : fromHi(ix - 0x00200000);
  const double hz = 0.5 * z - qx;
  const double a = 1.0 - qx;
  return a - (hz - (z * r - x * y));
}

const double INVPIO2 = 6.36619772367581382433e-01;
const double PIO2_1 = 1.57079632673412561417e+00;
const double PIO2_1T = 6.07710050650619224932e-11;
const double PIO2_2 = 6.07710050630396597660e-11;
const double PIO2_2T = 2.02226624879595063154e-21;
const double PIO2_3 = 2.02226624871116645580e-21;
const double PIO2_3T = 8.47842766036889956997e-32;

const int32_t NPIO2_HW[] = {
    0x3ff921fb, 0x400921fb, 0x4012d97c, 0x401921fb, 0x401f6a7a, 0x4022d97c, 0x4025fdbb, 0x402921fb,
    0x402c463a, 0x402f6a7a, 0x4031475c, 0x4032d97c, 0x40346b9c, 0x4035fdbb, 0x40378fdb, 0x403921fb,
    0x403ab41b, 0x403c463a, 0x403dd85a, 0x403f6a7a, 0x40407e4c, 0x4041475c, 0x4042106c, 0x4042d97c,
    0x4043a28c, 0x40446b9c, 0x404534ac, 0x4045fdbb, 0x4046c6cb, 0x40478fdb, 0x404858eb, 0x404921fb,
};

/** e_rem_pio2.c without its large-argument path: y[0] + y[1] = x - n*pi/2. */
int remPio2(double x, double y[2]) {
  const int32_t hx = hi(x);
  const int32_t ix = hx & 0x7fffffff;
  if (ix <= 0x3fe921fb) {
    y[0] = x;
    y[1] = 0;
    return 0;
  }
  if (ix < 0x4002d97c) {
    if (hx > 0) {
      double z = x - PIO2_1;
      if (ix != 0x3ff921fb) {
        y[0] = z - PIO2_1T;
        y[1] = (z - y[0]) - PIO2_1T;
      } else {
        z -= PIO2_2;
        y[0] = z - PIO2_2T;
        y[1] = (z - y[0]) - PIO2_2T;
      }
      return 1;
    }
    double z = x + PIO2_1;
    if (ix != 0x3ff921fb) {
      y[0] = z + PIO2_1T;
      y[1] = (z - y[0]) + PIO2_1T;
    } else {
      z += PIO2_2;
      y[0] = z + PIO2_2T;
      y[1] = (z - y[0]) + PIO2_2T;
    }
    return -1;
  }
  if (ix <= 0x413921fb) {
    const double t = std::fabs(x);
    const int n = static_cast<int>(t * INVPIO2 + 0.5);
    const double fn = static_cast<double>(n);
    double r = t - fn * PIO2_1;
    double w = fn * PIO2_1T;
    if (n < 32 && ix != NPIO2_HW[n - 1]) {
      y[0] = r - w;
    } else {
      const int32_t j = ix >> 20;
      y[0] = r - w;
      int32_t i = j - ((hi(y[0]) >> 20) & 0x7ff);
      if (i > 16) {
        double u = r;
        w = fn * PIO2_2;
        r = u - w;
        w = fn * PIO2_2T - ((u - r) - w);
        y[0] = r - w;
        i = j - ((hi(y[0]) >> 20) & 0x7ff);
        if (i > 49) {
          u = r;
          w = fn * PIO2_3;
          r = u - w;
          w = fn * PIO2_3T - ((u - r) - w);
          y[0] = r - w;
        }
      }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) {
      y[0] = -y[0];
      y[1] = -y[1];
      return -n;
    }
    return n;
  }
  y[0] = NAN;
  y[1] = NAN;
  return 0;
}

}  // namespace

double hypot(double x, double y) { return std::sqrt(x * x + y * y); }

double atan2(double y, double x) {
  const int32_t hx = hi(x);
  const uint32_t lx = lo(x);
  const int32_t ix = hx & 0x7fffffff;
  const int32_t hy = hi(y);
  const uint32_t ly = lo(y);
  const int32_t iy = hy & 0x7fffffff;
  if ((static_cast<uint32_t>(ix) | ((lx | (0u - lx)) >> 31)) > 0x7ff00000u ||
      (static_cast<uint32_t>(iy) | ((ly | (0u - ly)) >> 31)) > 0x7ff00000u) {
    return x + y;
  }
  if (((static_cast<uint32_t>(hx) - 0x3ff00000u) | lx) == 0) return atan(y);
  int m = static_cast<int>(((static_cast<uint32_t>(hy) >> 31) & 1) | ((static_cast<uint32_t>(hx) >> 30) & 2));

  if ((static_cast<uint32_t>(iy) | ly) == 0) {
    if (m == 0 || m == 1) return y;
    return m == 2 ? PI + TINY : -PI - TINY;
  }
  if ((static_cast<uint32_t>(ix) | lx) == 0) return hy < 0 ? -PI_O_2 - TINY : PI_O_2 + TINY;
  if (ix == 0x7ff00000) {
    if (iy == 0x7ff00000) {
      if (m == 0) return PI_O_4 + TINY;
      if (m == 1) return -PI_O_4 - TINY;
      if (m == 2) return 3.0 * PI_O_4 + TINY;
      return -3.0 * PI_O_4 - TINY;
    }
    if (m == 0) return 0.0;
    if (m == 1) return -0.0;
    if (m == 2) return PI + TINY;
    return -PI - TINY;
  }
  if (iy == 0x7ff00000) return hy < 0 ? -PI_O_2 - TINY : PI_O_2 + TINY;

  const int32_t k = (iy - ix) >> 20;
  double z;
  if (k > 60) {
    z = PI_O_2 + 0.5 * PI_LO;
    m &= 1;
  } else if (hx < 0 && k < -60) {
    z = 0.0;
  } else {
    z = atan(std::fabs(y / x));
  }
  if (m == 0) return z;
  if (m == 1) return -z;
  if (m == 2) return PI - (z - PI_LO);
  return (z - PI_LO) - PI;
}

double acos(double u) { return atan2(std::sqrt((1 - u) * (1 + u)), u); }

double sin(double x) {
  const int32_t ix = hi(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return kernelSin(x, 0, 0);
  if (ix >= 0x7ff00000) return x - x;
  double y[2];
  const int n = remPio2(x, y);
  switch (n & 3) {
    case 0: return kernelSin(y[0], y[1], 1);
    case 1: return kernelCos(y[0], y[1]);
    case 2: return -kernelSin(y[0], y[1], 1);
    default: return -kernelCos(y[0], y[1]);
  }
}

double cos(double x) {
  const int32_t ix = hi(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return kernelCos(x, 0);
  if (ix >= 0x7ff00000) return x - x;
  double y[2];
  const int n = remPio2(x, y);
  switch (n & 3) {
    case 0: return kernelCos(y[0], y[1]);
    case 1: return -kernelSin(y[0], y[1], 1);
    case 2: return -kernelCos(y[0], y[1]);
    default: return kernelSin(y[0], y[1], 1);
  }
}

}  // namespace fmath
}  // namespace gcode
