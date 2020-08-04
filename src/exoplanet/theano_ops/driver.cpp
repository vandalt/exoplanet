#include <exoplanet/kepler.h>
#include <exoplanet/starry.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <Eigen/Core>
#include <cmath>
#include <vector>

namespace py = pybind11;

// ASCII art from:
// http://patorjk.com/software/taag/#p=display&f=Small&c=c++

namespace driver {

//    _        _
//   | |_  ___| |_ __  ___ _ _ ___
//   | ' \/ -_) | '_ \/ -_) '_(_-<
//   |_||_\___|_| .__/\___|_| /__/
//              |_|

template <typename Scalar, int ExtraFlags>
struct flat_unchecked_array {
  flat_unchecked_array(py::array_t<Scalar, ExtraFlags> &array, bool require_mutable = false) {
    info = array.request();
    if (require_mutable && info.readonly) throw std::runtime_error("outputs must be writeable");
    data = (Scalar *)info.ptr;
  }

  inline Scalar &operator()(ssize_t index) { return data[index]; }
  inline ssize_t shape(ssize_t index) const { return info.shape[index]; }
  inline ssize_t size() const { return info.size; }
  inline ssize_t ndim() const { return info.ndim; }

  py::buffer_info info;
  Scalar *data;
};

template <typename T>
int sgn(T val) {
  return (T(0) < val) - (val < T(0));
}

//       _
//    __| |_ __ _ _ _ _ _ _  _
//   (_-<  _/ _` | '_| '_| || |
//   /__/\__\__,_|_| |_|  \_, |
//                        |__/

namespace starry {

auto get_cl(py::array_t<double, py::array::c_style> u_in,
            py::array_t<double, py::array::c_style> c_out) {
  auto u = u_in.unchecked<1>();
  auto c = c_out.mutable_unchecked<1>();
  ssize_t N = u.size();
  if (N < 1 || c.size() != N) throw std::runtime_error("dimension mismatch");

  std::vector<double> a(N);
  a[0] = 1;
  for (ssize_t n = 1; n < N; ++n) a[n] = 0;

  // Compute the a_n coefficients
  double bcoeff;
  int sign;
  for (ssize_t i = 1; i < N; ++i) {
    bcoeff = 1;
    sign = 1;
    for (ssize_t j = 0; j <= i; ++j) {
      a[j] -= u(i) * bcoeff * sign;
      sign *= -1;
      bcoeff *= (double(i - j) / (j + 1));
    }
  }

  // Now, compute the c_n coefficients
  for (ssize_t j = N - 1; j >= std::max<ssize_t>(2, N - 2); --j) {
    c(j) = a[j] / (j + 2);
  }
  for (ssize_t j = N - 3; j >= 2; --j) {
    c(j) = a[j] / (j + 2) + c(j + 2);
  }
  if (N >= 4)
    c(1) = a[1] + 3 * c(3);
  else
    c(1) = a[1];
  if (N >= 3)
    c(0) = a[0] + 2 * c(2);
  else
    c(0) = a[0];

  return c_out;
}

auto get_cl_rev(py::array_t<double, py::array::c_style> bc_in,
                py::array_t<double, py::array::c_style> bu_out) {
  auto bc_ = bc_in.unchecked<1>();
  auto bu = bu_out.mutable_unchecked<1>();
  ssize_t N = bc_.size();
  if (N < 1 || bu.size() != N) throw std::runtime_error("dimension mismatch");

  std::vector<double> ba(N), bc(N);
  for (ssize_t i = 0; i < N; ++i) {
    bu(i) = 0;
    bc[i] = bc_(i);
  }

  if (N >= 3) {
    // c[0] = a(0) + 2 * c[2];
    ba[0] = bc[0];
    bc[2] += 2 * bc[0];
  } else {
    // c[0] = a(0);
    ba[0] = bc[0];
  }

  if (N >= 4) {
    // c[1] = a(1) + 3 * c[3];
    ba[1] = bc[1];
    bc[3] += 3 * bc[1];
  } else {
    // c[1] = a(1);
    ba[1] = bc[1];
  }

  for (ssize_t j = 2; j <= N - 3; ++j) {
    // c[j] = a(j) / (j + 2) + c[j + 2];
    ba[j] = bc[j] / (j + 2);
    bc[j + 2] += bc[j];
  }
  for (ssize_t j = std::max<ssize_t>(2, N - 2); j <= N - 1; ++j) {
    // c[j] = a(j) / (j + 2);
    ba[j] = bc[j] / (j + 2);
  }

  // Compute the a_n coefficients
  double bcoeff;
  int sign;
  for (ssize_t i = 1; i < N; ++i) {
    bcoeff = 1;
    sign = 1;
    for (ssize_t j = 0; j <= i; ++j) {
      // a(j) -= u[i] * bcoeff * sign;
      bu(i) -= ba[j] * bcoeff * sign;
      sign *= -1;
      bcoeff *= (double(i - j) / (j + 1));
    }
  }

  return bu_out;
}

struct LimbDark {
  LimbDark() : ld(0){};

