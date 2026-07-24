#ifndef HFSDR_FREERTOS_PORT_ISR_H
#define HFSDR_FREERTOS_PORT_ISR_H

/*
 * Returning maskable interrupt vectors are assembly wrappers in
 * port_ch32v30x.S.  Their ordinary C bodies use the platform ABI and return
 * with ret; the wrapper owns the dedicated IRQ stack and the final mret.
 *
 * used and externally_visible keep the body available to the assembly-only
 * reference when link-time optimisation is enabled.  noinline keeps the
 * compiler-emitted stack-usage record attributable to the ISR body.
 */
#define PORT_ISR_BODY(vector_name)                                      \
    void vector_name##_Body(void)                                       \
        __attribute__((used, noinline, externally_visible));            \
    void vector_name##_Body(void)

#endif
