// pico-amiga — the right pane's "Description" block (UiDesc.h). Nothing to say yet:
// every option draws no block.
#include "UiDesc.h"

namespace nm {

int describe(const Node& n, int32_t value, DescLine* out, int max) {
    (void)n; (void)value; (void)out; (void)max;
    return 0;
}

}  // namespace nm
