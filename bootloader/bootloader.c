
#include "main.h"
#include "quadspi.h"
typedef void(*pFunction)(void);
pFunction JumpToApp;
#define APP_ADDRESS 0x90000000

void BootSwitchToExtFlash(void){

  SCB_DisableICache();
//  SCB_DisableDCache();
  RCC->D1CCIPR&=~RCC_D1CCIPR_CKPERSEL;
  RCC->D1CCIPR|=RCC_D1CCIPR_QSPISEL;
  CSP_QSPI_EnableMemoryMappedMode();
  __DSB();
  __ISB();
  for (int i = 0; i < 8; i++) {
      NVIC->ICER[i] = 0xFFFFFFFF;
      NVIC->ICPR[i] = 0xFFFFFFFF;
  }
  __disable_irq();
  SysTick->CTRL=0;
  SCB->VTOR = APP_ADDRESS;
  JumpToApp=(pFunction)(*(__IO uint32_t*)(APP_ADDRESS+4));
  __set_MSP(*(__IO uint32_t*)(APP_ADDRESS));
  JumpToApp();
}
