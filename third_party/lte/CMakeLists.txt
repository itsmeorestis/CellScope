cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# CellScope LTE stack: srsRAN2 (phy/mac/asn1) + FALCON + LTESniffer decode core,
# vendored and ported to MinGW/Windows. Builds standalone (spike) or as a
# subdirectory of the main CellScope build (aggregate target: cellscope_lte).
# ---------------------------------------------------------------------------

set(CS_LTE_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CS_LTE_STANDALONE ON)
  project(cellscope_lte C CXX)
endif()

# srsRAN/FALCON are C11/C++14; keep this in the subdirectory scope only.
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SRSRAN_DIR "${CS_ROOT}/srsran")
set(FALCON_DIR "${CS_ROOT}/falcon")
set(LTESNIFFER_DIR "${CS_ROOT}/ltesniffer")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

# NOTE: every setting below is directory-scoped — it applies only to the
# vendored LTE libraries, never to CellScope's own application code.
#
# Silence vendored-code warnings + strip srsRAN's dllimport decoration (static).
add_compile_options(-Wno-all -w)
add_definitions(-D_GNU_SOURCE -DSRSRAN_STATIC)
# Downlink-only build: guards LTESniffer's uplink paths (PUSCH/enb_ul) so we
# don't need to compile the uplink + srsran_rlc + phy/enb cascade.
add_definitions(-DCELLSCOPE_DL_ONLY)
# Force-include a central MinGW compat shim into every vendored TU.
add_compile_options(-include "${CMAKE_CURRENT_SOURCE_DIR}/compat/cs_compat.h")
# Make the POSIX-compat shim headers (syslog.h, sys/socket.h, pwd.h, ...) visible.
include_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/compat")

# ---------------------------------------------------------------------------
# srsran_phy
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SRSRAN_PHY_SRCS "${SRSRAN_DIR}/phy/*.c" "${SRSRAN_DIR}/phy/*.cpp")
list(FILTER SRSRAN_PHY_SRCS EXCLUDE REGEX "/(rf|test|enb|gnb)/")
list(FILTER SRSRAN_PHY_SRCS EXCLUDE REGEX "(netsink|netsource)")
# Drop any files that carry their own main()
set(_filtered "")
foreach(f ${SRSRAN_PHY_SRCS})
  file(READ "${f}" _c)
  if(NOT _c MATCHES "\n[ \t]*int[ \t]+main[ \t]*\\(")
    list(APPEND _filtered "${f}")
  endif()
endforeach()
set(SRSRAN_PHY_SRCS ${_filtered})

add_library(srsran_phy STATIC ${SRSRAN_PHY_SRCS})
target_include_directories(srsran_phy PUBLIC
  "${SRSRAN_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  ${FFTW3F_INCLUDE_DIRS})
target_link_libraries(srsran_phy PUBLIC ${FFTW3F_LIBRARIES} winpthread m)
target_compile_options(srsran_phy PRIVATE -march=native)

# ---------------------------------------------------------------------------
# falcon_phy (FALCON PHY + UE + util + prof + common)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE FALCON_SRCS
  "${FALCON_DIR}/src/phy/falcon_phch/*.c"
  "${FALCON_DIR}/src/phy/falcon_phch/*.cc"
  "${FALCON_DIR}/src/phy/falcon_ue/*.c"
  "${FALCON_DIR}/src/util/*.c"
  "${FALCON_DIR}/src/util/*.cc"
  "${FALCON_DIR}/src/prof/*.cc"
  "${FALCON_DIR}/src/common/*.cc")
list(FILTER FALCON_SRCS EXCLUDE REGEX "\\.orig$")
# DCIFormats.cc includes a header that does not ship and its body is fully
# commented-out legacy srslte_* code -> dead translation unit.
list(FILTER FALCON_SRCS EXCLUDE REGEX "DCIFormats")
# SignalManager.cc is LTESniffer's CTRL+C handler (POSIX signals); unused here.
list(FILTER FALCON_SRCS EXCLUDE REGEX "SignalManager")

add_library(falcon_phy STATIC ${FALCON_SRCS})
target_include_directories(falcon_phy PUBLIC
  "${FALCON_DIR}/include"
  "${SRSRAN_DIR}/include"
  ${FFTW3F_INCLUDE_DIRS})
