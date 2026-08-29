#pragma once

// enc_cpd: quadrature counts per encoder detent (CONFIG.JSN settings.encres);
// 2 = prototype half-cycle encoder (default), 4 = standard EC11 full-cycle
void initGPIO(xQueueHandle, int enc_cpd);
void gpioSetEncoderResolution(int enc_cpd);   // live re-apply (POST /settings)

