// Copyright 2020 Global Phasing Ltd.
//
// Reading intensities from MTZ. Merging multi-record or anomalous.

#ifndef GEMMI_MERGE_HPP_
#define GEMMI_MERGE_HPP_

#include "symmetry.hpp"
#include "unitcell.hpp"
#include "util.hpp"     // for vector_remove_if
#include "mtz.hpp"      // for Mtz
#include "refln.hpp"    // for ReflnBlock
#include "xds_ascii.hpp" // for XdsAscii

namespace gemmi {

struct Intensities {

  struct Refl {
    Miller hkl;
    int isign = 0;  // 1 for I(+), -1 for I(-)
    double value;
    double sigma;

    bool operator<(const Refl& o) const {
      return std::tie(hkl[0], hkl[1], hkl[2], isign) <
             std::tie(o.hkl[0], o.hkl[1], o.hkl[2], o.isign);
    }
  };

  std::vector<Refl> data;
  const SpaceGroup* spacegroup = nullptr;
  UnitCell unit_cell;
  double wavelength;

  bool have_sign() const { return !data.empty() && data[0].isign != 0; }

  std::array<double,2> resolution_range() const {
    double min_1_d2 = INFINITY;
    double max_1_d2 = 0;
    for (const Refl& x : data) {
      double a_1_d2 = unit_cell.calculate_1_d2(x.hkl);
      if (a_1_d2 < min_1_d2)
        min_1_d2 = a_1_d2;
      if (a_1_d2 > max_1_d2)
        max_1_d2 = a_1_d2;
    }
    return { 1 / std::sqrt(min_1_d2), 1 / std::sqrt(max_1_d2) };
  }

  template<typename Source>
  void copy_metadata(const Source& source) {
    unit_cell = source.cell;
    spacegroup = source.spacegroup;
    if (!spacegroup)
      fail("unknown space group");
  }

  void add_if_valid(const Refl& refl) {
      // XDS marks rejected reflections with negative sigma.
      // Sigma 0.0 is also problematic - it rarely happens (e.g. 5tkn).
    if (!std::isnan(refl.value) && refl.sigma > 0)
      data.push_back(refl);
  }

  template<typename DataProxy>
  void read_data(const DataProxy& proxy, size_t value_idx, size_t sigma_idx) {
    for (size_t i = 0; i < proxy.size(); i += proxy.stride()) {
      Refl refl;
      refl.hkl = proxy.get_hkl(i);
      refl.value = proxy.get_num(i + value_idx);
      refl.sigma = proxy.get_num(i + sigma_idx);
      add_if_valid(refl);
    }
  }

  std::string spacegroup_str() const { return spacegroup ? spacegroup->xhm() : "none"; }

  void remove_systematic_absences() {
    if (!spacegroup)
      return;
    GroupOps gops = spacegroup->operations();
    vector_remove_if(data, [&](Refl& x) {
        return gops.is_systematically_absent(x.hkl);
    });
  }

  void sort() { std::sort(data.begin(), data.end()); }

  void merge_in_place(bool output_plus_minus) {
    if (data.empty())
      return;
    if (!output_plus_minus)
      // discard signs so that merging produces Imean
      for (Refl& refl : data)
        refl.isign = 0;
    sort();
    std::vector<Refl>::iterator out = data.begin();
    double sum_wI = 0.;
    double sum_w = 0.;
    for (auto in = data.begin(); in != data.end(); ++in) {
      if (out->hkl != in->hkl || out->isign != in->isign) {
        out->value = sum_wI / sum_w;
        out->sigma = 1.0 / std::sqrt(sum_w);
        sum_wI = sum_w = 0.;
        ++out;
        out->hkl = in->hkl;
        out->isign = in->isign;
      }
      double w = 1. / (in->sigma * in->sigma);
      sum_wI += w * in->value;
      sum_w += w;
    }
    out->value = sum_wI / sum_w;
    out->sigma = 1.0 / std::sqrt(sum_w);
    data.erase(++out, data.end());
  }