target_link_libraries(falcon_phy PUBLIC srsran_phy)
target_compile_options(falcon_phy PRIVATE -march=native)

# ---------------------------------------------------------------------------
# srslog (srsRAN logging framework; self-contained C++)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SRSLOG_SRCS "${SRSRAN_DIR}/srslog/*.cpp" "${SRSRAN_DIR}/srslog/*.cc")
list(FILTER SRSLOG_SRCS EXCLUDE REGEX "/test/|_test\\.")
add_library(srslog STATIC ${SRSLOG_SRCS})
target_include_directories(srslog PUBLIC
  "${SRSRAN_DIR}/include"
  "${SRSRAN_DIR}/include/srsran/srslog/bundled")

# ---------------------------------------------------------------------------
# srsran_common (minimal: byte buffers, threads, MAC pcap. Excludes NR, crypto,
# networking and core-network pcap variants we do not need.)
# ---------------------------------------------------------------------------
set(SRSRAN_COMMON_SRCS
  "${SRSRAN_DIR}/common/byte_buffer.cc"
  "${SRSRAN_DIR}/common/buffer_pool.cc"
  "${SRSRAN_DIR}/common/pcap.c"
  "${SRSRAN_DIR}/common/mac_pcap.cc"
  "${SRSRAN_DIR}/common/mac_pcap_base.cc"
  "${SRSRAN_DIR}/common/threads.c"
  "${SRSRAN_DIR}/common/thread_pool.cc"
  "${SRSRAN_DIR}/common/standard_streams.cc"
  "${SRSRAN_DIR}/common/version.c"
  "${SRSRAN_DIR}/common/backtrace.c"
  "${SRSRAN_DIR}/common/time_prof.cc"
  "${SRSRAN_DIR}/common/tti_sync_cv.cc"
  "${SRSRAN_DIR}/common/rrc_common.cc"
  "${SRSRAN_DIR}/support/emergency_handlers.cc")
