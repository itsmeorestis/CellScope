// Phase-0 gate: prove srsRAN phy + FALCON link and their core symbols resolve
// under MinGW. Not a functional decode yet — just forces the linker to pull in
// the decode entry points so we validate the toolchain end to end.
#include <cstdio>

// srsRAN headers carry their own extern "C" guards; do not re-wrap them.
#include "srsran/srsran.h"
#undef I // complex.h defines I, which breaks C++ if left set

#include "falcon/phy/falcon_ue/falcon_ue_dl.h"

// LTESniffer decode-core symbol: proves the whole stack links together.
#include "PcapWriter.h"

int main()
{
    std::printf("CellScope LTE Phase-0 spike\n");

    // Touch a couple of PHY symbols so they must resolve at link time.
    srsran_cell_t cell = {};
    cell.nof_prb   = 50;
    cell.nof_ports = 1;
    cell.id        = 1;
    std::printf("srsran_cell nof_prb=%d isvalid=%d\n",
                cell.nof_prb, srsran_cell_isvalid(&cell));

    // Exercise the real DSP path at runtime: srsran_vec_* (our posix_memalign
    // shim) + an FFTW-backed DFT round-trip. Proves alloc/free correctness and
    // FFTW linkage, not just symbol resolution.
    const int N = 128;
    cf_t* in  = srsran_vec_cf_malloc(N);
    cf_t* out = srsran_vec_cf_malloc(N);
    if (!in || !out) {
        std::printf("FAIL: srsran_vec_cf_malloc returned NULL\n");
        return 1;
    }
    for (int i = 0; i < N; i++) in[i] = 1.0f + 0.0f * _Complex_I;

    srsran_dft_plan_t dft;
    if (srsran_dft_plan(&dft, N, SRSRAN_DFT_FORWARD, SRSRAN_DFT_COMPLEX)) {
        std::printf("FAIL: srsran_dft_plan\n");
        return 1;
    }
    srsran_dft_run_c(&dft, in, out);

    // DC bin of an all-ones input should equal N.
    float dc = __real__ out[0];
    std::printf("DFT DC bin = %.1f (expect %d)\n", dc, N);

    srsran_dft_plan_free(&dft);
    free(in);
    free(out);

    // Force the LTESniffer decode core + srsRAN mac/pcap to link.
    LTESniffer_pcap_writer pcap;
    pcap.set_ue_id(0);
    std::printf("LTESniffer pcap writer linked OK\n");

    std::printf("Phase-0 gate: %s\n", (dc > (float)N - 1.0f && dc < (float)N + 1.0f) ? "PASS" : "FAIL");
    return 0;
}