  void switch_to_asu_indices(bool merged=false) {
    GroupOps gops = spacegroup->operations();
    ReciprocalAsu asu(spacegroup);
    for (Refl& refl : data) {
      if (asu.is_in(refl.hkl))
        continue;
      auto hkl_isym = asu.to_asu(refl.hkl, gops);
      refl.hkl = hkl_isym.first;
      if (!merged)
        refl.isign = (hkl_isym.second % 2 == 0 ? -1 : 1);
    }
  }
};


inline Intensities read_unmerged_intensities_from_mtz(const Mtz& mtz) {
  if (mtz.batches.empty())
    fail("expected unmerged file");
  const Mtz::Column* isym_col = mtz.column_with_label("M/ISYM");
  if (!isym_col || isym_col->idx != 3)
    fail("unmerged file should have M/ISYM as 4th column");
  const Mtz::Column& col = mtz.get_column_with_label("I");
  size_t value_idx = col.idx;
  size_t sigma_idx = mtz.get_column_with_label("SIGI").idx;
  Intensities intensities;
  intensities.unit_cell = mtz.get_average_cell_from_batch_headers(nullptr);
  intensities.spacegroup = mtz.spacegroup;
  if (!intensities.spacegroup)
    fail("unknown space group");
  intensities.wavelength = mtz.dataset(col.dataset_id).wavelength;
  for (size_t i = 0; i < mtz.data.size(); i += mtz.columns.size()) {
    Intensities::Refl refl;
    refl.hkl = mtz.get_hkl(i);
    refl.isign = ((int)mtz.data[i + 3] % 2 == 0 ? -1 : 1);
    refl.value = mtz.data[i + value_idx];
    refl.sigma = mtz.data[i + sigma_idx];
    intensities.add_if_valid(refl);
  }
  // Aimless >=0.7.6 (from 2021) has an option to output unmerged file
  // with original indices instead of reduced indices, with all ISYM = 1.
  intensities.switch_to_asu_indices();
  return intensities;
}

inline Intensities read_mean_intensities_from_mtz(const Mtz& mtz) {
  if (!mtz.batches.empty())
    fail("expected merged file");
  const Mtz::Column* col = mtz.column_with_one_of_labels({"IMEAN", "I"});
  if (!col)
    fail("Mean intensities (IMEAN or I) not found.");
  size_t sigma_idx = mtz.get_column_with_label("SIG" + col->label).idx;
  Intensities intensities;
  intensities.copy_metadata(mtz);
  intensities.wavelength = mtz.dataset(col->dataset_id).wavelength;
  intensities.read_data(MtzDataProxy{mtz}, col->idx, sigma_idx);
  return intensities;
}

inline Intensities read_anomalous_intensities_from_mtz(const Mtz& mtz) {
  if (!mtz.batches.empty())
    fail("expected merged file");
  const Mtz::Column& col = mtz.get_column_with_label("I(+)");
  size_t value_idx[2] = {col.idx, mtz.get_column_with_label("I(-)").idx};
  size_t sigma_idx[2] = {mtz.get_column_with_label("SIGI(+)").idx,
                         mtz.get_column_with_label("SIGI(-)").idx};
  Intensities intensities;
  intensities.copy_metadata(mtz);
  intensities.wavelength = mtz.dataset(col.dataset_id).wavelength;
  for (size_t i = 0; i < mtz.data.size(); i += mtz.columns.size())
    for (int j = 0; j < 2; ++j) {
      Intensities::Refl refl;
      refl.hkl = mtz.get_hkl(i);
      refl.isign = (j == 0 ? 1 : -1);
      refl.value = mtz.data[i + value_idx[j]];
      refl.sigma = mtz.data[i + sigma_idx[j]];
      intensities.add_if_valid(refl);
    }
  return intensities;
}


inline Intensities read_unmerged_intensities_from_mmcif(const ReflnBlock& rb) {
  size_t value_idx = rb.get_column_index("intensity_net");
  size_t sigma_idx = rb.get_column_index("intensity_sigma");
  Intensities intensities;
  intensities.copy_metadata(rb);
  intensities.wavelength = rb.wavelength;
  intensities.read_data(ReflnDataProxy(rb), value_idx, sigma_idx);
  intensities.switch_to_asu_indices();
  return intensities;
}

inline Intensities read_mean_intensities_from_mmcif(const ReflnBlock& rb) {
  size_t value_idx = rb.get_column_index("intensity_meas");
  size_t sigma_idx = rb.get_column_index("intensity_sigma");
  Intensities intensities;
  intensities.copy_metadata(rb);
  intensities.wavelength = rb.wavelength;
  intensities.read_data(ReflnDataProxy(rb), value_idx, sigma_idx);
  return intensities;
}

inline Intensities read_anomalous_intensities_from_mmcif(const ReflnBlock& rb) {
  size_t value_idx[2] = {rb.get_column_index("pdbx_I_plus"),
                         rb.get_column_index("pdbx_I_minus")};
  size_t sigma_idx[2] = {rb.get_column_index("pdbx_I_plus_sigma"),
                         rb.get_column_index("pdbx_I_minus_sigma")};
  Intensities intensities;
  intensities.copy_metadata(rb);
  intensities.wavelength = rb.wavelength;
  ReflnDataProxy proxy(rb);
  for (size_t i = 0; i < proxy.size(); i += proxy.stride())
    for (int j = 0; j < 2; ++j) {
      Intensities::Refl refl;
      refl.hkl = proxy.get_hkl(i);
      refl.isign = (j == 0 ? 1 : -1);
      refl.value = proxy.get_num(i + value_idx[j]);
      refl.sigma = proxy.get_num(i + sigma_idx[j]);
      intensities.add_if_valid(refl);
    }
  return intensities;
}

inline Intensities read_unmerged_intensities_from_xds(const XdsAscii& xds) {
  Intensities intensities;
  intensities.unit_cell = xds.unit_cell;
  intensities.spacegroup = find_spacegroup_by_number(xds.spacegroup_number);
  intensities.wavelength = xds.wavelength;
  intensities.data.reserve(xds.data.size());
  for (const XdsAscii::Refl& in : xds.data) {
    Intensities::Refl refl;
    refl.hkl = in.hkl;
    refl.value = in.iobs;
    refl.sigma = in.sigma;
    intensities.add_if_valid(refl);
  }
  intensities.switch_to_asu_indices();
  return intensities;
}

} // namespace gemmi
#endif