add_library(srsran_common STATIC ${SRSRAN_COMMON_SRCS})
target_include_directories(srsran_common PUBLIC
  "${SRSRAN_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_link_libraries(srsran_common PUBLIC srslog srsran_phy winpthread)

# ---------------------------------------------------------------------------
# srsran_mac (LTE MAC PDU parsing -> human-readable MAC. Excludes NR.)
# ---------------------------------------------------------------------------
file(GLOB SRSRAN_MAC_SRCS "${SRSRAN_DIR}/mac/*.cc")
list(FILTER SRSRAN_MAC_SRCS EXCLUDE REGEX "_nr\\.")
add_library(srsran_mac STATIC ${SRSRAN_MAC_SRCS})
target_include_directories(srsran_mac PUBLIC "${SRSRAN_DIR}/include")
target_link_libraries(srsran_mac PUBLIC srsran_common srslog)

# ---------------------------------------------------------------------------
# srsRAN ASN.1 (LTE RRC + NAS only; exclude NR, NB-IoT, S1AP, NGAP, 5G)
# ---------------------------------------------------------------------------
set(SRSRAN_ASN1_DIR "${SRSRAN_DIR}/asn1")
set(ASN1_WARN_FLAGS -fno-exceptions -Wno-switch -Wno-unused-variable -Wno-return-type -Wno-sign-compare -Wno-reorder -Wno-parentheses)

# asn1_utils — base ASN.1 helpers
add_library(asn1_utils STATIC "${SRSRAN_ASN1_DIR}/asn1_utils.cc")
target_include_directories(asn1_utils PUBLIC "${SRSRAN_DIR}/include")
target_link_libraries(asn1_utils PUBLIC srsran_common)
target_compile_options(asn1_utils PRIVATE ${ASN1_WARN_FLAGS})

# srsran_asn1 — LTE NAS (liblte_mme) + common (gtpc.cc dropped: GTP-C core
# network, needs IPv6 types and is unused for air-interface decode)
add_library(srsran_asn1 STATIC
  "${SRSRAN_ASN1_DIR}/liblte_common.cc"
  "${SRSRAN_ASN1_DIR}/liblte_mme.cc")
target_include_directories(srsran_asn1 PUBLIC "${SRSRAN_DIR}/include")
target_link_libraries(srsran_asn1 PUBLIC asn1_utils srsran_common)
target_compile_options(srsran_asn1 PRIVATE ${ASN1_WARN_FLAGS})

# rrc_asn1 — LTE RRC (exclude NR / NR-utils; rrc_utils.cc pulls srsenb/*)
file(GLOB RRC_ASN1_BASE "${SRSRAN_ASN1_DIR}/rrc*.cc")
file(GLOB_RECURSE RRC_ASN1_SUB "${SRSRAN_ASN1_DIR}/rrc/*.cc")
set(RRC_ASN1_SRCS ${RRC_ASN1_BASE} ${RRC_ASN1_SUB})
list(FILTER RRC_ASN1_SRCS EXCLUDE REGEX "rrc_nr")
list(FILTER RRC_ASN1_SRCS EXCLUDE REGEX "rrc_utils")
add_library(rrc_asn1 STATIC ${RRC_ASN1_SRCS})
target_include_directories(rrc_asn1 PUBLIC "${SRSRAN_DIR}/include")
target_link_libraries(rrc_asn1 PUBLIC asn1_utils srsran_common)
target_compile_options(rrc_asn1 PRIVATE -Os ${ASN1_WARN_FLAGS})

# ---------------------------------------------------------------------------
# ltesniffer decode core (downlink subset). Excludes uplink (UL_Sniffer_PUSCH,
# ULSchedule), the CLI (ArgManager) and the RF/threading orchestrator
# (LTESniffer_Core, which pulls in srsue/hdr/ue.h). CellScope drives it.
# ---------------------------------------------------------------------------
file(GLOB LTESNIFFER_SRCS "${LTESNIFFER_DIR}/src/*.cc" "${LTESNIFFER_DIR}/src/*.c")
list(FILTER LTESNIFFER_SRCS EXCLUDE REGEX "(UL_Sniffer_PUSCH|ArgManager|LTESniffer_Core)")
add_library(ltesniffer_core STATIC ${LTESNIFFER_SRCS})
target_include_directories(ltesniffer_core PUBLIC
  "${LTESNIFFER_DIR}/include"
  "${LTESNIFFER_DIR}"
  "${FALCON_DIR}/include"
  "${SRSRAN_DIR}/include"
  ${FFTW3F_INCLUDE_DIRS})
target_link_libraries(ltesniffer_core PUBLIC
  falcon_phy srsran_phy srsran_mac srsran_common rrc_asn1 srsran_asn1 asn1_utils srslog)
target_compile_options(ltesniffer_core PRIVATE -march=native)

# ---------------------------------------------------------------------------
# CellScope LTE engine bridge. Compiled HERE (not in the CellScope target) so
# it inherits the MinGW compat shims and C++14 while it includes srsRAN /
# LTESniffer headers. It exposes only src/lte/lte_engine.h (STL/POD types) to
# the rest of the application.
# ---------------------------------------------------------------------------
add_library(cellscope_lte_engine STATIC "${CMAKE_SOURCE_DIR}/src/lte/lte_engine.cpp")
target_include_directories(cellscope_lte_engine PUBLIC "${CMAKE_SOURCE_DIR}/src/lte")
target_link_libraries(cellscope_lte_engine PUBLIC ltesniffer_core)
target_compile_options(cellscope_lte_engine PRIVATE -march=native)

# Aggregate target the main build links against.
add_library(cellscope_lte INTERFACE)
target_link_libraries(cellscope_lte INTERFACE cellscope_lte_engine)
target_include_directories(cellscope_lte INTERFACE "${CMAKE_SOURCE_DIR}/src/lte")

# ---------------------------------------------------------------------------
# Standalone Phase-0 gate harness (only when configured on its own).
# ---------------------------------------------------------------------------
if(CS_LTE_STANDALONE)
  add_executable(lte_spike "${CMAKE_CURRENT_SOURCE_DIR}/spike_main.cc")
  target_include_directories(lte_spike PRIVATE
    "${FALCON_DIR}/include" "${SRSRAN_DIR}/include"
    "${LTESNIFFER_DIR}/include" "${LTESNIFFER_DIR}")
  target_link_libraries(lte_spike PRIVATE ltesniffer_core falcon_phy srsran_phy)
endif()