  auto apply(py::array_t<double, py::array::c_style> cl_in,
             py::array_t<double, py::array::c_style> b_in,
             py::array_t<double, py::array::c_style> r_in,
             py::array_t<double, py::array::c_style> los_in,
             py::array_t<double, py::array::c_style> f_out,
             py::array_t<double, py::array::c_style> dfdcl_out,
             py::array_t<double, py::array::c_style> dfdb_out,
             py::array_t<double, py::array::c_style> dfdr_out) {
    flat_unchecked_array<double, py::array::c_style> b(b_in), r(r_in), los(los_in);
    ssize_t N = b.size();
    if (r.size() != N || los.size() != N) throw std::runtime_error("dimension mismatch");

    flat_unchecked_array<double, py::array::c_style> f(f_out, true), dfdb(dfdb_out, true),
        dfdr(dfdr_out, true);
    if (f.size() != N || dfdb.size() != N || dfdr.size() != N)
      throw std::runtime_error("dimension mismatch");

    py::buffer_info cl_info = cl_in.request(), dfdcl_info = dfdcl_out.request();
    ssize_t num_cl = cl_info.size;
    if (dfdcl_info.ndim <= 1 || dfdcl_info.shape[0] != num_cl || dfdcl_info.size != N * num_cl)
      throw std::runtime_error("invalid dimensions for dfdcl");
    if (dfdcl_info.readonly) throw std::runtime_error("dfdcl must be writeable");

    Eigen::Map<Eigen::VectorXd> cl((double *)cl_info.ptr, num_cl);
    Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> dfdcl(
        (double *)dfdcl_info.ptr, num_cl, N);
    dfdcl.setZero();

    // Re-initialize if lmax is wrong
    if (ld.lmax != int(num_cl))
      ld = exoplanet::starry::limbdark::GreensLimbDark<double>(int(num_cl));

    for (ssize_t i = 0; i < N; ++i) {
      f(i) = 0;
      dfdb(i) = 0;
      dfdr(i) = 0;

      if (los(i) > 0) {
        auto b_ = std::abs(b(i));
        auto r_ = std::abs(r(i));
        if (b_ < 1 + r_) {
          ld.compute<true>(b_, r_);
          auto sT = ld.sT;

          // The value of the light curve
          f(i) = sT.dot(cl) - 1;

          // The gradients
          dfdcl.col(i) = sT;
          dfdb(i) = sgn(b(i)) * ld.dsTdb.dot(cl);
          dfdr(i) = sgn(r(i)) * ld.dsTdr.dot(cl);
        }
      }
    }

    return std::make_tuple(f_out, dfdcl_out, dfdb_out, dfdr_out);
  }

  exoplanet::starry::limbdark::GreensLimbDark<double> ld;
};

}  // namespace starry

//    _            _
//   | |_____ _ __| |___ _ _
//   | / / -_) '_ \ / -_) '_|
//   |_\_\___| .__/_\___|_|
//           |_|

namespace kepler {

auto kepler(py::array_t<double, py::array::c_style> M_in,
            py::array_t<double, py::array::c_style> ecc_in,
            py::array_t<double, py::array::c_style> sinf_out,
            py::array_t<double, py::array::c_style> cosf_out) {
  flat_unchecked_array<double, py::array::c_style> M(M_in), ecc(ecc_in);
  flat_unchecked_array<double, py::array::c_style> cosf(cosf_out, true), sinf(sinf_out, true);
  ssize_t N = M.size();
  if (ecc.size() != N || cosf.size() != N || sinf.size() != N)
    throw std::runtime_error("dimension mismatch");
  for (ssize_t n = 0; n < N; ++n) {
    if (ecc(n) < 0 || ecc(n) > 1)
      throw std::runtime_error("eccentricity must be in the range [0, 1)");
    exoplanet::kepler::solve_kepler(M(n), ecc(n), cosf(n), sinf(n));
  }
  return std::make_tuple(cosf_out, sinf_out);
}

}  // namespace kepler
}  // namespace driver

//              _    _         _ _ _
//    _ __ _  _| |__(_)_ _  __| / / |
//   | '_ \ || | '_ \ | ' \/ _` | | |
//   | .__/\_, |_.__/_|_||_\__,_|_|_|
//   |_|   |__/

PYBIND11_MODULE(driver, m) {
  m.doc() = R"doc(
    The computation engine for the Theano ops
)doc";

  m.def("get_cl", &driver::starry::get_cl, py::arg("u").noconvert(), py::arg("c").noconvert());
  m.def("get_cl_rev", &driver::starry::get_cl_rev, py::arg("bc").noconvert(),
        py::arg("bu").noconvert());

  py::class_<driver::starry::LimbDark>(m, "LimbDark")
      .def(py::init<>())
      .def("apply", &driver::starry::LimbDark::apply, py::arg("cl").noconvert(),
           py::arg("b").noconvert(), py::arg("r").noconvert(), py::arg("los").noconvert(),
           py::arg("f").noconvert(), py::arg("dfdcl").noconvert(), py::arg("dfdb").noconvert(),
           py::arg("dfdr").noconvert());

  m.def("kepler", &driver::kepler::kepler, py::arg("M").noconvert(), py::arg("ecc").noconvert(),
        py::arg("sinf").noconvert(), py::arg("cosf").noconvert());

#ifdef VERSION_INFO
  m.attr("__version__") = VERSION_INFO;
#else
  m.attr("__version__") = "dev";
#endif
}
