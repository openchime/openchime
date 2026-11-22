#!/usr/bin/env bash
# Build the H.264 encoder of the committed libavc (static) from third_party/libavc
# -- the v1.10.0 release as ittiam-systems published it, committed
# (docs/details/VENDORS.md §1) -- into third_party/libavc-<version>[-arch]. Nothing is
# downloaded.
#
#   scripts/build/libavc.sh            native (Linux; `make EXPORT=1` links it)
#
# Only the encoder and the common code it uses are compiled, as plain C (and, on
# aarch64, the encoder's own assembly), by the compiler make names. Upstream's
# CMake project is not used: it declares C++ for its tests and fuzzers, so
# configuring it needs a C++ compiler for a library that contains none.
#
# Each x86 source gets only the instruction set its name says it uses -- an
# *_ssse3.c file -mssse3, an *_sse42.c file -msse4.2, everything else none --
# rather than upstream's -msse4.2 -mavx2 on every file, which lets the compiler
# put AVX2 into the generic code and would fault on a CPU without it. The
# daemon picks the path at run time (daemon/export_engine.c). The common AVX2
# sources are not built: the encoder calls none of them.
#
# The daemon's export of a video message (ARCH-135) is the only user.
set -euo pipefail
unset MAKEFLAGS MFLAGS

LIBAVC_VERSION=1.10.0

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SRC="$(pwd)/libavc"
[ -f "${SRC}/encoder/ih264e_api.c" ] || { echo "build_libavc: third_party/libavc is missing" >&2; exit 1; }

DEST="libavc-${LIBAVC_VERSION}${OC_ARCH:+-$OC_ARCH}"   # OC_ARCH: another architecture, with make's CC
CC="${CC:-cc}"
AR="${AR:-ar}"
[ -n "${OC_ARCH:-}" ] && AR="${OC_ARCH}-linux-gnu-ar"
ARCH="${OC_ARCH:-$(uname -m)}"

if [ ! -f "${DEST}/lib/libavcenc.a" ]; then
  echo "build_libavc: building ${DEST} (encoder only, static, ${ARCH})"
  rm -rf "${DEST}"
  mkdir -p "${DEST}/build" "${DEST}/lib" "${DEST}/include"

  common=(ih264_buf_mgr ih264_cabac_tables ih264_cavlc_tables ih264_chroma_intra_pred_filters
          ih264_common_tables ih264_deblk_edge_filters ih264_deblk_tables ih264_disp_mgr
          ih264_dpb_mgr ih264_ihadamard_scaling ih264_inter_pred_filters ih264_iquant_itrans_recon
          ih264_list ih264_luma_intra_pred_filters ih264_mem_fns ih264_padding
          ih264_resi_trans_quant ih264_trans_data ih264_weighted_pred ithread)
  encoder=(ih264e_api ih264e_bitstream ih264e_cabac ih264e_cabac_encode ih264e_cabac_init
           ih264e_cavlc ih264e_core_coding ih264e_deblk ih264e_encode ih264e_encode_header
           ih264e_fmt_conv ih264e_function_selector_generic ih264e_globals ih264e_half_pel
           ih264e_intra_modes_eval ih264e_mc ih264e_me ih264e_modify_frm_rate ih264e_process
           ih264e_rate_control ih264e_rc_mem_interface ih264e_sei ih264e_time_stamp ih264e_utils
           ih264e_version ime ime_distortion_metrics irc_bit_allocation irc_cbr_buffer_control
           irc_est_sad irc_fixed_point_error_bits irc_frame_info_collector irc_mb_model_based
           irc_picture_type irc_rate_control_api irc_rd_model irc_vbr_storage_vbv irc_vbr_str_prms psnr)

  files=()
  for f in "${common[@]}"; do files+=("common/$f.c"); done
  for f in "${encoder[@]}"; do files+=("encoder/$f.c"); done
  inc=(-I"${SRC}/common" -I"${SRC}/common/mvc" -I"${SRC}/encoder")
  march=()
  case "${ARCH}" in
    x86_64)
      x86_common=(ih264_chroma_intra_pred_filters_ssse3 ih264_deblk_chroma_ssse3 ih264_deblk_luma_ssse3
                  ih264_ihadamard_scaling_sse42 ih264_ihadamard_scaling_ssse3 ih264_inter_pred_filters_ssse3
                  ih264_iquant_itrans_recon_dc_ssse3 ih264_iquant_itrans_recon_sse42 ih264_iquant_itrans_recon_ssse3
                  ih264_luma_intra_pred_filters_ssse3 ih264_mem_fns_ssse3 ih264_padding_ssse3
                  ih264_resi_trans_quant_sse42 ih264_weighted_pred_sse42)
      x86_encoder=(ih264e_function_selector ih264e_function_selector_sse42 ih264e_function_selector_ssse3
                   ih264e_half_pel_ssse3 ih264e_intra_modes_eval_ssse3 ime_distortion_metrics_sse42)
      for f in "${x86_common[@]}"; do files+=("common/x86/$f.c"); done
      for f in "${x86_encoder[@]}"; do files+=("encoder/x86/$f.c"); done
      inc+=(-I"${SRC}/common/x86" -I"${SRC}/encoder/x86")
      ;;
    aarch64)
      for f in "${SRC}"/common/armv8/*.s; do files+=("common/armv8/$(basename "$f")"); done
      arm_selectors=(ih264e_function_selector ih264e_function_selector_a9q ih264e_function_selector_av8)
      arm_asm=(ih264e_evaluate_intra16x16_modes_av8 ih264e_evaluate_intra_chroma_modes_av8
               ih264e_half_pel_av8 ime_distortion_metrics_av8)
      for f in "${arm_selectors[@]}"; do files+=("encoder/arm/$f.c"); done
      for f in "${arm_asm[@]}"; do files+=("encoder/armv8/$f.s"); done
      inc+=(-I"${SRC}/common/arm" -I"${SRC}/common/armv8" -I"${SRC}/encoder/arm" -I"${SRC}/encoder/armv8")
      march=(-march=armv8-a)
      ;;
    *) echo "build_libavc: no build for ${ARCH}" >&2; exit 2 ;;
  esac

  objs=()
  for f in "${files[@]}"; do
    o="${DEST}/build/$(echo "$f" | tr '/' '_').o"
    flags=()
    case "$f" in
      *_ssse3.c) flags=(-mssse3) ;;
      *_sse42.c) flags=(-msse4.2) ;;
    esac
    # Upstream's code, compiled as released: its own warnings are not ours to fix.
    "${CC}" -std=gnu99 -O2 -fPIC -w -DN_MB_ENABLE -D_GNU_SOURCE "${march[@]}" "${flags[@]}" "${inc[@]}" \
      -c "${SRC}/$f" -o "$o"
    objs+=("$o")
  done
  "${AR}" rcs "${DEST}/lib/libavcenc.a" "${objs[@]}"
  cp "${SRC}/common/ih264_typedefs.h" "${SRC}/encoder/ih264e.h" "${SRC}/encoder/iv2.h" "${SRC}/encoder/ive2.h" "${DEST}/include/"
  rm -rf "${DEST}/build"
fi

echo "build_libavc: done: third_party/${DEST}/lib/libavcenc.a, headers in third_party/${DEST}/include"
